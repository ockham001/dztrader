#include "snapshot_builder.h"

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

/// 不可用原因（契约 sysmon §2.5 建议取值）。
constexpr const char* kReasonNotImplemented = "not_implemented";
constexpr const char* kReasonCollectFailed = "collect_failed";
constexpr const char* kReasonUnsupported = "unsupported";
constexpr const char* kReasonNoSensor = "no_sensor";
constexpr const char* kReasonDisabledByConfig = "disabled_by_config";

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

/// 把原始卷读数转换为快照条目（契约 sysmon §2.4）：
/// 容量读不到（total/free 任一为 nullopt）→ `available=false` + reason，三个容量字段为 null。
/// `used_bytes = total − free` 并钳制到 `[0, total]`，`used_ratio` 由之计算（total=0 时为 null）。
platform::SysmonVolumeInfo make_volume(const platform::RawVolumeInfo& raw, bool watched) {
    platform::SysmonVolumeInfo volume;
    volume.mount = raw.mount;
    volume.watched = watched;
    const bool readable = raw.total_bytes.has_value() && raw.free_bytes.has_value();
    volume.available = readable;
    if (!readable) {
        // 读不到不是 0：容量字段全部 null，原因取采集层给出的口径。
        volume.reason = raw.reason.empty() ? std::string(kReasonCollectFailed) : raw.reason;
        return volume;
    }
    const uint64_t total = *raw.total_bytes;
    volume.total_bytes = total;
    const uint64_t free_bytes = std::min(*raw.free_bytes, total);
    const uint64_t used_bytes = total - free_bytes;
    volume.used_bytes = used_bytes;
    if (total > 0) {
        volume.used_ratio = static_cast<double>(used_bytes) / static_cast<double>(total);
    }
    return volume;
}

/// 把原始落卷解析结果转换为快照条目（契约 sysmon §2.4）：
/// `mount` 为 nullopt ⇒ `available=false` + reason（不得猜卷）。
platform::SysmonWatchPathInfo make_watch_path(const platform::RawWatchPathInfo& raw) {
    platform::SysmonWatchPathInfo info;
    info.path = raw.path;
    info.mount = raw.mount;
    info.available = raw.mount.has_value();
    if (!info.available) {
        info.reason = raw.reason.empty() ? std::string(kReasonCollectFailed) : raw.reason;
    }
    return info;
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
                                        const TemperatureMetrics& temperature_metrics,
                                        const MemorySwapMetrics& memory_metrics,
                                        const DiskMetrics& disk_metrics,
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

    // cpu：总量/每核/饱和标记/负载/速率/每核频率。不可用一律 null 或空数组，不得用 0 冒充。
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
    // 每核频率（#73）：配置关闭优先；能力可用时输出本帧读数（未读到为 null），
    // 不沿用上一帧、也不用 0 冒充（契约 sysmon §2.3、§2.7）。
    if (!cpu_metrics.frequency_disabled && cpu_metrics.frequency_collected) {
        snapshot.cpu.core_frequencies = cpu_metrics.core_frequencies;
    } else {
        snapshot.cpu.core_frequencies = std::nullopt;
    }

    // 温度（#73）：瞬时 gauge，规则同每核频率。
    if (!temperature_metrics.disabled && temperature_metrics.collected) {
        snapshot.temperature = temperature_metrics.sensors;
    } else {
        snapshot.temperature = std::nullopt;
    }

    // 内存/交换（#74）：瞬时 gauge。本帧未读到为 null，不得沿用上一帧；配置关闭优先。
    // used 由 total - available/free 计算（契约 sysmon §2.4 的唯一口径），并做防御性钳制。
    snapshot.memory = std::nullopt;
    if (!memory_metrics.disabled && memory_metrics.collected && memory_metrics.memory.has_value() &&
        memory_metrics.memory->total_bytes > 0) {
        const auto& raw = *memory_metrics.memory;
        platform::SysmonMemoryInfo memory;
        memory.total_bytes = raw.total_bytes;
        memory.available_bytes = std::min(raw.available_bytes, raw.total_bytes);
        memory.used_bytes = memory.total_bytes - memory.available_bytes;
        memory.used_ratio =
            static_cast<double>(memory.used_bytes) / static_cast<double>(memory.total_bytes);
        if (raw.cached_bytes.has_value()) {
            memory.cached_bytes = std::min(*raw.cached_bytes, memory.total_bytes);
        }
        if (raw.buffers_bytes.has_value()) {
            memory.buffers_bytes = std::min(*raw.buffers_bytes, memory.total_bytes);
        }
        snapshot.memory = memory;
    }
    snapshot.swap = std::nullopt;
    if (!memory_metrics.swap_disabled && memory_metrics.swap_collected &&
        memory_metrics.swap.has_value()) {
        const auto& raw = *memory_metrics.swap;
        platform::SysmonSwapInfo swap;
        swap.total_bytes = raw.total_bytes;
        // 防御：free 超过 total 时钳制，保证 used 落在 [0, total]。
        const uint64_t free_bytes = std::min(raw.free_bytes, raw.total_bytes);
        swap.used_bytes = raw.total_bytes - free_bytes;
        if (raw.total_bytes > 0) {
            swap.used_ratio =
                static_cast<double>(swap.used_bytes) / static_cast<double>(raw.total_bytes);
        }
        snapshot.swap = swap;
    }

    // 卷与关注路径（#75）：落卷解析先算出来，volumes[].watched 由它决定。
    // watch_paths 不受 volumes 组开关影响（告警需要落卷信息，契约 sysmon §2.5）。
    snapshot.watch_paths = std::nullopt;
    if (disk_metrics.watch_paths.has_value()) {
        std::vector<platform::SysmonWatchPathInfo> watch_paths;
        watch_paths.reserve(disk_metrics.watch_paths->size());
        for (const auto& raw : *disk_metrics.watch_paths) {
            watch_paths.push_back(make_watch_path(raw));
        }
        snapshot.watch_paths = std::move(watch_paths);
    }
    std::set<std::string> watched_mounts;
    if (snapshot.watch_paths.has_value()) {
        for (const auto& info : *snapshot.watch_paths) {
            if (info.mount.has_value()) {
                watched_mounts.insert(*info.mount);
            }
        }
    }
    snapshot.volumes = std::nullopt;
    if (!disk_metrics.volumes_disabled && disk_metrics.volumes_collected &&
        disk_metrics.volumes.has_value()) {
        std::vector<platform::SysmonVolumeInfo> volumes;
        volumes.reserve(disk_metrics.volumes->size());
        for (const auto& raw : *disk_metrics.volumes) {
            volumes.push_back(make_volume(raw, watched_mounts.contains(raw.mount)));
        }
        snapshot.volumes = std::move(volumes);
    }

    // 共享内存目录占用（#75）：目录不可读时整个对象为 null（不得输出全 0 的对象）。
    snapshot.shm_dir = std::nullopt;
    if (disk_metrics.shm_dir_collected && disk_metrics.shm_dir.has_value()) {
        platform::SysmonShmDirInfo shm_dir;
        shm_dir.path = disk_metrics.shm_dir->path;
        shm_dir.total_bytes = disk_metrics.shm_dir->total_bytes;
        shm_dir.page_files = disk_metrics.shm_dir->page_files;
        shm_dir.oldest_file_age_sec = disk_metrics.shm_dir->oldest_file_age_sec;
        snapshot.shm_dir = shm_dir;
    }

    // 其余指标域：本版本未采集（契约 §2 定稿形状为 null）。
    snapshot.disk_io = std::nullopt;
    snapshot.network = std::nullopt;

    // availability：每帧必须覆盖全部 13 个固定键。
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
        } else if (metric == platform::SYSMON_METRIC_CPU_FREQUENCY) {
            // 配置关闭优先；能力可用但本帧未读到数值时仍为 available（数值为 null）。
            if (cpu_metrics.frequency_disabled) {
                mark_unavailable(snapshot.availability, key, kReasonDisabledByConfig);
            } else if (cpu_metrics.frequency_collected) {
                mark_available(snapshot.availability, key);
            } else {
                mark_unavailable(snapshot.availability, key, kReasonUnsupported);
            }
        } else if (metric == platform::SYSMON_METRIC_TEMPERATURE) {
            if (temperature_metrics.disabled) {
                mark_unavailable(snapshot.availability, key, kReasonDisabledByConfig);
            } else if (temperature_metrics.collected) {
                mark_available(snapshot.availability, key);
            } else if (system_info.os_family == "windows") {
                // Windows 不提供温度（无可靠 API，契约 sysmon §8）。
                mark_unavailable(snapshot.availability, key, kReasonUnsupported);
            } else {
                // Linux 无 hwmon 传感器（容器/虚拟机降级路径，契约 sysmon §2.5）。
                mark_unavailable(snapshot.availability, key, kReasonNoSensor);
            }
        } else if (metric == platform::SYSMON_METRIC_MEMORY) {
            // 配置关闭优先；能力可用但本帧未读到数值时仍为 available（数值为 null）。
            if (memory_metrics.disabled) {
                mark_unavailable(snapshot.availability, key, kReasonDisabledByConfig);
            } else if (memory_metrics.collected) {
                mark_available(snapshot.availability, key);
            } else {
                mark_unavailable(snapshot.availability, key, kReasonCollectFailed);
            }
        } else if (metric == platform::SYSMON_METRIC_SWAP) {
            if (memory_metrics.swap_disabled) {
                mark_unavailable(snapshot.availability, key, kReasonDisabledByConfig);
            } else if (!memory_metrics.swap_collected) {
                mark_unavailable(snapshot.availability, key, kReasonCollectFailed);
            } else if (!memory_metrics.swap_supported) {
                // 本机无交换分区/无页面文件（契约 sysmon §2.5：unsupported）。
                mark_unavailable(snapshot.availability, key, kReasonUnsupported);
            } else {
                mark_available(snapshot.availability, key);
            }
        } else if (metric == platform::SYSMON_METRIC_VOLUMES) {
            // 配置关闭优先；能力可用但本帧未采集到数值时仍为 available（域为 null）。
            if (disk_metrics.volumes_disabled) {
                mark_unavailable(snapshot.availability, key, kReasonDisabledByConfig);
            } else if (disk_metrics.volumes_collected) {
                mark_available(snapshot.availability, key);
            } else {
                mark_unavailable(snapshot.availability, key, kReasonCollectFailed);
            }
        } else if (metric == platform::SYSMON_METRIC_SHM_DIR) {
            if (disk_metrics.shm_dir_collected) {
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
