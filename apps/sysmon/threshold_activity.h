#ifndef DZTRADER_SYSMON_THRESHOLD_ACTIVITY_H_
#define DZTRADER_SYSMON_THRESHOLD_ACTIVITY_H_

/**
 * @file threshold_activity.h
 * @brief 阈值判定对象的**活动期**记账与判定对象标识（契约 sysmon §2.8）。
 *
 * 资源类判定（`ThresholdEvaluator`：内存 / 交换 / 卷 / CPU 饱和）与时间判定
 * （`ClockThresholdEvaluator`：`clock_offset` / `clock_bound`）共用本文件的两种类型：
 *
 * - `ThresholdKey`：判定对象标识。日志里的 `subject=` 与判定对象的稳定 identity 都取它。
 * - `ActivityPeriod`：「进入时记起点、持续期按里程碑重记、恢复时结束」的记账。
 *
 * **为什么共用**：里程碑锚点（恒为活动期起点 `since + N × 间隔`，不是「上次记录 + 间隔」）、
 * 运行期改变间隔后的重排、漏掉的里程碑不补发、持续秒数一律按单调时钟计——这些是同一份
 * 口径。两处各写一遍必然在某条边界上漂移（review-checklist §2.4「同一事实两处定义」）。
 * 本文件只做记账，不读时钟、不读配置、不写日志：两个时刻与间隔全部由调用方给出。
 */

#include <compare>
#include <cstdint>
#include <string>

namespace dztrader::sysmon {

/// 判定对象标识：kind + subject（`volume_free` 时 subject 为挂载点，其余为空串）。
///
/// `(kind, subject)` 在单次评估内唯一，本类型是状态表键的单一标识类型——
/// 避免把"两个字符串"当裸 pair 在接口间传递。
struct ThresholdKey {
    std::string kind;
    std::string subject;
    [[nodiscard]] auto operator<=>(const ThresholdKey& other) const = default;
};

/**
 * @brief 一个判定对象的**活动期**状态（一次「进入越限 → 回到阈值以内」是一次活动期）。
 *
 * 契约 sysmon §2.8：进入记一条、恢复记一条；持续期每满 `thresholds.log_repeat_sec`
 * 重记一条「仍越限」，重记计时用**单调时钟**（不受系统时间调整影响），基准是活动期的
 * **进入时刻**——因此里程碑恒为 `进入时刻 + N × 间隔`，长期运行也不会把重记时刻漂到
 * 周期的别处；中间漏掉的里程碑不补发（一条「仍越限」已足够表达"它还在持续"）。
 */
struct ActivityPeriod {
    bool active = false;             ///< 当前是否处于活动期
    int64_t since_unix_sec = 0;      ///< 本次活动的起点（墙钟，Unix 秒）
    int64_t since_monotonic_ms = 0;  ///< 本次活动期起点（单调时钟，重记基准）
    /// 已记录到第几个里程碑：里程碑 = `since_monotonic_ms + N × 间隔`（N 从 1 起）。
    int64_t recorded_milestone = 0;
    /// `recorded_milestone` 所依据的重记间隔（毫秒）；运行期改变间隔时按新间隔重排。
    int64_t scheduled_repeat_ms = 0;

    /**
     * @brief 进入一个新的活动期。
     *
     * @param wall_sec     墙钟时刻（Unix 秒，只用于日志里"始于何时"）
     * @param monotonic_ms 单调时钟毫秒（重记基准）
     * @param repeat_ms    当次生效的重记间隔（毫秒；0 = 不重记）
     */
    void start(int64_t wall_sec, int64_t monotonic_ms, int64_t repeat_ms) noexcept {
        active = true;
        since_unix_sec = wall_sec;
        since_monotonic_ms = monotonic_ms;
        // 第一条重记出现在"进入时刻 + 一个间隔"（里程碑 1）；之后每个间隔一条。
        recorded_milestone = 0;
        scheduled_repeat_ms = repeat_ms;
    }

    /// 活动期已持续秒数（按单调时钟；跨越采集缺口的恢复记录也如实反映总时长）。
    [[nodiscard]] int64_t elapsed_sec(int64_t now_monotonic_ms) const noexcept {
        return (now_monotonic_ms - since_monotonic_ms) / 1000;
    }

    /**
     * @brief 周期重记判定：跨过一个尚未记录的里程碑时返回 true（契约 sysmon §2.8）。
     *
     * 间隔运行期改变时按新间隔重排已记录的序号（当前调用不触发），使后续里程碑仍然落在
     * `since + N × 新间隔` 上，而不是沿用旧间隔的相位。
     *
     * @param now_monotonic_ms 当前单调时钟毫秒
     * @param repeat_ms        当次生效的重记间隔（毫秒；0 = 不重记，恒为 false）
     */
    [[nodiscard]] bool repeat_due(int64_t now_monotonic_ms, int64_t repeat_ms) noexcept {
        if (repeat_ms <= 0) {
            return false;  // 0 = 不重记（契约 §2.8）
        }
        const int64_t milestone = (now_monotonic_ms - since_monotonic_ms) / repeat_ms;
        if (repeat_ms != scheduled_repeat_ms) {
            scheduled_repeat_ms = repeat_ms;
            recorded_milestone = milestone;
            return false;
        }
        if (milestone <= recorded_milestone) {
            return false;  // 本里程碑已记过（或尚未到达）
        }
        recorded_milestone = milestone;  // 跳过的里程碑不补发
        return true;
    }
};

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_THRESHOLD_ACTIVITY_H_
