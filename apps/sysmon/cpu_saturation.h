#ifndef DZTRADER_SYSMON_CPU_SATURATION_H_
#define DZTRADER_SYSMON_CPU_SATURATION_H_

/**
 * @file cpu_saturation.h
 * @brief 饱和核判定（契约 sysmon §2.7）。
 *
 * 规则：单核使用率连续 `thresholds.cpu_sustain_samples` 次采样 **≥** 阈值
 * （`thresholds.cpu_sustain_used_pct / 100`）即标记为 `saturated=true`；任一采样严格
 * 低于阈值即清零并解除；不可用样本（null）不改变连续计数；核序列变化（热插拔）
 * 重置全部计数。阈值与连续次数每采样从当前配置读取，运行期修改即时生效。
 *
 * 非线程安全：只在 io_context 线程使用。
 */

#include <cstdint>
#include <vector>

#include <dztrader/platform/sysmon.h>

namespace dztrader::sysmon {

/// 饱和核判定状态机（契约 sysmon §2.7）。
class CoreSaturationTracker {
public:
    CoreSaturationTracker() = default;

    CoreSaturationTracker(const CoreSaturationTracker&) = delete;
    CoreSaturationTracker& operator=(const CoreSaturationTracker&) = delete;

    /**
     * @brief 推进一次采样并回写每个核的 `saturated` 标记。
     *
     * @param cores            本次每核使用率（index + usage_ratio）；`saturated` 由本函数写入
     * @param threshold_ratio  饱和阈值（比率，[0,1]；来自 thresholds.cpu_sustain_used_pct / 100）
     * @param required_samples 连续采样次数阈值（来自 thresholds.cpu_sustain_samples）
     */
    void update(std::vector<platform::SysmonCpuCore>& cores,
                double threshold_ratio,
                uint32_t required_samples);

    /// 最近一次 update 后处于饱和状态的核 index 集合（升序）。
    [[nodiscard]] const std::vector<uint32_t>& saturated_cores() const noexcept {
        return saturated_cores_;
    }

    /// 清空全部状态（核序列变化/重新建立基线时使用）。
    void reset();

private:
    struct CoreState {
        uint32_t index = 0;
        uint32_t consecutive = 0;
    };

    /// 与当前核序列对齐：长度或 index 序列变化时清空状态（热插拔 = 重新计数）。
    void sync_states(const std::vector<platform::SysmonCpuCore>& cores);

    std::vector<CoreState> states_;
    std::vector<uint32_t> saturated_cores_;
};

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_CPU_SATURATION_H_
