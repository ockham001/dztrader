#include "disk_io.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cpu_usage.h"

namespace dztrader::sysmon {
namespace {

/// 平均延迟（毫秒/请求）= Δ(读 + 写服务时间) / Δ(读 + 写完成请求数)。
std::optional<double> compute_latency_ms(const platform::RawDiskIoInfo& prev,
                                         const platform::RawDiskIoInfo& cur) noexcept {
    const auto read_time = counter_delta(prev.read_time_us, cur.read_time_us, cur.time_wrap_span);
    const auto write_time =
        counter_delta(prev.write_time_us, cur.write_time_us, cur.time_wrap_span);
    const auto read_count = counter_delta(prev.read_count, cur.read_count, cur.count_wrap_span);
    const auto write_count = counter_delta(prev.write_count, cur.write_count, cur.count_wrap_span);
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
    const auto busy_us = counter_delta(*prev.busy_time_us, *cur.busy_time_us, cur.time_wrap_span);
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
        // 字节计数器两平台均为 64 位：下降一律视为计数器重置（跨度 0）。
        info.read_bytes_per_sec =
            compute_rate(baseline->read_bytes, current.read_bytes, elapsed_seconds, 0);
        info.write_bytes_per_sec =
            compute_rate(baseline->write_bytes, current.write_bytes, elapsed_seconds, 0);
        // 计数类与时间类的回绕跨度随采样携带（平台原生位宽，契约 sysmon §2.7）。
        info.read_iops = compute_rate(baseline->read_count, current.read_count, elapsed_seconds,
                                      current.count_wrap_span);
        info.write_iops = compute_rate(baseline->write_count, current.write_count, elapsed_seconds,
                                       current.count_wrap_span);
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
