#ifndef DZTRADER_SYSMON_CLOCK_THRESHOLDS_H_
#define DZTRADER_SYSMON_CLOCK_THRESHOLDS_H_

/**
 * @file clock_thresholds.h
 * @brief 时间判定对象 `clock_offset` / `clock_bound` 的判定、周期重记与恢复
 *        （契约 sysmon §2.8「时间判定对象」；#301 / ADR 0047）。
 *
 * 与资源类 `ThresholdEvaluator` 的三点差异（其余口径共用，见 `threshold_activity.h`）：
 * 1. **判定时机是每次探测完成时**，不是计量 tick——探测是低频事件，没有「持续采样」概念；
 * 2. `clock_offset` 是**一个判定对象、两个级别**（warning / error），活动期内可在两档间
 *    升级与降级；**只有 error 档**产生 `NOTIFY_UI`（ADR 0047 对 ADR 0034 静默原则的部分修订）；
 * 3. 输入是**毫秒量**（有符号偏差与误差上界），不是 `[0,1]` 比率，因此当前值与阈值按
 *    毫秒口径记录，日志字段也不是 `current_pct` / `threshold_pct`。
 *
 * 边界（刻意保持纯逻辑，与资源类同一纪律）：**不读时钟、不读全局配置、不写帧、不写日志**。
 * 两个时刻、阈值与重记间隔全部由调用方注入；本文件只产出「待写日志记录 + 是否弹窗」，
 * 日志与 `NOTIFY_UI` 由调用方（`app.cpp`）落。
 *
 * 状态机语义（契约 sysmon §2.8）：
 * - `clock_offset`：`|offset_ms| ≥ offset_warn_ms` 进入 warning 档、`≥ offset_error_ms` 进入
 *   error 档；活动期从「进入 warning 或 error」开始、回到 `< offset_warn_ms` 结束。
 *   进入按当前档记一条；warning→error 记 `error`（升级 + 弹窗）；error→warning 记 `info`
 *   （降级，活动期不结束）；持续期按 `thresholds.log_repeat_sec` 重记 `info`；恢复记 `info`。
 * - `clock_bound`：`bound_ms > bound_alarm_ms` 进入、`≤` 恢复；进入 `warning`、重记与恢复
 *   `info`；**不弹窗**，也**不改变** `clock_offset` 的判定结果（「测不准」与「真的偏」是
 *   两件事：混成一个数事后无法区分该修网络还是修时钟）。
 * - **读不到不伪造恢复**：探测失败（两个值为 `nullopt`）时保留活动状态、不写恢复日志。
 * - **配置关闭静默清除**：`clock` 域被关闭时移除两个判定对象的活动状态，不写恢复日志。
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include <dztrader/platform/sysmon.h>

#include "threshold_activity.h"

namespace dztrader::sysmon {

/// `clock_offset` 当前所处档位（`None` = 未越限）。
enum class ClockOffsetTier {
    None,     ///< `|offset_ms| < offset_warn_ms`
    Warning,  ///< `offset_warn_ms ≤ |offset_ms| < offset_error_ms`
    Error,    ///< `|offset_ms| ≥ offset_error_ms`（唯一弹窗来源）
};

/// 一次判定产生的记录类型（契约 sysmon §2.8 时间判定对象的跃迁集合）。
///
/// 与资源类的 `ThresholdState` 刻意分开：后者的三态（进入/重记/恢复）表达不了
/// 「同一活动期内 warning ↔ error 的升级与降级」，而两者的 `state=` 日志取值也因此不同。
enum class ClockThresholdTransition {
    EnteredWarning,  ///< 进入活动期（warning 档）—— `warning` 日志
    EnteredError,    ///< 进入活动期（error 档）—— `error` 日志 + `NOTIFY_UI`
    Escalated,       ///< 活动期内 warning → error —— `error` 日志 + `NOTIFY_UI`
    Downgraded,      ///< 活动期内 error → warning —— `info` 日志（活动期不结束）
    Ongoing,         ///< 持续期按 `log_repeat_sec` 重记 —— `info` 日志
    Recovered,       ///< 回到阈值以内、活动期结束 —— `info` 日志
};

/// 一次探测完成后交给判定的输入事实。
struct ClockThresholdInputs {
    /// `clock` 域是否启用（`clock.enabled` 与 `sampling.groups.clock` 关闭取或）。
    /// `false` = 静默清除两个判定对象的活动状态（配置关闭不是恢复）。
    bool applicable = false;
    /// 本轮聚合偏差（毫秒，有符号；正 = 本地快）。`nullopt` = 本轮未测到 →
    /// **保留**活动状态、不写恢复日志（「测不到」不是「已恢复」）。
    std::optional<double> offset_ms;
    /// 本轮聚合误差上界（毫秒）。与 `offset_ms` 同生共死（契约 §2.9），但判定独立：
    /// 两者的越限互不影响对方的活动期。
    std::optional<double> bound_ms;
    int64_t now_unix_sec = 0;      ///< 墙钟时刻（只用于活动期起点，Unix 秒）
    int64_t now_monotonic_ms = 0;  ///< 单调时钟毫秒（只用于重记计时，不受系统时间调整影响）
};

/// 时间判定所需的配置（两个配置段各取一部分：三个时间阈值住 `clock` 段，重记间隔住
/// `thresholds` 段；契约 §3.1 把它们分开放，判定层在此汇成一份，避免在判定里两处各读一遍）。
struct ClockThresholdConfig {
    int32_t offset_warn_ms = 100;    ///< 偏差 warning 档阈值（`clock.offset_warn_ms`）
    int32_t offset_error_ms = 1000;  ///< 偏差 error 档阈值（`clock.offset_error_ms`）
    int32_t bound_alarm_ms = 200;    ///< 误差界质量判据（`clock.bound_alarm_ms`）
    int32_t log_repeat_sec = 600;    ///< 持续期重记间隔（`thresholds.log_repeat_sec`；0 = 不重记）

    /// 从两段当前生效配置取值（调用方须保证已经过校验：`offset_error_ms > offset_warn_ms`）。
    [[nodiscard]] static ClockThresholdConfig from(
        const platform::SysmonClockConfig& clock,
        const platform::SysmonThresholdsConfig& thresholds) noexcept;
};

/// 需要写的一条记录（不构成协议语义；调用方负责落日志与 `NOTIFY_UI`）。
///
/// 与资源类的 `ThresholdLogRecord` 分开：字段口径不同（毫秒 vs 比率）、多出「档位」与
/// 「是否弹窗」，混成一个结构会让资源类记录长期携带恒为 `None` / `false` 的字段。
struct ClockThresholdLogRecord {
    ThresholdKey key;  ///< 判定对象标识（`clock_offset` / `clock_bound`）
    ClockThresholdTransition transition = ClockThresholdTransition::Ongoing;  ///< 跃迁类型
    spdlog::level::level_enum level = spdlog::level::info;  ///< 日志级别（见文件头表格）
    /// 是否要求调用方发 `NOTIFY_UI`（`popup=true`）。只对 `clock_offset` 的
    /// error 档进入 / 升级为 true；`clock_bound` 与 warn 档恒为 false（契约 §2.8）。
    bool notify_popup = false;
    /// 当前偏差（毫秒，有符号）；`clock_bound` 的记录里为 `nullopt`。
    std::optional<double> offset_ms;
    /// 当前误差上界（毫秒）；`clock_offset` 的记录里为 `nullopt`。
    std::optional<double> bound_ms;
    /// 当次生效的两个偏差阈值（`clock_offset` 记录的「阈值」义务含两档，契约 §2.8）。
    int32_t offset_warn_ms = 0;
    int32_t offset_error_ms = 0;
    int32_t bound_alarm_ms = 0;  ///< 当次生效的质量阈值（`clock_bound` 记录用）
    int64_t active_sec = 0;      ///< 活动期已持续秒数（进入时为 0）
    int64_t since_unix_sec = 0;  ///< 本次活动期起点（墙钟 Unix 秒；进入记录里带上它）
    std::string message;         ///< 组装好的英文单行日志文本（`prose | k=v`）
    /// `notify_popup = true` 时要发给 UI 的通知正文（英文、小写开头、无句号、`prose | k=v`）。
    /// 与 `message` 在同一处生成：两处各拼一套文本口径必然漂移（review-checklist §2.4）。
    std::string notify_message;
};

/**
 * @brief 时间判定状态机：`clock_offset`（双档）与 `clock_bound`（独立质量判据）。
 *
 * 非线程安全：只在 io_context 线程上使用（与探测回调同一线程）。
 */
class ClockThresholdEvaluator {
public:
    /// 一次判定的产物。
    struct Result {
        std::vector<ClockThresholdLogRecord> records;  ///< 待写日志记录（可能为空）
    };

    ClockThresholdEvaluator() = default;
    ClockThresholdEvaluator(const ClockThresholdEvaluator&) = delete;
    ClockThresholdEvaluator& operator=(const ClockThresholdEvaluator&) = delete;
    ClockThresholdEvaluator(ClockThresholdEvaluator&&) = delete;
    ClockThresholdEvaluator& operator=(ClockThresholdEvaluator&&) = delete;
    ~ClockThresholdEvaluator() = default;

    /**
     * @brief 用一轮探测结果与当前生效配置推进状态机。
     *
     * 两个判定对象相互独立：`clock_offset` 的越限不改变 `clock_bound` 的活动期，反之亦然
     * （契约 §2.8：「测不准」与「真的偏」是两件事）。
     *
     * @param inputs 探测事实 + 注入的两个时刻
     * @param config 当前生效的时间阈值与重记间隔（调用方须保证已经过校验）
     * @return 本轮待写日志记录（`clock_offset` 在前、`clock_bound` 在后，各自至多一条）
     */
    [[nodiscard]] Result evaluate(const ClockThresholdInputs& inputs,
                                  const ClockThresholdConfig& config);

private:
    /// `clock_offset` 的状态：活动期 + 当前档位（档位只在活动期内有意义）。
    struct OffsetState {
        ActivityPeriod period;
        ClockOffsetTier tier = ClockOffsetTier::None;
    };

    /// 判定偏差档位（绝对值比较：正 = 本地快，方向不影响越限；契约 §2.8「相等按表处理」）。
    [[nodiscard]] static ClockOffsetTier judge_tier(double offset_ms,
                                                    int32_t warn_ms,
                                                    int32_t error_ms) noexcept;

    /// 推进 `clock_offset`：进入 / 升级 / 降级 / 重记 / 恢复。
    void evaluate_offset(const ClockThresholdInputs& inputs,
                         const ClockThresholdConfig& config,
                         int64_t repeat_ms,
                         Result& out);

    /// 推进 `clock_bound`：进入 / 重记 / 恢复（无档位、无弹窗）。
    void evaluate_bound(const ClockThresholdInputs& inputs,
                        const ClockThresholdConfig& config,
                        int64_t repeat_ms,
                        Result& out);

    /// 组装一条 `clock_offset` 记录（日志文本与通知正文在同一处生成，避免两套文本口径）。
    [[nodiscard]] ClockThresholdLogRecord make_offset_record(const ClockThresholdConfig& config,
                                                             ClockThresholdTransition transition,
                                                             spdlog::level::level_enum level,
                                                             bool notify_popup,
                                                             int64_t active_sec,
                                                             double offset_ms);

    /// 静默清除全部时间判定状态（配置关闭；清除原因不是"指标回到阈值以下"）。
    void clear() noexcept {
        offset_ = OffsetState{};
        bound_ = ActivityPeriod{};
    }

    OffsetState offset_;    ///< `clock_offset` 的活动期与档位
    ActivityPeriod bound_;  ///< `clock_bound` 的活动期
};

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_CLOCK_THRESHOLDS_H_
