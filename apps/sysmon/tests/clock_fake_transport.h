#ifndef DZTRADER_SYSMON_TESTS_CLOCK_FAKE_TRANSPORT_H_
#define DZTRADER_SYSMON_TESTS_CLOCK_FAKE_TRANSPORT_H_

/**
 * @file clock_fake_transport.h
 * @brief 时钟探测进程级 e2e 的假 NTP 传输（#299）：脚本化、不碰真实网络。
 *
 * 只被测试专用可执行文件 `clock_e2e_runner.cpp` 使用（真实 `run()` + 假传输），因此单用途、
 * 无需登记为共享测试头。行为按配置里的 host 名脚本化：
 * - `fake-ok`         —— 成功响应：服务端时间比本机快 50ms，`stratum=2`，`root_dispersion=1ms`
 * - `fake-ok-slow`    —— 成功响应，但延迟 2.5s 才回调（用于观察「探测不阻塞采集」）
 * - `fake-timeout`    —— 永不响应（超时）
 * - 其它（含真实主机名）—— 一律按超时处理：**e2e 绝不能因为配置里的真实主机名而访问外网**
 *
 * 响应报文用真实 NTPv4 布局（48 字节、mode 4、origin 回显请求的 transmit timestamp），
 * 因此被测的编解码/派生计算走的是与生产完全相同的路径；只有 socket 被替换成
 * `post` / `steady_timer`（后者用于慢响应场景）。
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
#include <string>
#include <utility>

namespace dztrader::sysmon::test {

/// 假服务端时间相对本机的领先量（`fake-ok` 的确定性偏差来源）。
inline constexpr int64_t kFakeServerAheadNs = 50'000'000;  // +50ms
/// `fake-ok-slow` 的响应延迟：刻意长于两个推送周期（推送间隔 1s），使 e2e 能观察到
/// **两帧**「探测在飞、计量样本仍在推进」的快照（异步探测不阻塞采集与帧处理）。
inline constexpr std::chrono::milliseconds kFakeSlowDelay{2500};
/// 响应报文的 `root_dispersion`（16.16 定点秒，1ms ≈ 66）。
inline constexpr uint16_t kFakeRootDispersionFixed = 66;
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

/// 构造一份「服务端时间 = 请求 T1 + ahead_ns」的成功响应。
inline std::vector<uint8_t> make_fake_response(const std::array<uint8_t, ntp::kPacketSize>& request,
                                               int64_t ahead_ns) {
    std::array<uint8_t, ntp::kPacketSize> response{};
    response[0] = static_cast<uint8_t>((0U << 6) | (ntp::kClientVersion << 3) |
                                       ntp::kModeServer);  // LI=0 / VN=4 / Mode=4
    response[1] = kFakeStratum;
    response[2] = 0;                                    // poll
    response[3] = 0;                                    // precision
    write_be32(response, 4, 0);                         // root delay
    write_be32(response, 8, kFakeRootDispersionFixed);  // root dispersion
    write_be32(response, 12, 0x4C4F434CU);              // reference id "LOCL"
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
        if (host == "fake-timeout" || !is_scripted(host)) {
            // 未脚本化的 host（含真实主机名）按超时处理：e2e 不得访问外网。
            boost::asio::post(io_, [handler = std::move(handler)]() mutable {
                NtpExchange result;
                result.status = NtpExchange::Status::Timeout;
                handler(std::move(result));
            });
            return;
        }
        // fake-ok / fake-ok-slow：真实 NTPv4 响应报文，仅回调时机不同。
        auto response = make_fake_response(request, kFakeServerAheadNs);
        if (host == "fake-ok-slow") {
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
    /// 是否是本替身认识的脚本化 host（其余一律按超时，绝不回落到真实网络）。
    static bool is_scripted(const std::string& host) {
        return host == "fake-ok" || host == "fake-ok-slow" || host == "fake-timeout";
    }

    boost::asio::io_context& io_;
};

}  // namespace dztrader::sysmon::test

#endif  // DZTRADER_SYSMON_TESTS_CLOCK_FAKE_TRANSPORT_H_
