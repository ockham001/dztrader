#ifndef DZTRADER_SYSMON_TESTS_CLOCK_FAKE_TRANSPORT_H_
#define DZTRADER_SYSMON_TESTS_CLOCK_FAKE_TRANSPORT_H_

/**
 * @file clock_fake_transport.h
 * @brief 时钟探测进程级 e2e 的假 NTP 传输（#299 / #301）：脚本化、不碰真实网络。
 *
 * 只被测试专用可执行文件 `clock_e2e_runner.cpp` 使用（真实 `run()` + 假传输），因此单用途、
 * 无需登记为共享测试头。行为按配置里的 host 名脚本化：
 * - `fake-ok`         —— 成功响应：服务端时间比本机快 50ms，`stratum=2`，`root_dispersion=1ms`
 * - `fake-ok-slow`    —— 成功响应，但延迟 2.5s 才回调（用于观察「探测不阻塞采集」）
 * - `fake-offset-N`   —— 成功响应，服务端时间比本机快 N 毫秒（`fake-offset-1500` → 偏差 −1500ms）。
 *                        #301 用它脚本化跨越 warn / error 两个阈值档的确定性偏差；N 取整数，
 *                        负号用 `fake-offset-negN`（`fake-offset-neg1500` → 偏差 +1500ms）。
 * - `fake-dispersion-N` —— 成功响应，偏差为 0 但 `root_dispersion = N` 毫秒（观察误差界质量判据）。
 * - `fake-timeout`    —— 永不响应（超时）
 * - 其它（含真实主机名）—— 一律按超时处理：**e2e 绝不能因为配置里的真实主机名而访问外网**
 *
 * 响应报文用真实 NTPv4 布局（48 字节、mode 4、origin 回显请求的 transmit timestamp），
 * 因此被测的编解码/派生计算走的是与生产完全相同的路径；只有 socket 被替换成
 * `post` / `steady_timer`（后者用于慢响应场景）。
 *
 * `offset_ms` 与 `bound_ms` 的关系：`offset ≈ −ahead`（服务端快 = 本地慢 = 负偏差），
 * `bound_ms = ½·RTT + root_dispersion`。RTT 由假传输经 `post` 投递的**异步回调**决定——
 * 实测约几十微秒（不是精确的 0），因此 `bound_ms` 只是**近似等于** `root_dispersion`。
 * `fake-dispersion-N` 的取值因此刻意远离阈值（如 900ms vs 200ms），不依赖那点抖动落在
 * 阈值哪一侧。
 */

#include "clock_probe.h"
#include "ntp_codec.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dztrader::sysmon::test {

/// 假服务端时间相对本机的领先量（`fake-ok` 的确定性偏差来源）。
inline constexpr int64_t kFakeServerAheadNs = 50'000'000;  // +50ms
/// `fake-ok-slow` 的响应延迟：刻意长于两个推送周期（推送间隔 1s），使 e2e 能观察到
/// **两帧**「探测在飞、计量样本仍在推进」的快照（异步探测不阻塞采集与帧处理）。
inline constexpr std::chrono::milliseconds kFakeSlowDelay{2500};
/// 响应报文的 `root_dispersion`：1ms（换算为 16.16 定点秒，见 `make_fake_response`）。
inline constexpr int64_t kFakeRootDispersionNs = 1'000'000;
/// 响应报文的参考源 stratum（[1,15] 内的一个普通值）。
inline constexpr uint8_t kFakeStratum = 2;

/// 写大端 32 位整数（NTP 时间戳字段布局）。
inline void write_be32(std::array<uint8_t, ntp::kPacketSize>& packet,
                       std::size_t offset,
                       uint32_t value) {
    packet[offset] = static_cast<uint8_t>((value >> 24) & 0xFFU);
    packet[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    packet[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    packet[offset + 3] = static_cast<uint8_t>(value & 0xFFU);
}

/// 读大端 32 位整数。
inline uint32_t read_be32(const std::array<uint8_t, ntp::kPacketSize>& packet, std::size_t offset) {
    return (static_cast<uint32_t>(packet[offset]) << 24) |
           (static_cast<uint32_t>(packet[offset + 1]) << 16) |
           (static_cast<uint32_t>(packet[offset + 2]) << 8) |
           static_cast<uint32_t>(packet[offset + 3]);
}

/// 把 NTP 时间戳换算为 Unix 纳秒（本模块只处理 1970 年之后的时刻）。
inline int64_t ntp_to_unix_nanoseconds(const std::array<uint8_t, ntp::kPacketSize>& packet,
                                       std::size_t offset) {
    const int64_t seconds = static_cast<int64_t>(read_be32(packet, offset));
    return (seconds - ntp::kUnixEpochOffsetSec) * 1'000'000'000LL +
           ntp::fraction_to_nanoseconds(read_be32(packet, offset + 4));
}

/// 构造一份「服务端时间 = 请求 T1 + ahead_ns、root_dispersion = dispersion_ns」的成功响应。
inline std::vector<uint8_t> make_fake_response(const std::array<uint8_t, ntp::kPacketSize>& request,
                                               int64_t ahead_ns,
                                               int64_t dispersion_ns = kFakeRootDispersionNs) {
    std::array<uint8_t, ntp::kPacketSize> response{};
    response[0] = static_cast<uint8_t>((0U << 6) | (ntp::kClientVersion << 3) |
                                       ntp::kModeServer);  // LI=0 / VN=4 / Mode=4
    response[1] = kFakeStratum;
    response[2] = 0;             // poll
    response[3] = 0;             // precision
    write_be32(response, 4, 0);  // root delay
    // root dispersion 是 16.16 定点秒（无符号）：raw = ns × 2^16 / 10^9，四舍五入
    // （与生产编解码 `fixed_point_nanoseconds` 的 round_div 同向，避免 1ms 量级的截断偏差）。
    const int64_t raw = (dispersion_ns * (1LL << 16) + 500'000'000LL) / 1'000'000'000LL;
    write_be32(response, 8, static_cast<uint32_t>(raw));
    write_be32(response, 12, 0x4C4F434CU);  // reference id "LOCL"
    // 16..23：reference timestamp（0）
    // 24..31：origin timestamp = 请求的 transmit timestamp（回显校验）
    for (std::size_t i = 0; i < 8; ++i) {
        response[24 + i] = request[40 + i];
    }
    const auto server_timestamp =
        ntp::from_unix_nanoseconds(ntp_to_unix_nanoseconds(request, 40) + ahead_ns);
    // 32..39：receive timestamp（T2）；40..47：transmit timestamp（T3）。
    // 假服务端处理时间取 0：T2 = T3 = T1 + ahead。
    write_be32(response, 32, server_timestamp.seconds);
    write_be32(response, 36, server_timestamp.fraction);
    write_be32(response, 40, server_timestamp.seconds);
    write_be32(response, 44, server_timestamp.fraction);
    return std::vector<uint8_t>(response.begin(), response.end());
}

/**
 * @brief 脚本化假传输：所有回调都经 `post` 异步投递（不阻塞调用线程、不访问网络）。
 *
 * 单线程模型：与生产传输一致，只在 io_context 线程使用。
 */
class FakeNtpTransport final : public NtpTransport {
public:
    explicit FakeNtpTransport(boost::asio::io_context& io)
        : io_(io) {}

    void async_exchange(std::string host,
                        std::array<uint8_t, ntp::kPacketSize> request,
                        int32_t /*timeout_ms*/,
                        Handler handler) override {
        const Script script = parse_script(host);
        if (!script.recognised) {
            // 未脚本化的 host（含真实主机名）按超时处理：e2e 不得访问外网。
            boost::asio::post(io_, [handler = std::move(handler)]() mutable {
                NtpExchange result;
                result.status = NtpExchange::Status::Timeout;
                handler(std::move(result));
            });
            return;
        }
        // 真实 NTPv4 响应报文：ahead 决定偏差、dispersion 决定误差界。
        auto response = make_fake_response(request, script.ahead_ns, script.dispersion_ns);
        if (script.slow) {
            // 用真实 asio 定时器延迟回调：探测在飞期间计量/推送定时器必须照常运行。
            auto timer = std::make_shared<boost::asio::steady_timer>(io_);
            timer->expires_after(kFakeSlowDelay);
            timer->async_wait([timer, handler = std::move(handler), response = std::move(response)](
                                  const boost::system::error_code& ec) mutable {
                if (ec) {
                    return;  // 进程退出时取消
                }
                NtpExchange result;
                result.status = NtpExchange::Status::Received;
                result.packet = std::move(response);
                handler(std::move(result));
            });
            return;
        }
        boost::asio::post(io_,
                          [handler = std::move(handler), response = std::move(response)]() mutable {
                              NtpExchange result;
                              result.status = NtpExchange::Status::Received;
                              result.packet = std::move(response);
                              handler(std::move(result));
                          });
    }

private:
    /// 一个 host 名脚本化出的响应参数。
    struct Script {
        bool recognised = false;  ///< false = 未脚本化（按超时处理，绝不回落到真实网络）
        bool slow = false;        ///< 是否延迟回调（观察「探测不阻塞采集」）
        /// 服务端相对本机的领先量（纳秒）。正值 → 服务端快 → 本地慢 → **负偏差**。
        int64_t ahead_ns = kFakeServerAheadNs;
        int64_t dispersion_ns = kFakeRootDispersionNs;
    };

    /// 把 host 名解析为响应参数（纯函数，便于按名字表推演）。
    ///
    /// 支持 `fake-offset-N` / `fake-offset-negN` / `fake-dispersion-N`：N 为毫秒整数。
    static Script parse_script(const std::string& host) {
        Script script;
        if (host == "fake-ok") {
            script.recognised = true;
            return script;
        }
        if (host == "fake-ok-slow") {
            script.recognised = true;
            script.slow = true;
            return script;
        }
        if (const auto millis = parse_suffix(host, "fake-offset-")) {
            // `fake-offset-1500` → 本机慢 1500ms（偏差 −1500）；`fake-offset-neg1500` → 本地快。
            script.recognised = true;
            script.ahead_ns = millis->value * 1'000'000;
            if (millis->negative) {
                script.ahead_ns = -script.ahead_ns;
            }
            return script;
        }
        if (const auto millis = parse_suffix(host, "fake-dispersion-")) {
            script.recognised = true;
            script.ahead_ns = 0;  // 偏差为 0：只让误差界单独越界
            script.dispersion_ns = millis->value * 1'000'000;
            return script;
        }
        return script;  // 未脚本化（含 fake-timeout）：调用方按超时处理
    }

    struct SuffixValue {
        int64_t value = 0;
        bool negative = false;
    };

    /// 解析 `<prefix>[neg]<digits>`；不匹配返回 nullopt。
    static std::optional<SuffixValue> parse_suffix(const std::string& host,
                                                   std::string_view prefix) {
        if (!host.starts_with(prefix)) {
            return std::nullopt;
        }
        std::string_view rest = std::string_view(host).substr(prefix.size());
        SuffixValue parsed;
        if (rest.starts_with("neg")) {
            parsed.negative = true;
            rest.remove_prefix(3);
        }
        if (rest.empty()) {
            return std::nullopt;
        }
        int64_t value = 0;
        for (const char ch : rest) {
            if (ch < '0' || ch > '9') {
                return std::nullopt;  // 非数字：不是本函数的脚本化形态
            }
            value = value * 10 + (ch - '0');
        }
        parsed.value = value;
        return parsed;
    }

    boost::asio::io_context& io_;
};

}  // namespace dztrader::sysmon::test

#endif  // DZTRADER_SYSMON_TESTS_CLOCK_FAKE_TRANSPORT_H_
