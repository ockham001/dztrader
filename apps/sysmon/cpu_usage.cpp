#include "cpu_usage.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace dztrader::sysmon {
namespace {

/// 计算单个累计计数器的增量。
/// @return 增量；计数器重置（64 位范围内的下降）返回 std::nullopt
std::optional<uint64_t> counter_delta(uint64_t prev, uint64_t cur) noexcept {
    if (cur >= prev) {
        return cur - prev;
    }
    // 仅当两次值都在 32 位范围内时按 2^32 回绕计算（契约 sysmon §2.7）。
    if (prev <= std::numeric_limits<uint32_t>::max() &&
        cur <= std::numeric_limits<uint32_t>::max()) {
        return (kCpuCounterWrapSpan - prev) + cur;
    }
    return std::nullopt;  // 64 位计数器下降 = 重置
}

}  // namespace

std::optional<double> compute_cpu_usage(const platform::RawCpuTimes& prev,
                                        const platform::RawCpuTimes& cur) noexcept {
    // 首次采样：无基线不可计算（契约：返回不可用而非 0）。
    if (prev.total_ticks == 0 && prev.idle_ticks == 0) {
        return std::nullopt;
    }
    const auto total_delta = counter_delta(prev.total_ticks, cur.total_ticks);
    const auto idle_delta = counter_delta(prev.idle_ticks, cur.idle_ticks);
    if (!total_delta.has_value() || !idle_delta.has_value() || total_delta.value() == 0) {
        return std::nullopt;
    }
    const double idle_ratio =
        static_cast<double>(idle_delta.value()) / static_cast<double>(total_delta.value());
    return std::clamp(1.0 - idle_ratio, 0.0, 1.0);
}

}  // namespace dztrader::sysmon
