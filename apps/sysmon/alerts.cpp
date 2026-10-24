#include "alerts.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

/// 字节数的人类可读文案（通知正文用；1 位小数，剩余量与阈值同口径）。
std::string format_bytes(uint64_t bytes) {
    constexpr double kKib = 1024.0;
    constexpr double kMib = kKib * 1024.0;
    constexpr double kGib = kMib * 1024.0;
    const double value = static_cast<double>(bytes);
    if (value >= kGib) {
        return std::format("{:.1f} GiB", value / kGib);
    }
    if (value >= kMib) {
        return std::format("{:.1f} MiB", value / kMib);
    }
    if (value >= kKib) {
        return std::format("{:.1f} KiB", value / kKib);
    }
    return std::format("{} B", bytes);
}

/// 比率 → 百分比文案（通知正文用；1 位小数）。
std::string format_ratio_pct(double ratio) { return std::format("{:.1f}%", ratio * 100.0); }

/// 使用率是否可判定：nullopt / 非有限值 / 越界都按「本帧不可判定」处理（保留活动状态）。
bool is_valid_ratio(const std::optional<double>& ratio) {
    return ratio.has_value() && std::isfinite(*ratio) && *ratio >= 0.0 && *ratio <= 1.0;
}

std::string usage_trigger_message(std::string_view metric,
                                  double used_ratio,
                                  double threshold_pct) {
    return std::format("{}使用率 {} 达到告警阈值 {:.1f}%", metric, format_ratio_pct(used_ratio),
                       threshold_pct);
}

std::string usage_recovery_message(std::string_view metric,
                                   double used_ratio,
                                   double threshold_pct) {
    return std::format("{}使用率已回到告警阈值以下（当前 {}，阈值 {:.1f}%）", metric,
                       format_ratio_pct(used_ratio), threshold_pct);
}

/// 关注卷剩余空间的描述（百分比形态在卷总量为 0 时无法计算，如实说明而不编一个百分比）。
std::string volume_free_desc(const WatchedVolumeUsage& usage) {
    if (usage.total_bytes > 0) {
        const double free_ratio =
            static_cast<double>(usage.free_bytes) / static_cast<double>(usage.total_bytes);
        return std::format("剩余 {}（{}）", format_bytes(usage.free_bytes),
                           format_ratio_pct(free_ratio));
    }
    return std::format("剩余 {}（卷总量为 0）", format_bytes(usage.free_bytes));
}

std::string volume_trigger_message(const WatchedVolumeUsage& usage,
                                   const platform::SysmonAlertsConfig& config) {
    return std::format("关注卷 {} 剩余空间低于告警阈值：{}，阈值 {:.1f}% / {}", usage.mount,
                       volume_free_desc(usage), config.volume_free_pct,
                       format_bytes(config.volume_free_bytes));
}

std::string volume_recovery_message(const WatchedVolumeUsage& usage) {
    return std::format("关注卷 {} 剩余空间已回到告警阈值以上：{}", usage.mount,
                       volume_free_desc(usage));
}

}  // namespace

AlertInputs make_alert_inputs(const MemorySwapMetrics& memory_metrics,
                              const DiskMetrics& disk_metrics,
                              int64_t now_unix_sec,
                              int64_t now_monotonic_ms) {
    AlertInputs inputs;
    inputs.now_unix_sec = now_unix_sec;
    inputs.now_monotonic_ms = now_monotonic_ms;

    // 内存：口径与快照一致（used = total − available）；配置关闭时 applicable=false。
    inputs.memory_applicable = !memory_metrics.disabled;
    if (!memory_metrics.disabled && memory_metrics.memory.has_value() &&
        memory_metrics.memory->total_bytes > 0) {
        const auto& raw = *memory_metrics.memory;
        const uint64_t available = std::min(raw.available_bytes, raw.total_bytes);
        inputs.memory_used_ratio =
            static_cast<double>(raw.total_bytes - available) / static_cast<double>(raw.total_bytes);
    }

    // 交换：无交换分区/页面文件（swap_supported=false）时告警不适用（不判定）。
    inputs.swap_applicable = !memory_metrics.swap_disabled && memory_metrics.swap_supported;
    if (memory_metrics.swap.has_value() && memory_metrics.swap->total_bytes > 0) {
        const auto& raw = *memory_metrics.swap;
        const uint64_t free_bytes = std::min(raw.free_bytes, raw.total_bytes);
        inputs.swap_used_ratio = static_cast<double>(raw.total_bytes - free_bytes) /
                                 static_cast<double>(raw.total_bytes);
    }

    // 关注卷剩余空间（路径感知）：只把落卷成功且容量可读的关注卷交给判定；
    // 非关注卷容量再低也进不了 watched_volumes（契约 sysmon §2.8）。
    inputs.volumes_applicable = !disk_metrics.volumes_disabled;
    inputs.watch_paths_collected = disk_metrics.watch_paths.has_value();
    if (!disk_metrics.watch_paths.has_value()) {
        return inputs;
    }
    std::set<std::string> watched_mounts;
    for (const auto& info : *disk_metrics.watch_paths) {
        if (info.mount.has_value()) {
            watched_mounts.insert(*info.mount);
        }
    }
    inputs.watched_mounts.assign(watched_mounts.begin(), watched_mounts.end());
    if (!disk_metrics.volumes.has_value()) {
        return inputs;
    }
    for (const auto& volume : *disk_metrics.volumes) {
        if (!watched_mounts.contains(volume.mount)) {
            continue;
        }
        // 卷存在但容量读不到：无法判定，保留该卷的活动状态（不伪造恢复）。
        if (!volume.total_bytes.has_value() || !volume.free_bytes.has_value()) {
            continue;
        }
        WatchedVolumeUsage usage;
        usage.mount = volume.mount;
        usage.total_bytes = *volume.total_bytes;
        usage.free_bytes = std::min(*volume.free_bytes, usage.total_bytes);
        inputs.watched_volumes.push_back(std::move(usage));
    }
    // 告警通知顺序按挂载点稳定排序（采集层顺序不保证）。
    std::sort(inputs.watched_volumes.begin(), inputs.watched_volumes.end(),
              [](const WatchedVolumeUsage& lhs, const WatchedVolumeUsage& rhs) {
                  return lhs.mount < rhs.mount;
              });
    return inputs;
}

AlertEvaluator::Result AlertEvaluator::evaluate(const AlertInputs& inputs,
                                                const platform::SysmonAlertsConfig& config) {
    Result result;
    // 冷却时间来自配置（秒 → 毫秒）；配置校验保证 >= 0，不在此重复钳制。
    const int64_t cooldown_ms = static_cast<int64_t>(config.cooldown_sec) * 1000;

    // 内存：配置关闭 → 静默清除；能力仍在但本帧未采到 → 保留活动状态（不伪造恢复）。
    if (!inputs.memory_applicable) {
        erase_kind(platform::SYSMON_ALERT_MEMORY_USED);
    } else if (is_valid_ratio(inputs.memory_used_ratio)) {
        const double used_ratio = *inputs.memory_used_ratio;
        const bool breached = used_ratio >= config.memory_used_pct / 100.0;
        transition(platform::SYSMON_ALERT_MEMORY_USED, "", breached,
                   usage_trigger_message("内存", used_ratio, config.memory_used_pct),
                   usage_recovery_message("内存", used_ratio, config.memory_used_pct), cooldown_ms,
                   inputs, result);
    }

    // 交换：与内存同构（无交换/页面文件被移除时 swap_applicable = false → 静默清除）。
    if (!inputs.swap_applicable) {
        erase_kind(platform::SYSMON_ALERT_SWAP_USED);
    } else if (is_valid_ratio(inputs.swap_used_ratio)) {
        const double used_ratio = *inputs.swap_used_ratio;
        const bool breached = used_ratio >= config.swap_used_pct / 100.0;
        transition(platform::SYSMON_ALERT_SWAP_USED, "", breached,
                   usage_trigger_message("交换", used_ratio, config.swap_used_pct),
                   usage_recovery_message("交换", used_ratio, config.swap_used_pct), cooldown_ms,
                   inputs, result);
    }

    // 关注卷剩余空间（路径感知）：只判定关注卷，非关注卷容量再低也不告警。
    if (!inputs.volumes_applicable) {
        erase_kind(platform::SYSMON_ALERT_VOLUME_FREE);
    } else {
        // 关注路径集合变化（配置改动 / 路径不再存在）时，静默清除不再被关注的卷；
        // 落卷解析本帧失败（watch_paths_collected =
        // false）不做此判断（不能区分"不再关注"与"暂时解析不到"）。
        if (inputs.watch_paths_collected) {
            const std::set<std::string> watched(inputs.watched_mounts.begin(),
                                                inputs.watched_mounts.end());
            std::erase_if(states_, [&watched](const auto& entry) {
                return entry.first.first == platform::SYSMON_ALERT_VOLUME_FREE &&
                       !watched.contains(entry.first.second);
            });
        }
        for (const auto& usage : inputs.watched_volumes) {
            if (usage.mount.empty()) {
                continue;
            }
            // 两种形态是「或」：百分比形态保护大卷的比例份额，绝对容量形态保护小卷的最小可用量；
            // 形态阈值设为 0 即关闭该形态（< 0 永不成立，无需特判）。
            bool breached = usage.free_bytes < config.volume_free_bytes;
            if (!breached && usage.total_bytes > 0) {
                const double free_ratio =
                    static_cast<double>(usage.free_bytes) / static_cast<double>(usage.total_bytes);
                breached = free_ratio < config.volume_free_pct / 100.0;
            }
            transition(platform::SYSMON_ALERT_VOLUME_FREE, usage.mount, breached,
                       volume_trigger_message(usage, config), volume_recovery_message(usage),
                       cooldown_ms, inputs, result);
        }
    }

    // 当前活动告警：按 (kind, subject) 升序（std::map 的键序即输出序），与通知解耦——
    // 冷却期内被抑制的通知不影响本列表（契约 sysmon §2.8）。
    result.active.reserve(states_.size());
    for (const auto& [key, state] : states_) {
        if (!state.active) {
            continue;
        }
        result.active.push_back(ActiveAlert{key.first, key.second, state.since_unix_sec});
    }
    return result;
}

void AlertEvaluator::reset() { states_.clear(); }

void AlertEvaluator::transition(const std::string& kind,
                                const std::string& subject,
                                bool breached,
                                const std::string& trigger_message,
                                const std::string& recovery_message,
                                int64_t cooldown_ms,
                                const AlertInputs& inputs,
                                Result& out) {
    const std::pair<std::string, std::string> key{kind, subject};
    auto it = states_.find(key);
    if (it == states_.end()) {
        if (!breached) {
            return;  // 从未越限的对象不建状态
        }
        it = states_.emplace(key, SubjectState{}).first;
    }
    auto& state = it->second;

    if (breached) {
        if (!state.active) {
            // 进入新的活动期：记录 since（墙钟，供 UI 展示"始于何时"）。
            state.active = true;
            state.since_unix_sec = inputs.now_unix_sec;
            state.notified = false;
            state.suppressed = false;
        }
        // 本次活动期已弹过（notified）或因冷却被抑制（suppressed）：阈值持续满足不重弹、
        // 被抑制的活动期也不在冷却结束后补发。
        if (state.notified || state.suppressed) {
            return;
        }
        // 冷却防抖：距上次已发出的触发通知不足 cooldown 时抑制本次整个活动期。
        const bool cooling = state.has_notified &&
                             inputs.now_monotonic_ms - state.last_notify_monotonic_ms < cooldown_ms;
        if (cooling) {
            state.suppressed = true;
            return;
        }
        state.notified = true;
        state.has_notified = true;
        state.last_notify_monotonic_ms = inputs.now_monotonic_ms;
        out.notifications.push_back(AlertNotification{kind, subject, false, trigger_message});
        return;
    }

    // 未越限：活动期结束。只有本次活动期真的弹过触发通知才发恢复通知——
    // 被冷却抑制的活动期用户从未收到触发通知，恢复也就无从谈起（不得静默"恢复"一个没通知过的告警）。
    if (state.active && state.notified) {
        out.notifications.push_back(AlertNotification{kind, subject, true, recovery_message});
    }
    state.active = false;
    state.notified = false;
    state.suppressed = false;
    state.since_unix_sec = 0;
}

void AlertEvaluator::erase_kind(const std::string& kind) {
    std::erase_if(states_, [&kind](const auto& entry) { return entry.first.first == kind; });
}

}  // namespace dztrader::sysmon
