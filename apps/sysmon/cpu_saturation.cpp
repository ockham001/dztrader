#include "cpu_saturation.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dztrader::sysmon {

void CoreSaturationTracker::sync_states(const std::vector<platform::SysmonCpuCore>& cores) {
    bool same = states_.size() == cores.size();
    if (same) {
        for (std::size_t i = 0; i < cores.size(); ++i) {
            if (states_[i].index != cores[i].index) {
                same = false;
                break;
            }
        }
    }
    if (!same) {
        states_.clear();
        states_.resize(cores.size());
        for (std::size_t i = 0; i < cores.size(); ++i) {
            states_[i].index = cores[i].index;
        }
    }
}

void CoreSaturationTracker::update(std::vector<platform::SysmonCpuCore>& cores,
                                   double threshold_ratio,
                                   uint32_t required_samples) {
    sync_states(cores);
    saturated_cores_.clear();
    // 配置校验保证 >= 1；这里再防御一次，避免 0 让「连续 0 次」立即饱和。
    const uint32_t required = required_samples == 0 ? 1 : required_samples;
    for (std::size_t i = 0; i < cores.size(); ++i) {
        auto& state = states_[i];
        const auto ratio = cores[i].usage_ratio;
        if (ratio.has_value()) {
            if (ratio.value() >= threshold_ratio) {
                if (state.consecutive < required) {
                    ++state.consecutive;
                }
            } else {
                state.consecutive = 0;  // 回落到阈值以下：解除饱和
            }
        }
        // 不可用样本不改变连续计数（无法观察时不做判定）。
        cores[i].saturated = state.consecutive >= required;
        if (cores[i].saturated) {
            saturated_cores_.push_back(cores[i].index);
        }
    }
}

void CoreSaturationTracker::reset() {
    states_.clear();
    saturated_cores_.clear();
}

}  // namespace dztrader::sysmon
