#include "snapshot_builder.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

/// 不可用原因（契约 sysmon §2.5 建议取值）。
constexpr const char* kReasonNotImplemented = "not_implemented";
constexpr const char* kReasonCollectFailed = "collect_failed";

/// 跨平台语义提示的键：按指标键命名（契约 sysmon §2.5）。
constexpr const char* kHintKeyLoadAverage = "cpu.load_average";

/// Windows 负载代理的语义提示：明示与 Unix load average 不是同一个量，前端不得同轴比较。
/// 英文、小写开头、无句号（与契约 reason 文案风格一致）。
constexpr const char* kProcessorQueueHint =
    "processor queue length proxy, not a unix load average; do not compare the two on the same "
    "axis";

void mark_available(platform::SysmonAvailability& availability, const char* key) {
    platform::SysmonAvailabilityEntry entry;
    entry.available = true;
    availability.metrics[key] = entry;
}

void mark_unavailable(platform::SysmonAvailability& availability,
                      const char* key,
                      const char* reason) {
    platform::SysmonAvailabilityEntry entry;
    entry.available = false;
    entry.reason = reason;
    availability.metrics[key] = entry;
}

void mark_not_implemented(platform::SysmonAvailability& availability, const char* key) {
    mark_unavailable(availability, key, kReasonNotImplemented);
}

/// 人类可读平台描述：Windows 必须含 build 号（区分 Win10/Win11），Linux 带内核 release。
std::string make_platform_description(const platform::RawSystemInfo& info) {
    if (info.os_family == "windows") {
        return std::format("{} (build {})", info.os_version, info.os_build);
    }
    if (info.os_family == "linux") {
        return std::format("{} (kernel {})", info.os_version, info.kernel_version);
    }
    // 编译期/启动期基线检查保证不会走到这里；兜底保持非空字符串。
    return info.os_version.empty() ? std::string("unknown") : info.os_version;
}

}  // namespace

platform::SysmonSnapshot build_snapshot(std::string_view source,
                                        const platform::RawSystemInfo& system_info,
                                        const CpuMetrics& cpu_metrics,
                                        int32_t measure_interval_ms,
                                        int64_t last_sample_unix_ms,
                                        uint64_t missed_ticks,
                                        int64_t now_unix_ms) {
    platform::SysmonSnapshot snapshot;
    snapshot.source = std::string(source);
    snapshot.timestamp = now_unix_ms;

    // sampling：计量元信息（缺口标记用于区分"机器空闲"与"监控停顿"）。
    snapshot.sampling.measure_interval_ms = measure_interval_ms;
    snapshot.sampling.last_sample_unix_ms = last_sample_unix_ms;
    snapshot.sampling.missed_ticks = missed_ticks;

    // platform：原始系统信息一一对应。
    snapshot.platform.family = system_info.os_family;
    snapshot.platform.version = system_info.os_version;
    snapshot.platform.kernel = system_info.kernel_version;
    snapshot.platform.arch = system_info.arch;
    snapshot.platform.build = system_info.os_build;

    // system：uptime 由快照时刻与启动时间计算，负数按 0（时钟回拨保护）。
    snapshot.system.hostname = system_info.hostname;
    snapshot.system.cpu_model = system_info.cpu_model;
    snapshot.system.logical_cores = system_info.logical_cores;
    snapshot.system.boot_time_unix_sec = system_info.boot_time_unix_sec;
    const int64_t now_sec = now_unix_ms / 1000;
    snapshot.system.uptime_sec =
        now_sec > system_info.boot_time_unix_sec ? now_sec - system_info.boot_time_unix_sec : 0;

    // cpu：总量/每核/饱和标记/负载/速率。不可用一律 null 或空数组，不得用 0 冒充。
    snapshot.cpu.total.usage_ratio = cpu_metrics.total_usage_ratio;
    if (cpu_metrics.cores_collected) {
        snapshot.cpu.cores = cpu_metrics.cores;
        snapshot.cpu.saturated_cores = cpu_metrics.saturated_cores;
        // 契约 sysmon §2.3：每核序列可用时长度恒等于逻辑核数（热插拔后以本次序列为准）。
        if (!snapshot.cpu.cores.empty()) {
            snapshot.system.logical_cores = static_cast<uint32_t>(snapshot.cpu.cores.size());
        }
    } else {
        snapshot.cpu.cores.clear();
        snapshot.cpu.saturated_cores.clear();
    }
    snapshot.cpu.load_average = cpu_metrics.load_average;
    if (cpu_metrics.rates_collected) {
        snapshot.cpu.context_switches_per_sec = cpu_metrics.context_switches_per_sec;
        snapshot.cpu.interrupts_per_sec = cpu_metrics.interrupts_per_sec;
    } else {
        snapshot.cpu.context_switches_per_sec = std::nullopt;
        snapshot.cpu.interrupts_per_sec = std::nullopt;
    }

    // 其余指标域：本版本未采集（契约 §2 定稿形状为 null）。
    snapshot.memory = std::nullopt;
    snapshot.swap = std::nullopt;
    snapshot.volumes = std::nullopt;
    snapshot.disk_io = std::nullopt;
    snapshot.network = std::nullopt;
    snapshot.shm_dir = std::nullopt;

    // availability：每帧必须覆盖全部 11 个固定键。
    snapshot.availability.platform_description = make_platform_description(system_info);
    for (const char* key : platform::SYSMON_METRIC_KEYS) {
        const std::string_view metric(key);
        if (metric == platform::SYSMON_METRIC_SYSTEM ||
            metric == platform::SYSMON_METRIC_CPU_TOTAL) {
            mark_available(snapshot.availability, key);
        } else if (metric == platform::SYSMON_METRIC_CPU_CORES) {
            if (cpu_metrics.cores_collected) {
                mark_available(snapshot.availability, key);
            } else {
                mark_unavailable(snapshot.availability, key, kReasonCollectFailed);
            }
        } else if (metric == platform::SYSMON_METRIC_CPU_LOAD_AVERAGE) {
            if (cpu_metrics.load_collected) {
                mark_available(snapshot.availability, key);
            } else {
                mark_unavailable(snapshot.availability, key, kReasonCollectFailed);
            }
        } else if (metric == platform::SYSMON_METRIC_CPU_CONTEXT_SWITCHES) {
            if (cpu_metrics.rates_collected) {
                mark_available(snapshot.availability, key);
            } else {
                mark_unavailable(snapshot.availability, key, kReasonCollectFailed);
            }
        } else {
            mark_not_implemented(snapshot.availability, key);
        }
    }

    // semantic_hints：Windows 负载代理必须说明口径不同（契约 sysmon §2.5）。
    snapshot.availability.semantic_hints.clear();
    if (snapshot.cpu.load_average.has_value() &&
        snapshot.cpu.load_average->semantic == platform::SYSMON_LOAD_SEMANTIC_PROCESSOR_QUEUE) {
        snapshot.availability.semantic_hints[kHintKeyLoadAverage] = kProcessorQueueHint;
    }

    return snapshot;
}

}  // namespace dztrader::sysmon
