#include "cpu_usage.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dztrader::sysmon {
namespace {

/// 由一组累计计数器计算使用率 = 1 − Δidle/Δtotal，钳制到 [0, 1]。
std::optional<double> usage_from_counters(uint64_t prev_total,
                                          uint64_t prev_idle,
                                          uint64_t cur_total,
                                          uint64_t cur_idle) noexcept {
    const auto total_delta = counter_delta(prev_total, cur_total, kCpuCounterWrapSpan);
    const auto idle_delta = counter_delta(prev_idle, cur_idle, kCpuCounterWrapSpan);
    if (!total_delta.has_value() || !idle_delta.has_value() || total_delta.value() == 0) {
        return std::nullopt;
    }
    const double idle_ratio =
        static_cast<double>(idle_delta.value()) / static_cast<double>(total_delta.value());
    return std::clamp(1.0 - idle_ratio, 0.0, 1.0);
}

}  // namespace

std::optional<uint64_t> counter_delta(uint64_t prev, uint64_t cur, uint64_t wrap_span) noexcept {
    if (cur >= prev) {
        return cur - prev;
    }
    // 仅当该计数器有 32 位回绕跨度且两次值都落在跨度内时按跨度回绕计算（契约 sysmon §2.7）。
    if (wrap_span != 0 && prev < wrap_span && cur < wrap_span) {
        return (wrap_span - prev) + cur;
    }
    return std::nullopt;  // 64 位计数器下降 / 超出跨度 = 计数器重置
}

std::optional<double> compute_cpu_usage(const platform::RawCpuTimes& prev,
                                        const platform::RawCpuTimes& cur) noexcept {
    // 首次采样：无基线不可计算（契约：返回不可用而非 0）。
    if (prev.total_ticks == 0 && prev.idle_ticks == 0) {
        return std::nullopt;
    }
    return usage_from_counters(prev.total_ticks, prev.idle_ticks, cur.total_ticks, cur.idle_ticks);
}

std::vector<std::optional<double>> compute_core_usages(
    const std::vector<platform::RawCpuCoreTimes>& prev,
    const std::vector<platform::RawCpuCoreTimes>& cur) noexcept {
    std::vector<std::optional<double>> usages(cur.size());
    // 核序列长度或 index 序列变化（热插拔/处理器组变化）= 基线失效，本次全部不可用。
    if (prev.size() != cur.size()) {
        return usages;
    }
    for (std::size_t i = 0; i < cur.size(); ++i) {
        if (prev[i].index != cur[i].index) {
            return usages;
        }
    }
    for (std::size_t i = 0; i < cur.size(); ++i) {
        if (prev[i].total_ticks == 0 && prev[i].idle_ticks == 0) {
            continue;  // 该核首次采样：不可用，等待下一个样本建立基线
        }
        usages[i] = usage_from_counters(prev[i].total_ticks, prev[i].idle_ticks, cur[i].total_ticks,
                                        cur[i].idle_ticks);
    }
    return usages;
}

std::optional<double> compute_rate(uint64_t prev,
                                   uint64_t cur,
                                   double elapsed_seconds,
                                   uint64_t wrap_span) noexcept {
    if (!(elapsed_seconds > 0.0)) {
        return std::nullopt;
    }
    const auto delta = counter_delta(prev, cur, wrap_span);
    if (!delta.has_value()) {
        return std::nullopt;  // 计数器重置：本次速率不可用
    }
    return static_cast<double>(delta.value()) / elapsed_seconds;
}

}  // namespace dztrader::sysmon
