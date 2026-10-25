#include "thresholds.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace dztrader::sysmon {
namespace {

/// 使用率是否可判定：nullopt / 非有限值 / 越界都按「本帧不可判定」处理（保留活动状态）。
bool is_valid_ratio(const std::optional<double>& ratio) {
    return ratio.has_value() && std::isfinite(*ratio) && *ratio >= 0.0 && *ratio <= 1.0;
}

/// 比率 → 百分比文案（日志正文用；1 位小数）。
std::string format_pct(double ratio) { return std::format("{:.1f}", ratio * 100.0); }

/// 日志正文里判定对象的展示标识：卷用挂载点，其余用 kind
/// （`prose | k=v` 单行文本，英文、小写开头、无句号，AGENTS 日志门禁）。
std::string subject_field(const ThresholdKey& key) {
    return key.subject.empty() ? key.kind : key.subject;
}

/// 比率型判定对象的 `k=v` 片段（使用率 / 卷空闲百分比）。
std::string ratio_fields(double current_ratio, double threshold_ratio) {
    return std::format("current_pct={} threshold_pct={}", format_pct(current_ratio),
                       format_pct(threshold_ratio));
}

/// `cpu_saturated` 的 `k=v` 片段：契约 §2.8 要求日志能看出"当时涉及哪些核"，
/// 因此列出饱和核的 index 而不是只报一个数量（数量可由列表长度读出，反过来不行）。
std::string saturated_fields(const std::vector<uint32_t>& saturated_cores) {
    std::string list;
    for (const auto core : saturated_cores) {
        if (!list.empty()) {
            list += ',';
        }
        list += std::to_string(core);
    }
    return std::format("saturated_cores={}", list.empty() ? "-" : list);
}

/// 组装一条记录文本（状态名进 `state=`；级别由调用方按状态给出）。
std::string record_message(const ThresholdKey& key,
                           ThresholdState state,
                           std::string_view value_fields,
                           int64_t active_sec) {
    switch (state) {
        case ThresholdState::Entered:
            return std::format("threshold exceeded | subject={} state=entered {}",
                               subject_field(key), value_fields);
        case ThresholdState::Ongoing:
            return std::format(
                "threshold still exceeded | subject={} state=ongoing {} active_sec={}",
                subject_field(key), value_fields, active_sec);
        case ThresholdState::Recovered:
            return std::format("threshold recovered | subject={} state=recovered {} active_sec={}",
                               subject_field(key), value_fields, active_sec);
    }
    return {};  // 穷举所有状态；不可达
}

}  // namespace

ThresholdInputs make_threshold_inputs(const CpuMetrics& cpu_metrics,
                                      const MemorySwapMetrics& memory_metrics,
                                      const DiskMetrics& disk_metrics,
                                      int64_t now_unix_sec,
                                      int64_t now_monotonic_ms) {
    ThresholdInputs inputs;
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

    // 交换：无交换分区/页面文件（swap_supported=false）时判定不适用（不判定）。
    inputs.swap_applicable = !memory_metrics.swap_disabled && memory_metrics.swap_supported;
    if (memory_metrics.swap.has_value() && memory_metrics.swap->total_bytes > 0) {
        const auto& raw = *memory_metrics.swap;
        const uint64_t free_bytes = std::min(raw.free_bytes, raw.total_bytes);
        inputs.swap_used_ratio = static_cast<double>(raw.total_bytes - free_bytes) /
                                 static_cast<double>(raw.total_bytes);
    }

    // CPU 饱和：配置关闭时静默清除；本帧每核使用率是否可读决定"空集"是否算恢复（§2.8）。
    inputs.cpu_applicable = !cpu_metrics.disabled;
    inputs.saturated_cores = cpu_metrics.saturated_cores;
    inputs.cpu_cores_readable = std::any_of(
        cpu_metrics.cores.begin(), cpu_metrics.cores.end(),
        [](const platform::SysmonCpuCore& core) { return core.usage_ratio.has_value(); });

    // 卷（全部卷，逐卷一条判定对象）：只收容量可读且总量 > 0 的条目。
    inputs.volumes_applicable = !disk_metrics.volumes_disabled;
    if (!disk_metrics.volumes.has_value()) {
        return inputs;  // 本帧卷域整体不可读：known_mounts 保持 nullopt（不得据此清状态）
    }
    std::vector<std::string> known_mounts;
    known_mounts.reserve(disk_metrics.volumes->size());
    for (const auto& volume : *disk_metrics.volumes) {
        known_mounts.push_back(volume.mount);
        // 卷存在但容量读不到：无法判定，保留该卷的活动状态（不伪造恢复）。
        if (!volume.total_bytes.has_value() || !volume.free_bytes.has_value()) {
            continue;
        }
        VolumeUsage usage;
        usage.mount = volume.mount;
        usage.total_bytes = *volume.total_bytes;
        usage.free_bytes = std::min(*volume.free_bytes, usage.total_bytes);
        // 总量为 0 的卷无法计算百分比，不参与判定（契约 §2.8）。
        if (usage.total_bytes == 0) {
            continue;
        }
        inputs.volumes.push_back(std::move(usage));
    }
    // 记录顺序按挂载点稳定排序（采集层顺序不保证；日志顺序需确定，便于测试与人工比对）。
    std::sort(inputs.volumes.begin(), inputs.volumes.end(),
              [](const VolumeUsage& lhs, const VolumeUsage& rhs) { return lhs.mount < rhs.mount; });
    std::sort(known_mounts.begin(), known_mounts.end());
    inputs.known_mounts = std::move(known_mounts);
    return inputs;
}

bool ThresholdEvaluator::repeat_due(const SubjectState& state,
                                    int64_t now_monotonic_ms,
                                    int64_t repeat_ms) {
    if (repeat_ms <= 0) {
        return false;  // 0 = 不重记（契约 §2.8）
    }
    return now_monotonic_ms >= state.next_repeat_monotonic_ms;
}

ThresholdEvaluator::Result ThresholdEvaluator::evaluate(
    const ThresholdInputs& inputs, const platform::SysmonThresholdsConfig& config) {
    Result result;
    // 重记间隔（秒 → 毫秒）；配置校验保证 >= 0，不在此重复钳制。
    const int64_t repeat_ms = static_cast<int64_t>(config.log_repeat_sec) * 1000;

    // 内存 / 交换：能力关闭 → 静默清除；能力仍在但本帧未采到 → 保留活动状态（不伪造恢复）。
    evaluate_usage(platform::SYSMON_THRESHOLD_MEMORY_USED, inputs.memory_applicable,
                   inputs.memory_used_ratio, config.memory_used_pct, repeat_ms, inputs, result);
    evaluate_usage(platform::SYSMON_THRESHOLD_SWAP_USED, inputs.swap_applicable,
                   inputs.swap_used_ratio, config.swap_used_pct, repeat_ms, inputs, result);

    // CPU 饱和：整机一个判定对象，日志列出本帧饱和核的 index（契约 §2.8：要能看出涉及哪些核）。
    // §2.8：不得按"空集"判恢复——每核序列整体未采到时 saturated 全部为 false，
    // 那只是"本帧无信息"；只有本帧每核使用率确实可读时，空集才等于恢复。
    if (!inputs.cpu_applicable) {
        erase_kind(platform::SYSMON_THRESHOLD_CPU_SATURATED);
    } else if (inputs.cpu_cores_readable) {
        const ThresholdKey key{platform::SYSMON_THRESHOLD_CPU_SATURATED, ""};
        const TransitionOutcome outcome =
            transition(key, !inputs.saturated_cores.empty(), repeat_ms, inputs);
        if (outcome.produced) {
            // 当前值记录饱和核数（无量纲）；阈值留空——饱和判定在采集阶段由
            // thresholds.cpu_sustain_* 完成，越限层只看"有没有饱和核"。
            result.records.push_back(make_record(
                key, outcome, static_cast<double>(inputs.saturated_cores.size()), std::nullopt,
                record_message(key, outcome.state, saturated_fields(inputs.saturated_cores),
                               outcome.active_sec)));
        }
    }

    // 卷（全部卷，逐卷一条判定对象）：只按空闲百分比判定，没有绝对容量形态（ADR 0034）。
    if (!inputs.volumes_applicable) {
        erase_kind(platform::SYSMON_THRESHOLD_VOLUME_FREE);
    } else {
        // 本帧卷域可读时，从 volumes[] 消失的卷静默清除（卸载/热拔：判定对象不再存在，
        // 不是"指标回到阈值以下"）。卷域整体不可读（known_mounts = nullopt）时不做此判断
        // ——无法区分"卷消失"与"暂时枚举不到"。
        if (inputs.known_mounts.has_value()) {
            const std::vector<std::string>& known = *inputs.known_mounts;
            std::erase_if(states_, [&known](const auto& entry) {
                return entry.first.kind == platform::SYSMON_THRESHOLD_VOLUME_FREE &&
                       !std::binary_search(known.begin(), known.end(), entry.first.subject);
            });
        }
        const double threshold_ratio = config.volume_free_pct / 100.0;
        for (const auto& usage : inputs.volumes) {
            if (usage.mount.empty()) {
                continue;
            }
            const ThresholdKey key{platform::SYSMON_THRESHOLD_VOLUME_FREE, usage.mount};
            const double free_ratio =
                static_cast<double>(usage.free_bytes) / static_cast<double>(usage.total_bytes);
            // 卷是**空闲**百分比阈值：free_pct < volume_free_pct 即越限；
            // 恰好等于阈值不算越限、并且满足恢复（契约 §2.8 的边界口径）。
            const TransitionOutcome outcome =
                transition(key, free_ratio < threshold_ratio, repeat_ms, inputs);
            if (outcome.produced) {
                result.records.push_back(make_record(
                    key, outcome, free_ratio, threshold_ratio,
                    record_message(key, outcome.state, ratio_fields(free_ratio, threshold_ratio),
                                   outcome.active_sec)));
            }
        }
    }

    // 记录按 (kind, subject) 升序输出：调用方的卷顺序不保证（采集层顺序、热插拔增删都会变），
    // 而日志顺序应当确定——否则同样的机器状态每次重启都换一个行序，人工比对与测试都不稳。
    std::sort(result.records.begin(), result.records.end(),
              [](const ThresholdLogRecord& lhs, const ThresholdLogRecord& rhs) {
                  return lhs.key < rhs.key;
              });
    return result;
}

void ThresholdEvaluator::evaluate_usage(const char* kind,
                                        bool applicable,
                                        const std::optional<double>& used_ratio,
                                        double threshold_pct,
                                        int64_t repeat_ms,
                                        const ThresholdInputs& inputs,
                                        Result& out) {
    if (!applicable) {
        erase_kind(kind);
        return;
    }
    if (!is_valid_ratio(used_ratio)) {
        return;  // 本帧未采到：保留活动状态，不伪造恢复
    }
    const ThresholdKey key{kind, ""};
    const double ratio = *used_ratio;
    const double threshold_ratio = threshold_pct / 100.0;
    const bool breached = ratio >= threshold_ratio;  // 恰好等于阈值算进入越限
    const TransitionOutcome outcome = transition(key, breached, repeat_ms, inputs);
    if (outcome.produced) {
        out.records.push_back(
            make_record(key, outcome, ratio, threshold_ratio,
                        record_message(key, outcome.state, ratio_fields(ratio, threshold_ratio),
                                       outcome.active_sec)));
    }
}

ThresholdEvaluator::TransitionOutcome ThresholdEvaluator::transition(
    const ThresholdKey& key, bool breached, int64_t repeat_ms, const ThresholdInputs& inputs) {
    TransitionOutcome outcome;
    auto it = states_.find(key);
    if (it == states_.end()) {
        if (!breached) {
            return outcome;  // 从未越限的对象不建状态
        }
        it = states_.emplace(key, SubjectState{}).first;
    }
    auto& state = it->second;

    if (breached) {
        if (!state.active) {
            // 进入新的活动期：墙钟记起点（供日志表达"始于何时"），单调时钟作重记基准。
            state.active = true;
            state.since_unix_sec = inputs.now_unix_sec;
            state.since_monotonic_ms = inputs.now_monotonic_ms;
            // 第一条重记出现在"进入时刻 + 一个间隔"；之后每个间隔一条。
            state.next_repeat_monotonic_ms = inputs.now_monotonic_ms + repeat_ms;
            outcome.state = ThresholdState::Entered;
            outcome.produced = true;
            outcome.active_sec = 0;
            return outcome;
        }
        // 活动期内：阈值持续满足不重记，但每满间隔记一条「仍越限」。
        // 里程碑按**活动期起点**对齐（契约 §2.8）：第 N 条在 since + N×间隔，
        // 因此长时间运行也不会把重记时刻漂到周期的别处。
        if (repeat_due(state, inputs.now_monotonic_ms, repeat_ms)) {
            outcome.state = ThresholdState::Ongoing;
            outcome.produced = true;
            outcome.active_sec = (inputs.now_monotonic_ms - state.since_monotonic_ms) / 1000;
            // 记下已到达的最新里程碑：下一条的判据是「距 since 又满一个间隔」。
            state.next_repeat_monotonic_ms += repeat_ms;
            // 时钟跳变等导致一次性落后多个间隔时，不补发历史记录（一条"仍越限"已足够
            // 表达"它还在持续"），直接把里程碑推进到当前时刻之后的第一个间隔。
            while (state.next_repeat_monotonic_ms <= inputs.now_monotonic_ms) {
                state.next_repeat_monotonic_ms += repeat_ms;
            }
        }
        return outcome;
    }

    // 未越限：活动期结束。恢复只在**真正读到低于阈值**时发生（读不到不进这里）。
    if (state.active) {
        outcome.state = ThresholdState::Recovered;
        outcome.produced = true;
        outcome.active_sec = (inputs.now_monotonic_ms - state.since_monotonic_ms) / 1000;
    }
    states_.erase(it);
    return outcome;
}

ThresholdLogRecord ThresholdEvaluator::make_record(const ThresholdKey& key,
                                                   const TransitionOutcome& outcome,
                                                   double current_value,
                                                   std::optional<double> threshold_value,
                                                   std::string message) {
    ThresholdLogRecord record;
    record.key = key;
    record.state = outcome.state;
    // 进入越限是 warning（需要事后能一眼看到"什么时候开始坏"），重记与恢复是 info
    // （它们的信息量在"持续了多久"，不需要抬高级别）。
    record.level =
        outcome.state == ThresholdState::Entered ? spdlog::level::warn : spdlog::level::info;
    record.current_value = current_value;
    record.threshold_value = threshold_value;
    record.active_sec = outcome.active_sec;
    record.message = std::move(message);
    return record;
}

void ThresholdEvaluator::erase_kind(const std::string& kind) {
    std::erase_if(states_, [&kind](const auto& entry) { return entry.first.kind == kind; });
}

}  // namespace dztrader::sysmon
