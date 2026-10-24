#ifndef DZTRADER_SYSMON_ALERTS_H_
#define DZTRADER_SYSMON_ALERTS_H_

/**
 * @file alerts.h
 * @brief 阈值告警的判定、冷却防抖与恢复（契约 sysmon §2.8；#80）。
 *
 * 边界（刻意保持纯逻辑，便于确定性测试）：
 * - 本类**不读时钟、不读全局配置、不写帧**：判定所需的资源事实、配置与两个注入时刻
 *   （墙钟用于 `since`、单调时钟用于冷却）全部由调用方给出；
 * - 本类只产出「要发哪些 UI 通知」与「当前活动告警集合」；通知的发送级别/弹窗与
 *   快照编码由调用方（`main.cpp`）负责。
 *
 * 状态机语义（契约 sysmon §2.8）：
 * - 每个告警对象 `(kind, subject)` 维护一个活动期：首次越限进入活动期（记 `since`），
 *   恢复时离开活动期；
 * - 触发通知只在进入活动期时发一次，阈值持续满足**不重弹**；
 * - 冷却期内（距上次已发出的触发通知不足 `cooldown_sec`）再次进入活动期**不弹**；
 *   被抑制的活动期不补发、恢复时也不发恢复通知（用户从未收到触发通知）；
 * - 本帧读不到数值（能力仍在）时**保留**活动状态、不产生恢复；能力被配置关闭时静默清除。
 */

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <dztrader/platform/sysmon.h>

#include "snapshot_builder.h"

namespace dztrader::sysmon {

/// 关注卷的容量事实（只含已解析出挂载点且本帧容量可读的关注卷）。
struct WatchedVolumeUsage {
    std::string mount;         ///< 卷挂载点（与 `volumes[].mount` 同口径）
    uint64_t total_bytes = 0;  ///< 卷总容量
    uint64_t free_bytes = 0;   ///< 卷剩余容量（已钳制到 `[0, total_bytes]`）
};

/// 一次计量得到的告警判定输入事实。
///
/// `*_applicable = false` 表示该指标类别已被配置关闭（或本机不再具备该能力，如交换被移除）：
/// 对应告警被**静默清除**（配置关闭不是恢复）。`optional` 数值为 `nullopt` 表示
/// 「能力仍在但本帧未采到」：保留活动状态，不伪造恢复。
struct AlertInputs {
    bool memory_applicable = false;           ///< 内存采集能力是否仍在
    std::optional<double> memory_used_ratio;  ///< 内存使用率 `[0,1]`；nullopt = 本帧未采到
    bool swap_applicable = false;             ///< 交换能力是否仍在（无交换/已移除时 false）
    std::optional<double> swap_used_ratio;    ///< 交换使用率 `[0,1]`；nullopt = 本帧未采到
    bool volumes_applicable = false;          ///< 卷容量采集能力是否仍在
    /// 本帧关注路径落卷解析是否成功采集：false 时无法判断"某卷不再被关注"，不清理卷告警。
    bool watch_paths_collected = false;
    /// 当前关注卷挂载点（解析成功的 `watch_paths`，按 `mount` 去重；用于识别不再被关注的卷）。
    std::vector<std::string> watched_mounts;
    /// 本帧容量可读的关注卷（`mount` 必须出现在 `watched_mounts` 中、单帧内唯一）。
    std::vector<WatchedVolumeUsage> watched_volumes;
    int64_t now_unix_sec = 0;      ///< 墙钟时刻（只用于 `since`，Unix 秒）
    int64_t now_monotonic_ms = 0;  ///< 单调时钟毫秒（只用于冷却计时，不受系统时间调整影响）
};

/**
 * @brief 由一次计量的指标事实构造告警判定输入（**路径感知的唯一实现点**）。
 *
 * 只把「落卷解析成功且容量可读」的**关注卷**交给判定：非关注卷容量再低也不进入
 * `watched_volumes`，因此不会产生告警（契约 sysmon §2.8）。
 * 转换口径与快照一致：`used = total − available/free`；能力被配置关闭或本机无交换时
 * `*_applicable = false`（告警被静默清除）。
 *
 * @param memory_metrics 内存/交换测量结果（含配置关闭与无交换标记）
 * @param disk_metrics   卷/关注路径测量结果（含配置关闭标记）
 * @param now_unix_sec   墙钟时刻（Unix 秒，写入 `since`）
 * @param now_monotonic_ms 单调时钟毫秒（冷却计时）
 * @return 可直接交给 `AlertEvaluator::evaluate` 的输入
 */
[[nodiscard]] AlertInputs make_alert_inputs(const MemorySwapMetrics& memory_metrics,
                                            const DiskMetrics& disk_metrics,
                                            int64_t now_unix_sec,
                                            int64_t now_monotonic_ms);

/// 需要发送的一条 UI 通知（`DZ_FRAME_NOTIFY_UI`，契约 notify-ui）。
///
/// 级别/弹窗由调用方按下表发送（契约 sysmon §2.8）：触发 = error + popup；恢复 = info + 不弹。
struct AlertNotification {
    std::string kind;       ///< 告警 kind（契约 sysmon §2.8）
    std::string subject;    ///< volume_free 的挂载点；其余 kind 为空串
    bool recovery = false;  ///< false = 触发通知；true = 恢复通知
    std::string message;    ///< 用户可读正文（不构成协议语义）
};

/// 当前活动告警（快照 `alerts[]` 的条目，按 `(kind, subject)` 升序）。
struct ActiveAlert {
    std::string kind;
    std::string subject;
    int64_t since_unix_sec = 0;
};

/**
 * @brief 阈值告警状态机：触发 / 冷却 / 恢复。
 *
 * 非线程安全：只在 io_context 线程上使用。
 */
class AlertEvaluator {
public:
    /// 一次评估的产物。
    struct Result {
        std::vector<AlertNotification> notifications;  ///< 本次需要发送的通知（可能为空）
        std::vector<ActiveAlert> active;               ///< 当前活动告警（按 kind、subject 升序）
    };

    AlertEvaluator() = default;
    AlertEvaluator(const AlertEvaluator&) = delete;
    AlertEvaluator& operator=(const AlertEvaluator&) = delete;
    AlertEvaluator(AlertEvaluator&&) = delete;
    AlertEvaluator& operator=(AlertEvaluator&&) = delete;
    ~AlertEvaluator() = default;

    /**
     * @brief 用本帧事实与当前生效配置推进状态机。
     *
     * @param inputs 资源事实 + 注入的两个时刻
     * @param config 当前生效 `alerts` 配置（阈值与冷却；调用方须保证已经过校验）
     * @return 待发送通知与当前活动告警集合
     */
    [[nodiscard]] Result evaluate(const AlertInputs& inputs,
                                  const platform::SysmonAlertsConfig& config);

    /// 清空全部告警状态（不产生通知）。进程内状态随进程生命周期归零，正常路径无需调用；
    /// 供测试与将来的配置重置路径使用。
    void reset();

private:
    /// 单个告警对象 `(kind, subject)` 的活动期状态。
    struct SubjectState {
        bool active = false;         ///< 当前是否处于活动期
        bool notified = false;       ///< 本次活动期是否已发出触发通知
        bool suppressed = false;     ///< 本次活动期是否因冷却被抑制（不补发、恢复时也不通知）
        bool has_notified = false;   ///< 是否曾发出过触发通知（冷却基准是否有效）
        int64_t since_unix_sec = 0;  ///< 本次活动期起点（墙钟，Unix 秒）
        int64_t last_notify_monotonic_ms = 0;  ///< 上次发出触发通知的单调时钟
    };

    /// 推进一个告警对象的状态机；需要通知时追加到 `out`。
    void transition(const std::string& kind,
                    const std::string& subject,
                    bool breached,
                    const std::string& trigger_message,
                    const std::string& recovery_message,
                    int64_t cooldown_ms,
                    const AlertInputs& inputs,
                    Result& out);

    /// 静默清除某一 kind 的全部状态（配置关闭 / 能力消失）。
    void erase_kind(const std::string& kind);

    std::map<std::pair<std::string, std::string>, SubjectState> states_;
};

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_ALERTS_H_
