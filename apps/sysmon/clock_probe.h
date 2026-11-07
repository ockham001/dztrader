#ifndef DZTRADER_SYSMON_CLOCK_PROBE_H_
#define DZTRADER_SYSMON_CLOCK_PROBE_H_

/**
 * @file clock_probe.h
 * @brief 一轮时钟探测的编排：「每台服务器各发一次」+ 结果选择 + KoD 状态 + 手动限流判定
 *        （契约 sysmon §1.1、§2.7、§2.9）。
 *
 * **本 spec 唯一的注入接缝**：传输是抽象接口 `NtpTransport`，测试注入假实现，全程不碰真实
 * 网络（CI 无法依赖外网 NTP：2026-11-04 实测 8 台候选服务器有 4 台全部超时）。真实 Asio/UDP
 * 实现与定时调度由 #299 交付；本文件不含任何 socket 代码。
 *
 * 编排语义（全部来自契约 §2.7「时钟偏差的计算语义」与 §2.9）：
 * - 每轮对配置的**每台**服务器各发一次；单台超时/失败不影响其它台
 * - 聚合值取**最小 RTT** 的那台（同 RTT 取 `bound_ms` 更小者），同时保留本轮全部明细
 *   （含每台的失败原因 `timeout` / `kod_rate` / `kod_deny` / `bad_echo` / `unreachable`）
 * - 全部失败时聚合值为**不可用**（`nullopt`），不得用 0 或上一轮结果冒充
 * - KoD：`RATE` 对该服务器退避、`DENY`/`RSTR` 拉黑，其余 KoD 忽略；退避与拉黑状态**跨轮保持**
 * - 被退避/拉黑跳过的服务器**仍出现在本轮明细**里（`ok=false` + 对应原因），明细因此恒与
 *   配置的服务器列表对齐
 *
 * 时钟读数也是注入的（`MomentSource`）：一次交互只在「发送前」与「收到后」两个时刻取读数，
 * 因此测试可以完全确定地断言偏差/往返/误差上界，不依赖真实时间。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <dztrader/platform/sysmon.h>

#include "ntp_codec.h"

namespace dztrader::sysmon {

/// 一次 UDP 交互的结果（契约 sysmon §2.7：单台失败不影响其它台）。
struct NtpExchange {
    enum class Status {
        Received,     ///< 收到响应字节（是否可用由报文层判定）
        Timeout,      ///< 超时未响应（`clock.probe_timeout_ms`）
        Unreachable,  ///< 解析/收发失败（DNS 失败、socket 错误等）
    };
    Status status = Status::Timeout;
    /// `Received` 时的响应字节；长度不足 48 时由报文层判为 bad_echo（截断包）
    std::vector<uint8_t> packet;
};

/**
 * @brief NTPv4 客户端模式的传输接缝（UDP/123，48 字节报文）。
 *
 * 实现契约：`async_exchange()` 不得阻塞调用线程（io_context 单线程，阻塞会拖住采集与帧处理），
 * 且**恰好回调 `handler` 一次**——`Timeout` 表示 `timeout_ms` 内没有响应。真实实现由 #299
 * 交付（Asio 异步 UDP）；测试注入假实现，因此本票内没有任何网络访问。
 */
class NtpTransport {
public:
    using Handler = std::function<void(NtpExchange)>;

    NtpTransport() = default;
    virtual ~NtpTransport() = default;
    NtpTransport(const NtpTransport&) = delete;
    NtpTransport& operator=(const NtpTransport&) = delete;
    NtpTransport(NtpTransport&&) = delete;
    NtpTransport& operator=(NtpTransport&&) = delete;

    /**
     * @brief 异步发送一个请求并等待响应。
     *
     * @param host        目标服务器（主机名或 IP，来自 `clock.servers`）
     * @param request     已编码的 48 字节请求（transmit timestamp 已填入我方 T1）
     * @param timeout_ms  单台超时（`clock.probe_timeout_ms`）
     * @param handler     结果回调；实现必须恰好调用一次（可同步调用，也可异步调用）
     */
    virtual void async_exchange(std::string host,
                                std::array<uint8_t, ntp::kPacketSize> request,
                                int32_t timeout_ms,
                                Handler handler) = 0;
};

/// 一次时刻读数：墙钟（`system_clock`，用于报文时间戳）与单调时钟（`steady_clock`，用于往返）。
///
/// 两者必须取自同一时刻：契约 §2.7 明确往返用单调时钟测量——用墙钟测往返时 NTP 正在调钟
/// 会让 RTT 为负。
struct Moment {
    int64_t wall_ns = 0;    ///< 自 Unix 纪元起的纳秒（墙钟口径）
    int64_t steady_ns = 0;  ///< 单调时钟纳秒（只用于求差，无绝对含义）
};

/// 时刻读数接缝（生产实现直接取两个 `std::chrono` 时钟；测试注入脚本化读数）。
using MomentSource = std::function<Moment()>;

/// 一轮探测的结果（契约 sysmon §2.9 `clock` 域中由探测产出的部分）。
///
/// 三个聚合字段**同生共死**：全部服务器失败时同时为「不可用」（`offset_ms` / `bound_ms` /
/// `source_stratum` 为 `nullopt`、`source` 为空串），绝不用 0 或上一轮结果冒充。
/// `daemons` / `daemon_conflict` / `kernel_synced` 由同步服务检测（#298）填充，不在本结构内。
struct ProbeOutcome {
    std::optional<double> offset_ms;         ///< 聚合有符号偏差（毫秒，正 = 本地快）
    std::optional<double> bound_ms;          ///< 聚合误差上界（毫秒）
    std::string source;                      ///< 聚合值采用的服务器标识；未成功时为空串
    std::optional<uint32_t> source_stratum;  ///< 参考源 stratum（[1,15]）；未成功时为空
    /// 本轮每台服务器明细（顺序与去重后的入参列表一致，含失败原因与退避/拉黑跳过）
    std::vector<platform::SysmonClockServerInfo> servers;
    int64_t finished_unix_ms = 0;  ///< 本轮**完成**时刻（Unix 毫秒；`last_probe_unix_ms` 的来源）
};

/// 手动触发限流判定的结果（契约 sysmon §1.1：dzsysmon 侧权威，拒绝只记日志、无错误通道）。
enum class ManualTriggerDecision {
    Allowed,        ///< 允许立即发起一轮探测
    ProbeInFlight,  ///< 已有探测在飞：拒绝（不排队、不叠加）
    RateLimited,    ///< 距上次触发不足 `manual_min_interval_sec`：拒绝
};

/**
 * @brief 手动触发限流判定（纯判定：不读时钟、不碰帧，两个时刻由调用方给出）。
 *
 * 判定顺序：**在飞优先于限流**——在飞是更根本的拒绝理由（此时无论间隔多长都不该发起第二轮），
 * 日志里应当看到真实原因而不是被限流掩盖。
 *
 * @param now_steady_ns          当前单调时钟读数
 * @param last_trigger_steady_ns 上次**受理**的触发时刻（单调）；`nullopt` = 从未触发
 * @param min_interval_sec       配置 `clock.manual_min_interval_sec`（0 = 不限流）
 * @param probe_in_flight        是否已有探测在飞
 */
[[nodiscard]] ManualTriggerDecision judge_manual_trigger(
    int64_t now_steady_ns,
    std::optional<int64_t> last_trigger_steady_ns,
    int32_t min_interval_sec,
    bool probe_in_flight) noexcept;

/// KoD `RATE` 的退避时长：契约只定「对该服务器退避」（§2.7），未定幅度。
///
/// 取 1 小时：本仓定时探测本就 ≤5 次/天，但公共 NTP 的 RATE 多由出口 IP 共享触发，退避更久会让
/// 「别人的滥用」长期惩罚本机；1 小时足以降频又不至于让恢复过慢。
inline constexpr int64_t kKissRateBackoffNs = 3'600LL * 1'000'000'000LL;

/**
 * @brief 一轮时钟探测的编排器（契约 sysmon §2.7 多服务器聚合、§2.9 明细）。
 *
 * 生命周期：一个实例跨轮复用（KoD 退避/拉黑状态必须跨轮保持），非线程安全——与 dzsysmon 的
 * io_context 单线程模型一致，只在那一线程上调用。
 */
class ClockProber {
public:
    using RoundHandler = std::function<void(ProbeOutcome)>;

    /**
     * @param transport 传输接缝（真实 Asio 实现由 #299 注入；测试注入假实现）
     * @param moment    时刻读数接缝（生产取两个 std::chrono 时钟；测试注入脚本化读数）
     */
    ClockProber(NtpTransport& transport, MomentSource moment);

    ClockProber(const ClockProber&) = delete;
    ClockProber& operator=(const ClockProber&) = delete;
    ClockProber(ClockProber&&) = delete;
    ClockProber& operator=(ClockProber&&) = delete;

    /// @return 是否有探测在飞（手动触发限流判定要读它）。
    [[nodiscard]] bool probe_in_flight() const noexcept { return in_flight_; }

    /**
     * @brief 发起一轮探测：对 @p hosts 中每台各发一次，全部结算后回调 @p done 一次。
     *
     * - `hosts` 中的**重复项只探一次**且只保留一条明细（快照 `servers[].host` 要求帧内唯一，§7）
     * - 处于退避/拉黑状态的服务器不发包，直接以 `kod_rate` / `kod_deny` 结算
     * - 聚合值取最小 RTT（同 RTT 取 `bound_ms` 更小者）；全部失败则为不可用空态
     * - 全部服务器都秒结算时（例如全部被拉黑），`done` 在 `probe()` 返回前同步被调用
     *
     * @param hosts       本轮要探测的服务器列表（配置校验保证非空）
     * @param timeout_ms  单台超时（`clock.probe_timeout_ms`）
     * @param done        本轮完成回调（恰好调用一次）
     * @return false = 已有探测在飞，本次未发起（`done` 不会被调用）
     */
    [[nodiscard]] bool probe(const std::vector<std::string>& hosts,
                             int32_t timeout_ms,
                             RoundHandler done);

private:
    /// 单台服务器的进行中状态（下标即 `details_` 的 slot，回调按它以 O(1) 找回上下文）。
    struct Pending {
        ntp::Timestamp t1;      ///< 我方发送时刻（即报文里的 transmit timestamp）
        int64_t steady_ns = 0;  ///< 发送时刻的单调时钟读数（往返基线）
        bool settled = true;    ///< 是否已结算（重复回调幂等忽略）
    };

    /// KoD 状态：`blocked = true` 表示拉黑到进程重启，否则按单调时钟退避到 `until_ns`。
    struct KissEntry {
        bool blocked = false;
        int64_t until_ns = 0;
    };

    /// 处理一次交互结果并推进结算计数。
    void handle_exchange(std::size_t slot, NtpExchange result);

    /// 记录 KoD 状态（跨轮保持）。
    void note_kiss(const std::string& host, ntp::KissCode code, int64_t now_steady_ns);

    /// 被退避/拉黑时返回对应的失败原因；可探测返回 `nullopt`。
    [[nodiscard]] std::optional<const char*> skip_reason(const std::string& host,
                                                         int64_t now_steady_ns) const;

    /// 收尾：算聚合值并交付本轮结果。
    void finish_round();

    /// 记录一个失败明细（四个测量字段为 `null`，与 §7 校验一致）。
    static void mark_failed(platform::SysmonClockServerInfo& detail, const char* error);

    NtpTransport& transport_;
    MomentSource moment_;
    std::map<std::string, KissEntry> kiss_state_;  ///< 按 host 保持的 KoD 状态
    std::vector<Pending> pending_;  ///< 本轮每台的上下文（下标 == `details_` 的下标）
    std::vector<platform::SysmonClockServerInfo> details_;  ///< 本轮明细骨架
    std::size_t remaining_ = 0;                             ///< 尚未结算的台数
    RoundHandler done_;
    bool in_flight_ = false;         ///< 本轮探测在飞
    bool issuing_ = false;           ///< 正在 probe() 的发包循环里（同步回调要延后收尾）
    bool finish_requested_ = false;  ///< 发包循环里已结算完，出循环后立即收尾
};

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_CLOCK_PROBE_H_
