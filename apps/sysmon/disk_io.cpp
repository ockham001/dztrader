#include "disk_io.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

/// 计算单个累计计数器的增量（契约 sysmon §2.7 的回绕/重置规则）。
///
/// 本次 ≥ 上次 ⇒ 正常增量；否则仅当两次值都落在回绕跨度内时按跨度回绕，
/// 其余下降视为计数器重置（本次不可用）。
std::optional<uint64_t> counter_delta(uint64_t prev, uint64_t cur, uint64_t wrap_span) noexcept {
    if (cur >= prev) {
        return cur - prev;
    }
    if (wrap_span != 0 && prev < wrap_span && cur < wrap_span) {
        return (wrap_span - prev) + cur;
    }
    return std::nullopt;
}

/// 时间类计数器（微秒）的增量；回绕跨度为 Linux 32 位毫秒计数器的微秒表示。
std::optional<uint64_t> time_delta(uint64_t prev, uint64_t cur) noexcept {
    return counter_delta(prev, cur, kDiskTimeCounterWrapSpanUs);
}

/// 速率型派生值：Δ累计计数 / 实际经过秒数。
std::optional<double> compute_rate(uint64_t prev,
                                   uint64_t cur,
                                   uint64_t wrap_span,
                                   double elapsed_seconds) noexcept {
    const auto delta = counter_delta(prev, cur, wrap_span);
    if (!delta.has_value()) {
        return std::nullopt;  // 计数器重置：本帧速率不可用（不得沿用上一帧或输出 0）
    }
    return static_cast<double>(*delta) / elapsed_seconds;
}

/// 平均延迟（毫秒/请求）= Δ(读 + 写服务时间) / Δ(读 + 写完成请求数)。
std::optional<double> compute_latency_ms(const platform::RawDiskIoInfo& prev,
                                         const platform::RawDiskIoInfo& cur) noexcept {
    const auto read_time = time_delta(prev.read_time_us, cur.read_time_us);
    const auto write_time = time_delta(prev.write_time_us, cur.write_time_us);
    const auto read_count = counter_delta(prev.read_count, cur.read_count, kDiskCounterWrapSpan);
    const auto write_count = counter_delta(prev.write_count, cur.write_count, kDiskCounterWrapSpan);
    if (!read_time.has_value() || !write_time.has_value() || !read_count.has_value() ||
        !write_count.has_value()) {
        return std::nullopt;
    }
    const uint64_t total_ops = *read_count + *write_count;
    if (total_ops == 0) {
        return std::nullopt;  // 窗口内没有完成的请求：平均延迟无定义（不是 0）
    }
    const uint64_t total_us = *read_time + *write_time;
    return static_cast<double>(total_us) / static_cast<double>(total_ops) / 1000.0;
}

/// 利用率 = Δ忙时间 / 计量窗口，钳制到 `[0,1]`；口径异常时不可用（不钳成 100%）。
std::optional<double> compute_utilization(const platform::RawDiskIoInfo& prev,
                                          const platform::RawDiskIoInfo& cur,
                                          double elapsed_seconds) noexcept {
    if (!prev.busy_time_us.has_value() || !cur.busy_time_us.has_value()) {
        return std::nullopt;
    }
    const auto busy_us = time_delta(*prev.busy_time_us, *cur.busy_time_us);
    if (!busy_us.has_value()) {
        return std::nullopt;
    }
    const double window_us = elapsed_seconds * 1'000'000.0;
    if (static_cast<double>(*busy_us) > window_us * kDiskUtilizationTolerance) {
        // Δ忙时间超过计量窗口的容忍上限：计数器口径异常（如 Windows QueryTime 受系统时间
        // 跳变影响），本帧不可用——不用 100% 冒充满载（契约 sysmon §2.7）。
        return std::nullopt;
    }
    return std::clamp(static_cast<double>(*busy_us) / window_us, 0.0, 1.0);
}

/// 在上一帧中查找同一设备（设备集合可在帧间变化，按设备标识匹配而不是数组下标）。
const platform::RawDiskIoInfo* find_device(const std::vector<platform::RawDiskIoInfo>& prev,
                                           const std::string& device) noexcept {
    for (const auto& entry : prev) {
        if (entry.device == device) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

std::vector<platform::SysmonDiskIoInfo> compute_disk_io(
    const std::vector<platform::RawDiskIoInfo>& prev,
    const std::vector<platform::RawDiskIoInfo>& cur,
    double elapsed_seconds) noexcept {
    std::vector<platform::SysmonDiskIoInfo> results;
    results.reserve(cur.size());
    // 采样窗口非法（定时器异常/配置重排导致非正间隔）时整帧派生值不可用。
    const bool window_valid = std::isfinite(elapsed_seconds) && elapsed_seconds > 0.0;
    for (const auto& current : cur) {
        platform::SysmonDiskIoInfo info;
        info.device = current.device;
        const platform::RawDiskIoInfo* baseline =
            window_valid ? find_device(prev, current.device) : nullptr;
        if (baseline == nullptr) {
            // 首次采样 / 本帧新出现的设备：无基线，全部派生字段不可用（含瞬时 gauge，
            // 保证"首次采样整块不可用"，契约 sysmon §2.7）。
            results.push_back(std::move(info));
            continue;
        }
        info.read_bytes_per_sec = compute_rate(baseline->read_bytes, current.read_bytes,
                                               kDiskCounterWrapSpan, elapsed_seconds);
        info.write_bytes_per_sec = compute_rate(baseline->write_bytes, current.write_bytes,
                                                kDiskCounterWrapSpan, elapsed_seconds);
        info.read_iops = compute_rate(baseline->read_count, current.read_count,
                                      kDiskCounterWrapSpan, elapsed_seconds);
        info.write_iops = compute_rate(baseline->write_count, current.write_count,
                                       kDiskCounterWrapSpan, elapsed_seconds);
        info.latency_ms = compute_latency_ms(*baseline, current);
        // 队列深度是在途请求数的瞬时 gauge：直接从本帧读数取值，不做差值。
        if (current.in_flight.has_value()) {
            info.queue_depth = static_cast<double>(*current.in_flight);
        }
        info.utilization = compute_utilization(*baseline, current, elapsed_seconds);
        results.push_back(std::move(info));
    }
    return results;
}

}  // namespace dztrader::sysmon
