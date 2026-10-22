#ifndef DZTRADER_PLATFORM_SYSMON_H_
#define DZTRADER_PLATFORM_SYSMON_H_

/**
 * @file sysmon.h
 * @brief dzsysmon JSON 帧类型与校验（契约 sysmon）。
 *
 * 帧：
 * - `DZ_FRAME_RTN_SYSMON_SNAPSHOT`：实时快照推送（无 `instance_id`，发送方固定 `dzsysmon`）
 * - `DZ_FRAME_SET_SYSMON_CONFIG`：运行期配置修改（定向，`instance_id=dzsysmon`）
 * - `DZ_FRAME_RTN_SYSMON_CONFIG`：回推当前生效配置（定向，`instance_id=dzsysmon`）
 *
 * 本文件是**类型层真相源**：JSON 字段名、类型、可空性、取值范围以契约 sysmon 为准，
 * 本头保持一一对应；契约变更必须同步本文件（general §11.3）。
 *
 * `null` 语义（契约 sysmon）：仅快照中"该指标暂不可用/尚未实现"的位置允许显式 `null`；
 * 其余位置出现 `null` 均为校验失败（general §8）。
 */

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace dztrader::platform {

// ===== 可用性映射键（契约 sysmon §可用性描述）=====
// 该集合是发送方与消费方的稳定契约：每帧快照的 availability.metrics 必须覆盖全部键。
inline constexpr const char* SYSMON_METRIC_SYSTEM = "system";
inline constexpr const char* SYSMON_METRIC_CPU_TOTAL = "cpu.total";
inline constexpr const char* SYSMON_METRIC_CPU_CORES = "cpu.cores";
inline constexpr const char* SYSMON_METRIC_CPU_LOAD_AVERAGE = "cpu.load_average";
inline constexpr const char* SYSMON_METRIC_CPU_CONTEXT_SWITCHES = "cpu.context_switches";
inline constexpr const char* SYSMON_METRIC_CPU_FREQUENCY = "cpu.frequency";
inline constexpr const char* SYSMON_METRIC_MEMORY = "memory";
inline constexpr const char* SYSMON_METRIC_SWAP = "swap";
inline constexpr const char* SYSMON_METRIC_VOLUMES = "volumes";
inline constexpr const char* SYSMON_METRIC_DISK_IO = "disk_io";
inline constexpr const char* SYSMON_METRIC_NETWORK = "network";
inline constexpr const char* SYSMON_METRIC_SHM_DIR = "shm_dir";
inline constexpr const char* SYSMON_METRIC_TEMPERATURE = "temperature";

/// 可用性映射的固定键集合全集（契约 sysmon §2.5；每帧快照必须全覆盖）。
inline constexpr std::array<const char*, 13> SYSMON_METRIC_KEYS = {
    SYSMON_METRIC_SYSTEM,
    SYSMON_METRIC_CPU_TOTAL,
    SYSMON_METRIC_CPU_CORES,
    SYSMON_METRIC_CPU_LOAD_AVERAGE,
    SYSMON_METRIC_CPU_CONTEXT_SWITCHES,
    SYSMON_METRIC_CPU_FREQUENCY,
    SYSMON_METRIC_MEMORY,
    SYSMON_METRIC_SWAP,
    SYSMON_METRIC_VOLUMES,
    SYSMON_METRIC_DISK_IO,
    SYSMON_METRIC_NETWORK,
    SYSMON_METRIC_SHM_DIR,
    SYSMON_METRIC_TEMPERATURE,
};

/// 配置 payload / 配置文件的业务段全集（契约 sysmon §3.1；顺序固定）。
inline constexpr std::array<const char*, 6> SYSMON_CONFIG_SECTIONS = {
    "sampling", "disk", "network", "history", "realtime", "alerts"};

/// 配置 `sampling.groups` 的 B 组指标键（契约 sysmon §3.1）：温度与每核频率。
inline constexpr const char* SYSMON_GROUP_TEMPERATURE = "temperature";
inline constexpr const char* SYSMON_GROUP_CPU_FREQUENCY = "cpu_frequency";

// ===== 快照 payload（契约 sysmon §快照）=====

/// 采样元信息。
struct SysmonSamplingMeta {
    int32_t measure_interval_ms = 0;  ///< 计量间隔（毫秒），固定基线 1000
    int64_t last_sample_unix_ms = 0;  ///< 最近一次计量成功的墙钟时间（Unix 毫秒）
    uint64_t missed_ticks = 0;        ///< 累计错过的计量 tick 数（缺口标记）
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonSamplingMeta,
                                                measure_interval_ms,
                                                last_sample_unix_ms,
                                                missed_ticks)
};

/// 平台信息。`build` 为区分 Windows 10/11 的 build 号（Linux 为 0）。
struct SysmonPlatformInfo {
    std::string family;   ///< "windows" / "linux"
    std::string version;  ///< 人类可读版本
    std::string kernel;   ///< Linux 内核 release；Windows 为 NT 版本号
    std::string arch;     ///< "x86_64"
    uint32_t build = 0;   ///< Windows build；Linux 0
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SysmonPlatformInfo, family, version, kernel, arch, build)
};

/// 系统信息。
struct SysmonSystemInfo {
    std::string hostname;
    std::string cpu_model;
    uint32_t logical_cores = 0;
    int64_t boot_time_unix_sec = 0;
    int64_t uptime_sec = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SysmonSystemInfo, hostname, cpu_model, logical_cores, boot_time_unix_sec, uptime_sec)
};

/// CPU 总使用率。`usage_ratio` 为 null 表示首次采样或计数器不可用（不得用 0 冒充）。
struct SysmonCpuTotal {
    std::optional<double> usage_ratio;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonCpuTotal, usage_ratio)
};

/// 单核使用率与饱和标记。
struct SysmonCpuCore {
    uint32_t index = 0;
    std::optional<double> usage_ratio;
    bool saturated = false;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonCpuCore, index, usage_ratio, saturated)
};

/// 单核当前频率（契约 sysmon §2.3；`index` 与 `cpu.cores[].index` 同源）。
struct SysmonCoreFrequency {
    uint32_t index = 0;  ///< OS CPU 编号（不保证连续）
    uint32_t mhz = 0;    ///< 当前频率（MHz，> 0）
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonCoreFrequency, index, mhz)
};

/// 系统负载（跨平台语义不同，`semantic` 说明口径）。
struct SysmonLoadAverage {
    std::optional<double> one;
    std::optional<double> five;
    std::optional<double> fifteen;
    std::string semantic;  ///< "load_average"（Linux）/ "processor_queue"（Windows 代理量）
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonLoadAverage, one, five, fifteen, semantic)
};

/// CPU 区块。
struct SysmonCpuInfo {
    SysmonCpuTotal total;
    std::vector<SysmonCpuCore> cores;
    /// 每核当前频率；null = 本平台不提供/不可用（契约 sysmon §2.3）。
    std::optional<std::vector<SysmonCoreFrequency>> core_frequencies;
    std::vector<uint32_t> saturated_cores;
    std::optional<SysmonLoadAverage> load_average;  ///< null = 本平台不提供/不可用
    std::optional<double> context_switches_per_sec;
    std::optional<double> interrupts_per_sec;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonCpuInfo,
                                                total,
                                                cores,
                                                core_frequencies,
                                                saturated_cores,
                                                load_average,
                                                context_switches_per_sec,
                                                interrupts_per_sec)
};

/// 内存。唯一口径：`used_bytes = total_bytes - available_bytes`。
struct SysmonMemoryInfo {
    uint64_t total_bytes = 0;
    uint64_t available_bytes = 0;
    uint64_t used_bytes = 0;
    std::optional<double> used_ratio;  ///< used_bytes / total_bytes；total=0 时 null
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SysmonMemoryInfo, total_bytes, available_bytes, used_bytes, used_ratio)
};

/// 交换分区/页文件。total_bytes=0 表示本机无交换（availability 标记不可用）。
struct SysmonSwapInfo {
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    std::optional<double> used_ratio;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonSwapInfo, total_bytes, used_bytes, used_ratio)
};

/// 卷容量。
struct SysmonVolumeInfo {
    std::string mount;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    std::optional<double> used_ratio;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SysmonVolumeInfo, mount, total_bytes, used_bytes, used_ratio)
};

/// 每物理盘 I/O 速率（不可用时对应字段为 null）。
struct SysmonDiskIoInfo {
    std::string device;
    std::optional<double> read_bytes_per_sec;
    std::optional<double> write_bytes_per_sec;
    std::optional<double> read_iops;
    std::optional<double> write_iops;
    std::optional<double> latency_ms;
    std::optional<double> queue_depth;
    std::optional<double> utilization;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonDiskIoInfo,
                                                device,
                                                read_bytes_per_sec,
                                                write_bytes_per_sec,
                                                read_iops,
                                                write_iops,
                                                latency_ms,
                                                queue_depth,
                                                utilization)
};

/// 每网卡速率与累计量。
struct SysmonNetworkInfo {
    /// 网卡名。C++ 字段不叫 `interface`：Windows SDK 的 COM 头（objbase.h）把它
    /// 定义成 `struct` 宏，包含顺序变化即编译失败；JSON 键仍是契约规定的 `interface`。
    std::string interface_name;
    uint64_t rx_bytes_total = 0;
    uint64_t tx_bytes_total = 0;
    std::optional<double> rx_bytes_per_sec;
    std::optional<double> tx_bytes_per_sec;
    std::optional<double> rx_errors_per_sec;
    std::optional<double> tx_errors_per_sec;
    std::optional<double> rx_drops_per_sec;
    std::optional<double> tx_drops_per_sec;
    bool is_virtual = false;
};

// 手写序列化：JSON 键 `interface` ↔ C++ 字段 `interface_name`（Windows `interface` 宏冲突）。
inline void to_json(nlohmann::json& j, const SysmonNetworkInfo& n) {
    j = nlohmann::json{{"interface", n.interface_name},
                       {"rx_bytes_total", n.rx_bytes_total},
                       {"tx_bytes_total", n.tx_bytes_total},
                       {"rx_bytes_per_sec", n.rx_bytes_per_sec},
                       {"tx_bytes_per_sec", n.tx_bytes_per_sec},
                       {"rx_errors_per_sec", n.rx_errors_per_sec},
                       {"tx_errors_per_sec", n.tx_errors_per_sec},
                       {"rx_drops_per_sec", n.rx_drops_per_sec},
                       {"tx_drops_per_sec", n.tx_drops_per_sec},
                       {"is_virtual", n.is_virtual}};
}

inline void from_json(const nlohmann::json& j, SysmonNetworkInfo& n) {
    n.interface_name = j.at("interface").get<std::string>();
    n.rx_bytes_total = j.value("rx_bytes_total", uint64_t{0});
    n.tx_bytes_total = j.value("tx_bytes_total", uint64_t{0});
    const auto optional_number = [&j](const char* key) -> std::optional<double> {
        if (!j.contains(key) || j[key].is_null()) {
            return std::nullopt;
        }
        return j[key].get<double>();
    };
    n.rx_bytes_per_sec = optional_number("rx_bytes_per_sec");
    n.tx_bytes_per_sec = optional_number("tx_bytes_per_sec");
    n.rx_errors_per_sec = optional_number("rx_errors_per_sec");
    n.tx_errors_per_sec = optional_number("tx_errors_per_sec");
    n.rx_drops_per_sec = optional_number("rx_drops_per_sec");
    n.tx_drops_per_sec = optional_number("tx_drops_per_sec");
    n.is_virtual = j.value("is_virtual", false);
}

/// 共享内存目录占用。
struct SysmonShmDirInfo {
    std::string path;
    uint64_t total_bytes = 0;
    uint64_t page_files = 0;
    std::optional<int64_t> oldest_file_age_sec;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SysmonShmDirInfo, path, total_bytes, page_files, oldest_file_age_sec)
};

/// 单个温度传感器读数（契约 sysmon §2.4；温度为瞬时 gauge，不是累计计数器）。
struct SysmonTemperatureInfo {
    std::string sensor;    ///< 传感器稳定标识（Linux hwmon：`<name>#<hwmonN>:<label>`）
    double celsius = 0.0;  ///< 摄氏温度
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonTemperatureInfo, sensor, celsius)
};

/// 单个指标的可用性。
struct SysmonAvailabilityEntry {
    bool available = false;
    std::optional<std::string> reason;  ///< 不可用原因；available=true 时省略
};

// 手写序列化：`reason` 在可用时省略（契约 sysmon §校验：除快照指标位置外禁止显式 null）。
inline void to_json(nlohmann::json& j, const SysmonAvailabilityEntry& e) {
    j = nlohmann::json{{"available", e.available}};
    if (e.reason && !e.reason->empty()) {
        j["reason"] = *e.reason;
    }
}

inline void from_json(const nlohmann::json& j, SysmonAvailabilityEntry& e) {
    if (!j.is_object()) {
        throw std::runtime_error("availability entry must be an object");
    }
    if (!j.contains("available") || !j["available"].is_boolean()) {
        throw std::runtime_error("availability entry missing boolean field: available");
    }
    e.available = j["available"].get<bool>();
    if (j.contains("reason") && !j["reason"].is_null()) {
        e.reason = j["reason"].get<std::string>();
    } else {
        e.reason = std::nullopt;
    }
}

/// 可用性描述（平台描述 + 按指标可用性映射 + 语义提示）。
struct SysmonAvailability {
    std::string platform_description;  ///< 含 Windows build 号
    std::map<std::string, SysmonAvailabilityEntry> metrics;
    std::map<std::string, std::string> semantic_hints;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonAvailability,
                                                platform_description,
                                                metrics,
                                                semantic_hints)
};

/// RTN_SYSMON_SNAPSHOT payload（契约 sysmon §快照）。
struct SysmonSnapshot {
    std::string source;     ///< 发送方进程名（无 instance_id 帧的来源显式携带，general §5）
    int64_t timestamp = 0;  ///< 快照生成时刻（Unix 毫秒）
    SysmonSamplingMeta sampling;
    SysmonPlatformInfo platform;
    SysmonSystemInfo system;
    SysmonCpuInfo cpu;
    /// 温度传感器读数；null = 本平台不提供/无传感器/不可用（契约 sysmon §2.4）。
    std::optional<std::vector<SysmonTemperatureInfo>> temperature;
    std::optional<SysmonMemoryInfo> memory;  ///< null = 本版本未采集/不可用
    std::optional<SysmonSwapInfo> swap;
    std::optional<std::vector<SysmonVolumeInfo>> volumes;
    std::optional<std::vector<SysmonDiskIoInfo>> disk_io;
    std::optional<std::vector<SysmonNetworkInfo>> network;
    std::optional<SysmonShmDirInfo> shm_dir;
    SysmonAvailability availability;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonSnapshot,
                                                source,
                                                timestamp,
                                                sampling,
                                                platform,
                                                system,
                                                cpu,
                                                temperature,
                                                memory,
                                                swap,
                                                volumes,
                                                disk_io,
                                                network,
                                                shm_dir,
                                                availability)

    /// 校验快照 payload（契约 sysmon §校验；general §8）。
    /// @return 通过返回 std::nullopt；失败返回英文错误消息（小写开头、无句号）。
    [[nodiscard]] static std::optional<std::string> validate(const nlohmann::json& payload);
};

// ===== 配置 payload（契约 sysmon §配置）=====

/// 采样配置。
struct SysmonSamplingConfig {
    int32_t measure_interval_ms = 1000;  ///< 计量间隔，[100, 60000]
    int32_t record_interval_sec = 60;    ///< 记录间隔，[1, 86400]
    int32_t phase_offset_sec = 3;        ///< 记录相位偏移，[0, 86400]
    /// 启用的指标大类（契约 #41 A/B 组）：cpu/memory/swap/volumes/disk_io/network/
    /// temperature/cpu_frequency。温度与每核频率属 B 组：Linux 默认开，Windows 默认关
    /// （平台不可靠或不存在，用户可在配置中显式开启；本版本 Windows 采集器仍报告不可用）。
    std::map<std::string, bool> groups;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SysmonSamplingConfig, measure_interval_ms, record_interval_sec, phase_offset_sec, groups)

    /// 默认启用的指标大类全集（平台相关默认值，契约 sysmon §3.1）。
    static std::map<std::string, bool> default_groups();
};

/// 磁盘配置。
struct SysmonDiskConfig {
    std::vector<std::string> watch_paths;  ///< 关注路径（共享内存目录/数据库目录所在卷）
    std::vector<std::string> devices;      ///< 设备范围（空 = 全部物理盘）
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonDiskConfig, watch_paths, devices)
};

/// 网络配置。
struct SysmonNetworkConfig {
    bool hide_virtual = true;  ///< 默认隐藏虚拟网卡（Hyper-V / WSL / Docker）
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonNetworkConfig, hide_virtual)
};

/// 历史存留配置（历史写入由后续票实现；本票仅配置形状定稿）。
struct SysmonHistoryConfig {
    bool enabled = true;
    int32_t raw_retention_hours = 24;
    int32_t max_retention_days = 30;
    uint64_t max_bytes = 2'147'483'648;  ///< 总量上限（0 = 不限制）
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SysmonHistoryConfig, enabled, raw_retention_hours, max_retention_days, max_bytes)
};

/// 实时推送配置。
struct SysmonRealtimeConfig {
    int32_t push_interval_sec = 5;  ///< 推送间隔，[1, 3600]；与计量频率解耦
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonRealtimeConfig, push_interval_sec)
};

/// 告警配置（告警触发由后续票实现；本票仅配置形状定稿）。
struct SysmonAlertsConfig {
    double cpu_sustain_used_pct = 90.0;  ///< 单核持续饱和阈值（百分比）
    int32_t cpu_sustain_samples = 10;    ///< 持续采样次数阈值
    double memory_used_pct = 90.0;
    double swap_used_pct = 50.0;
    double volume_free_pct = 10.0;               ///< 关注卷剩余空间低于该百分比告警
    uint64_t volume_free_bytes = 1'073'741'824;  ///< 同时低于该绝对容量告警
    int32_t cooldown_sec = 600;                  ///< 同类告警冷却
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonAlertsConfig,
                                                cpu_sustain_used_pct,
                                                cpu_sustain_samples,
                                                memory_used_pct,
                                                swap_used_pct,
                                                volume_free_pct,
                                                volume_free_bytes,
                                                cooldown_sec)
};

/// RTN_SYSMON_CONFIG payload / SET_SYSMON_CONFIG 合并后的完整配置（契约 sysmon §配置）。
struct SysmonConfig {
    SysmonSamplingConfig sampling;
    SysmonDiskConfig disk;
    SysmonNetworkConfig network;
    SysmonHistoryConfig history;
    SysmonRealtimeConfig realtime;
    SysmonAlertsConfig alerts;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SysmonConfig, sampling, disk, network, history, realtime, alerts)

    /// 默认配置（缺配置/损坏时自愈用；仓库不提交模板文件）。
    [[nodiscard]] static SysmonConfig default_config();

    /// 把 `patch` 按 RFC 7386 合并到当前配置并校验。
    /// @param current 当前生效配置 JSON（全量）
    /// @param patch   增量 patch（必须为 object；任意位置 null 非法）
    /// @return 校验通过的合并结果；失败抛 std::runtime_error（英文错误消息，cfg 不变）。
    [[nodiscard]] static nlohmann::json apply_patch(const nlohmann::json& current,
                                                    const nlohmann::json& patch);

    /// 校验全量配置 JSON。@return 通过 nullopt；失败英文错误消息。
    [[nodiscard]] static std::optional<std::string> validate(const nlohmann::json& cfg);
};

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_SYSMON_H_
