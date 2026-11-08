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
 * - `DZ_FRAME_QUERY_SYSMON_CLOCK`：手动触发一次时钟探测（定向，`instance_id=dzsysmon`，
 *   形态 3 单向下行动作帧、空 payload、无 RTN；结果经快照 `clock` 域广播，契约 sysmon §1.1）
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
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace dztrader::platform {

/// 系统监控进程的固定实例名（契约 sysmon §5：快照/配置帧的固定发送方与镜像挂载实例）。
///
/// 单一真相源：发送方（`dzsysmon` 的可执行文件名即 instance_id）、dzweb 的镜像挂载、
/// SET 帧的路由键、校验里 source 字段的期望值都引用此处，避免同一个字面量散落多处
/// （`core_data_type.h` 的帧名是另一维度：帧类型不是实例标识）。
inline constexpr const char* SYSMON_INSTANCE_NAME = "dzsysmon";

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
inline constexpr const char* SYSMON_METRIC_CLOCK_OFFSET = "clock.offset";
inline constexpr const char* SYSMON_METRIC_CLOCK_SYNC_SERVICE = "clock.sync_service";

/// 可用性映射的固定键集合全集（契约 sysmon §2.5；每帧快照必须全覆盖）。
///
/// `clock.offset` / `clock.sync_service`（#295 登记，契约 sysmon §2.9）是**两个独立键**：
/// 前者表达「最近一轮探测是否测到」（**非**能力粘性，测不到就回到不可用），后者表达
/// 「同步服务枚举能力」；两键互不影响。
inline constexpr std::array<const char*, 15> SYSMON_METRIC_KEYS = {
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
    SYSMON_METRIC_CLOCK_OFFSET,
    SYSMON_METRIC_CLOCK_SYNC_SERVICE,
};

// ===== 时钟探测的失败原因（契约 sysmon §2.9 `servers[].error`；#297）=====
//
// 唯一真相源：发送方（探测核心）产出这些取值，接收方（`SysmonSnapshot::validate`）校验
// 合法集合。两处各写一份字面量会在「改一个取值」时静默漂移，所以在此集中定义。
inline constexpr const char* SYSMON_CLOCK_ERROR_TIMEOUT = "timeout";
inline constexpr const char* SYSMON_CLOCK_ERROR_KOD_RATE = "kod_rate";
inline constexpr const char* SYSMON_CLOCK_ERROR_KOD_DENY = "kod_deny";
inline constexpr const char* SYSMON_CLOCK_ERROR_BAD_ECHO = "bad_echo";
inline constexpr const char* SYSMON_CLOCK_ERROR_UNREACHABLE = "unreachable";

/// `servers[].error` 的合法取值全集（契约 sysmon §2.9；校验与发送方共用）。
inline constexpr std::array<const char*, 5> SYSMON_CLOCK_ERROR_CODES = {
    SYSMON_CLOCK_ERROR_TIMEOUT,  SYSMON_CLOCK_ERROR_KOD_RATE,    SYSMON_CLOCK_ERROR_KOD_DENY,
    SYSMON_CLOCK_ERROR_BAD_ECHO, SYSMON_CLOCK_ERROR_UNREACHABLE,
};

/// 配置 payload / 配置文件的业务段全集（契约 sysmon §3.1；顺序固定）。
inline constexpr std::array<const char*, 7> SYSMON_CONFIG_SECTIONS = {
    "sampling", "disk", "network", "history", "realtime", "thresholds", "clock"};

/// 阈值越限配置段名（契约 sysmon §3.1；本段原名 `alerts`，#123 随 ADR 0034 改名）。
inline constexpr const char* SYSMON_THRESHOLDS_SECTION = "thresholds";

/// 旧版配置段名（契约 sysmon §3.1「旧配置迁移」）：读到本段时把可映射的键搬到
/// `thresholds`、写回文件并记一条日志，**不报错、不阻塞启动**（ADR 0034）。
inline constexpr const char* SYSMON_LEGACY_ALERTS_SECTION = "alerts";

/// 配置 `sampling.groups` 的指标键（契约 sysmon §3.1.1）：A 组 CPU/内存/交换 + B 组温度与每核频率。
///
/// `cpu` 覆盖 `cpu.total` / `cpu.cores` / `cpu.load_average` / `cpu.context_switches`
/// 与饱和核判定， 但**不含** `cpu.frequency`（由独立键 `cpu_frequency` 控制）。
inline constexpr const char* SYSMON_GROUP_CPU = "cpu";
inline constexpr const char* SYSMON_GROUP_MEMORY = "memory";
inline constexpr const char* SYSMON_GROUP_SWAP = "swap";
inline constexpr const char* SYSMON_GROUP_TEMPERATURE = "temperature";
inline constexpr const char* SYSMON_GROUP_CPU_FREQUENCY = "cpu_frequency";

/// 时钟偏差域在 `sampling.groups` 里的开关（契约 sysmon §3.1.1；#296）。
///
/// 与 `clock.enabled` 是**同一意图的两层入口**：两者同时为 true 才启用（关闭取或）。
/// 单独放常量是因为构造层（快照 `clock` 域是否输出 `null`）与时长/阈值判定都要读它，
/// 而字面量散落会在两处各写一套后漂移。
inline constexpr const char* SYSMON_GROUP_CLOCK = "clock";

// ===== 阈值越限判定对象 kind（契约 sysmon §2.8；#123 / 时间判定对象 #301）=====
// 日志记录里的稳定取值，**不进快照、不构成协议语义**（ADR 0034）。`cpu_saturated` 只出现
// 在日志与 UI 色标里：CPU 饱和从来不发通知，改日志后它也成了一等判定对象。
inline constexpr const char* SYSMON_THRESHOLD_MEMORY_USED = "memory_used";
inline constexpr const char* SYSMON_THRESHOLD_SWAP_USED = "swap_used";
inline constexpr const char* SYSMON_THRESHOLD_VOLUME_FREE = "volume_free";
inline constexpr const char* SYSMON_THRESHOLD_CPU_SATURATED = "cpu_saturated";

/// 时间判定对象（契约 sysmon §2.8「时间判定对象」；#301，ADR 0047）。
///
/// 与上面四个资源类有两点差异：
/// 1. `clock_offset` 的 **error 档**产生 `NOTIFY_UI` 弹窗（warn 档与 `clock_bound` 都只写
///    日志）——ADR 0047 对 ADR 0034 静默原则的部分修订；
/// 2. 判定时机在**每次探测完成时**，不是计量 tick。
inline constexpr const char* SYSMON_THRESHOLD_CLOCK_OFFSET = "clock_offset";
inline constexpr const char* SYSMON_THRESHOLD_CLOCK_BOUND = "clock_bound";

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

/// 内存。唯一口径：`used_bytes = total_bytes - available_bytes`（契约 sysmon §2.4）。
struct SysmonMemoryInfo {
    uint64_t total_bytes = 0;
    uint64_t available_bytes = 0;
    uint64_t used_bytes = 0;
    std::optional<double> used_ratio;  ///< used_bytes / total_bytes；total=0 时 null
    /// 可回收文件页缓存（Linux `Cached + SReclaimable`；Windows `SystemCache`）；
    /// null = 本平台不提供/本帧未读到。信息项，不参与 used 口径。
    std::optional<uint64_t> cached_bytes;
    /// 内核文件系统元数据缓存（仅 Linux `Buffers`）；Windows 恒为 null。
    std::optional<uint64_t> buffers_bytes;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonMemoryInfo,
                                                total_bytes,
                                                available_bytes,
                                                used_bytes,
                                                used_ratio,
                                                cached_bytes,
                                                buffers_bytes)
};

/// 交换分区/页文件（契约 sysmon §2.4）。total_bytes=0 表示本机无交换/无页面文件
/// （availability 标记 unsupported）；Windows 口径为页面文件支撑的提交量。
struct SysmonSwapInfo {
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    std::optional<double> used_ratio;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonSwapInfo, total_bytes, used_bytes, used_ratio)
};

/// 卷容量与可用性（契约 sysmon §2.4）。
///
/// `available=false` 表示卷存在但容量读不到（无权限/不可访问）：此时
/// `total_bytes`/`used_bytes`/`used_ratio` 必须为 null，不得用 0 冒充；
/// 该卷仍进数组（"存在但读不到"与"没有这个卷"是不同事实，消费方都要能显示）。
struct SysmonVolumeInfo {
    std::string mount;      ///< 规范化挂载点（Linux 挂载目录；Windows 盘符根，如 `C:\\`）
    bool available = true;  ///< 容量是否可读
    std::optional<uint64_t> total_bytes;  ///< available=false 时为 null
    std::optional<uint64_t> used_bytes;   ///< available=false 时为 null
    std::optional<double> used_ratio;     ///< available=false / total=0 时为 null
    std::optional<std::string> reason;    ///< available=false 时的原因（英文、小写开头）
};

// 手写序列化：`reason` 在 available=true 时省略（契约 sysmon §7），
// 且 available=false 时三个容量字段必须输出 null（不得省略、不得用 0）。
inline void to_json(nlohmann::json& j, const SysmonVolumeInfo& v) {
    j = nlohmann::json{{"mount", v.mount},
                       {"available", v.available},
                       {"total_bytes", v.total_bytes.has_value() ? nlohmann::json(*v.total_bytes)
                                                                 : nlohmann::json(nullptr)},
                       {"used_bytes", v.used_bytes.has_value() ? nlohmann::json(*v.used_bytes)
                                                               : nlohmann::json(nullptr)},
                       {"used_ratio", v.used_ratio.has_value() ? nlohmann::json(*v.used_ratio)
                                                               : nlohmann::json(nullptr)}};
    if (v.reason.has_value() && !v.reason->empty()) {
        j["reason"] = *v.reason;
    }
}

inline void from_json(const nlohmann::json& j, SysmonVolumeInfo& v) {
    v.mount = j.at("mount").get<std::string>();
    v.available = j.value("available", true);
    v.total_bytes = j.value("total_bytes", nlohmann::json(nullptr)).is_null()
                        ? std::nullopt
                        : std::optional<uint64_t>(j["total_bytes"].get<uint64_t>());
    v.used_bytes = j.value("used_bytes", nlohmann::json(nullptr)).is_null()
                       ? std::nullopt
                       : std::optional<uint64_t>(j["used_bytes"].get<uint64_t>());
    v.used_ratio = (!j.contains("used_ratio") || j["used_ratio"].is_null())
                       ? std::nullopt
                       : std::optional<double>(j["used_ratio"].get<double>());
    v.reason = (!j.contains("reason") || j["reason"].is_null())
                   ? std::nullopt
                   : std::optional<std::string>(j["reason"].get<std::string>());
}

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

/// 每网卡累计量与速率（契约 sysmon §2.4 `network[]`）。
///
/// 八个 `*_total` 是原始累计计数器（收/发 × 字节/包/错误/丢弃）；六个 `*_per_sec`
/// 由纯函数从两次采样差值计算，首次采样 / 新出现的网卡 / 计数器重置时为 null
/// （不得用 0 冒充）。`is_virtual` 只用结构性依据判定（Linux 网卡是否有 `device`
/// 支撑；Windows `HardwareInterface`），不看名字前缀。
struct SysmonNetworkInfo {
    /// 网卡名。C++ 字段不叫 `interface`：Windows SDK 的 COM 头（objbase.h）把它
    /// 定义成 `struct` 宏，包含顺序变化即编译失败；JSON 键仍是契约规定的 `interface`。
    std::string interface_name;
    /// 累计收发（字节 / 包 / 错误 / 丢弃）。
    uint64_t rx_bytes_total = 0;
    uint64_t tx_bytes_total = 0;
    uint64_t rx_packets_total = 0;
    uint64_t tx_packets_total = 0;
    uint64_t rx_errors_total = 0;
    uint64_t tx_errors_total = 0;
    uint64_t rx_drops_total = 0;
    uint64_t tx_drops_total = 0;
    /// 每秒速率；null = 首次采样 / 计数器重置（不得用 0 冒充）。
    std::optional<double> rx_bytes_per_sec;
    std::optional<double> tx_bytes_per_sec;
    std::optional<double> rx_errors_per_sec;
    std::optional<double> tx_errors_per_sec;
    std::optional<double> rx_drops_per_sec;
    std::optional<double> tx_drops_per_sec;
    /// 是否虚拟网卡（不由硬件设备支撑）；`hide_virtual = true` 时发送方不输出 true 的条目。
    bool is_virtual = false;
};

// 手写序列化：JSON 键 `interface` ↔ C++ 字段 `interface_name`（Windows `interface` 宏冲突）。
inline void to_json(nlohmann::json& j, const SysmonNetworkInfo& n) {
    j = nlohmann::json{{"interface", n.interface_name},
                       {"rx_bytes_total", n.rx_bytes_total},
                       {"tx_bytes_total", n.tx_bytes_total},
                       {"rx_packets_total", n.rx_packets_total},
                       {"tx_packets_total", n.tx_packets_total},
                       {"rx_errors_total", n.rx_errors_total},
                       {"tx_errors_total", n.tx_errors_total},
                       {"rx_drops_total", n.rx_drops_total},
                       {"tx_drops_total", n.tx_drops_total},
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
    // 八个累计计数器必须存在（契约 sysmon §7：累计值没有"不可用"语义）——用 at() 而非
    // value() 兜底默认值，使解码路径与 validate 的必填规则一致：缺字段即抛，不静默补 0。
    n.rx_bytes_total = j.at("rx_bytes_total").get<uint64_t>();
    n.tx_bytes_total = j.at("tx_bytes_total").get<uint64_t>();
    n.rx_packets_total = j.at("rx_packets_total").get<uint64_t>();
    n.tx_packets_total = j.at("tx_packets_total").get<uint64_t>();
    n.rx_errors_total = j.at("rx_errors_total").get<uint64_t>();
    n.tx_errors_total = j.at("tx_errors_total").get<uint64_t>();
    n.rx_drops_total = j.at("rx_drops_total").get<uint64_t>();
    n.tx_drops_total = j.at("tx_drops_total").get<uint64_t>();
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
    // is_virtual 同样必填（契约 sysmon §7）：缺 key 即抛，不默认成 false。
    n.is_virtual = j.at("is_virtual").get<bool>();
}

/// 共享内存目录占用（契约 sysmon §2.4）：用于确认页清理机制没有失效。
///
/// `oldest_file_age_sec` 为最旧**页文件**的文件年龄（秒，0 = 刚写入）；无页文件时为 null。
/// 目录不可读时整个对象为 null（可用性 collect_failed），不得输出全 0 的对象。
struct SysmonShmDirInfo {
    std::string path;          ///< 共享内存根目录绝对路径（$DZTRADER_HOME/shm）
    uint64_t total_bytes = 0;  ///< 目录下全部普通文件字节数之和（递归）
    uint64_t page_files = 0;   ///< 页文件数量（递归，`<数字>.dat`；不含 meta.dat）
    std::optional<int64_t> oldest_file_age_sec;  ///< 最旧页文件年龄（秒）；无页文件为 null
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SysmonShmDirInfo, path, total_bytes, page_files, oldest_file_age_sec)
};

/// 单个温度传感器读数（契约 sysmon §2.4；温度为瞬时 gauge，不是累计计数器）。
///
/// `chip` / `label` 是 #126 的 additive 扩展：**本版本发送方每次都要给出两者**（`chip` 非空），
/// 但消费方必须容忍它们缺失（旧版本发送方不带）——缺失时由展示层从 `sensor` 回退解析。
/// 因此两个字段在 C++ 侧是普通 `std::string`（序列化恒有值），兼容性落在消费方的校验与展示层。
struct SysmonTemperatureInfo {
    /// 传感器稳定标识（Linux hwmon：`<name>#<hwmonN>:<label>`）。**值不因新增 chip/label 而改变**：
    /// 它是历史库 `temperature` 域的维度键（契约 §2.4）。
    std::string sensor;
    /// 芯片名（hwmon 的 `name`，如 `coretemp` / `amdgpu` /
    /// `acpitz`）；发送方必须给出且非空（#126）。
    std::string chip;
    /// 通道标签（`tempN_label`）；#126。空串 = 该通道没有标签（合法事实，不是不可用）。
    std::string label;
    double celsius = 0.0;  ///< 摄氏温度
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonTemperatureInfo, sensor, chip, label, celsius)
};

/// 单台 NTP 服务器的探测明细（契约 sysmon §2.9 `servers[]`）。
///
/// `ok=true` 时四项测量非 `null` 且 `error = null`；`ok=false` 时四项测量必须为 `null`、
/// `error` 取 `timeout` / `kod_rate` / `kod_deny` / `bad_echo` / `unreachable`
/// ——「失败样本不产出任何测量值」（§7）。
struct SysmonClockServerInfo {
    std::string host;                  ///< 配置 `clock.servers` 中的一项（帧内唯一）
    bool ok = false;                   ///< 该台是否返回可用样本
    std::optional<double> offset_ms;   ///< 该台的有符号偏差（毫秒，正 = 本地快）
    std::optional<double> bound_ms;    ///< 该台的误差上界（毫秒）
    std::optional<double> rtt_ms;      ///< 该台的往返时延（毫秒，>= 0）
    std::optional<uint32_t> stratum;   ///< 该台参考源 stratum（[1,15]）
    std::optional<std::string> error;  ///< ok=false 时的失败原因；ok=true 时为 null
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SysmonClockServerInfo, host, ok, offset_ms, bound_ms, rtt_ms, stratum, error)
};

/// 检测到的时间同步服务（契约 sysmon §2.9 `daemons[]`）：名称 + 是否运行。
struct SysmonClockDaemonInfo {
    std::string name;      ///< 服务名（chronyd / ntpd / systemd-timesyncd / W32Time）
    bool running = false;  ///< 是否正在运行（Linux 只列出运行中的；Windows 可为 false）
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonClockDaemonInfo, name, running)
};

/// 时钟偏差域（契约 sysmon §2.9）：最近一轮探测的聚合值 + 每台明细 + 同步服务检测。
///
/// **只保留最近一轮**，不进任何历史（ADR 0047 决策 4）。整个域为 `null` **仅**表示该域
/// 被配置关闭（§2.9）；「尚未探测」是**域内空态**：`last_probe_unix_ms = 0`、
/// `offset_ms`/`bound_ms`/`source_stratum` 为 `null`、`source` 空串、`servers` 空数组。
/// 「全部失败」时 `last_probe_unix_ms > 0` 而三个测量字段仍为 `null`（不得用 0 或上一轮
/// 结果冒充）。
///
/// `offset_ms` / `bound_ms` / `source_stratum` **同生共死**——有偏差就必有界，「无界偏差」
/// 在交易场景里是误导；`source` 为空串当且仅当三者全为 `null`（§7）。
struct SysmonClockInfo {
    std::optional<double> offset_ms;             ///< 聚合有符号偏差（毫秒，正 = 本地快）
    std::optional<double> bound_ms;              ///< 聚合误差上界（毫秒）；与 offset_ms 同生共死
    std::string source;                          ///< 聚合值采用的服务器标识；未成功时为空串
    std::optional<uint32_t> source_stratum;      ///< 参考源 stratum（[1,15]）；未成功时 null
    int64_t last_probe_unix_ms = 0;              ///< 最近一轮探测完成时刻；0 = 尚未探测
    std::vector<SysmonClockServerInfo> servers;  ///< 最近一轮每台明细（空 = 尚未探测）
    /// 检测到的同步服务；空数组 = 未检测到运行中的服务（或检测失败），不是能力不可用。
    std::vector<SysmonClockDaemonInfo> daemons;
    bool daemon_conflict = false;  ///< 检测到 >= 2 种不同同步服务同时运行
    /// 内核/服务层「是否已同步」的辅助判据；`null` = 读不到（不猜成 false）。
    std::optional<bool> kernel_synced;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonClockInfo,
                                                offset_ms,
                                                bound_ms,
                                                source,
                                                source_stratum,
                                                last_probe_unix_ms,
                                                servers,
                                                daemons,
                                                daemon_conflict,
                                                kernel_synced)
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
    /// 时钟偏差域（契约 sysmon §2.9）；`null` = 该域被配置关闭。
    std::optional<SysmonClockInfo> clock;
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
                                                clock,
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

/// 磁盘配置（本段只含设备范围）。
struct SysmonDiskConfig {
    std::vector<std::string> devices;  ///< 设备范围（空 = 全部物理盘）
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonDiskConfig, devices)
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

/// 阈值越限配置（本段原名 `alerts`，#123 随 ADR 0034 改名，契约 sysmon §3.1）。
///
/// 全部阈值住同一段：它们**都不打扰使用者**（只写日志 + UI 色标），因此不需要再分
/// "哪些会打扰我"。没有 `volume_free_bytes`（绝对容量阈值已删除），也没有 `cooldown_sec`
/// （不再有通知可限流，防刷屏由「进入/恢复各一条 + 按 `log_repeat_sec` 重记」保证）。
struct SysmonThresholdsConfig {
    double cpu_sustain_used_pct = 90.0;  ///< 单核持续饱和阈值（百分比，[0,100]）
    int32_t cpu_sustain_samples = 10;    ///< 持续采样次数阈值，[1,3600]
    double memory_used_pct = 90.0;       ///< 内存使用率越限阈值，[0,100]
    double swap_used_pct = 50.0;         ///< 交换使用率越限阈值，[0,100]
    double volume_free_pct = 10.0;       ///< 卷**空闲**百分比阈值，[0,100]；低于即越限
    int32_t log_repeat_sec = 600;        ///< 持续越限的日志重记间隔，[0,86400]；0 = 不重记
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonThresholdsConfig,
                                                cpu_sustain_used_pct,
                                                cpu_sustain_samples,
                                                memory_used_pct,
                                                swap_used_pct,
                                                volume_free_pct,
                                                log_repeat_sec)
};

/// `clock.probe_times` 单个元素的解析结果（进程本地时区的一天内时刻）。
struct SysmonProbeTimeOfDay {
    int hour = 0;    ///< [0, 23]
    int minute = 0;  ///< [0, 59]
};

/**
 * @brief 解析 `HH:MM`（契约 sysmon §3.1：两位小时 + 两位分钟，`00:00`–`23:59`）。
 *
 * 配置校验与探测定时排期共用这一份解析：校验拒绝的写法排期也不会接受（反之亦然），
 * 避免「配置接受、排期静默忽略」这类两处语法漂移。只接受契约写死的形态——`8:30`
 * 这类单数字小时不收（同一时刻出现两种写法会让配置差异比较失真）。
 *
 * @param text 待解析文本
 * @return 合法时返回时分；非法返回 `std::nullopt`（不抛异常）
 */
[[nodiscard]] std::optional<SysmonProbeTimeOfDay> parse_sysmon_probe_time(
    std::string_view text) noexcept;

/// 时钟偏差监控域的配置（契约 sysmon §3.1 `clock` 段；#296）。
///
/// **域开关 = `enabled` 与 `sampling.groups.clock` 同时为 true**（关闭取或）：两层入口
/// 各在自己的段里如实回推持久化值，不做镜像同步。关闭时停止定时探测、跳过同步服务检测、
/// 快照 `clock` 域为 `null`、两键 `disabled_by_config`（§3.1.1）。
///
/// 三个时间阈值住本段而不是 `thresholds` 段（§3.1）：它们与探测参数同属一个功能单元，
/// 且其中 error 档会弹窗，与资源类「都不打扰」的性质不同（ADR 0047）。
struct SysmonClockConfig {
    bool enabled = true;  ///< 时钟监控域总开关（与 sampling.groups.clock 关闭取或）
    /// 探测服务器列表（NTPv4 客户端模式，UDP/123）；元素为非空字符串，**数组不得为空**
    /// （关闭监控用 enabled=false，清空服务器是校验失败——否则「尚未探测」与「无服务器
    /// 可探」在快照里不可区分）。
    std::vector<std::string> servers{"ntp.aliyun.com", "ntp.tencent.com", "cn.pool.ntp.org"};
    /// 每天定时探测的时刻（进程本地时区，`HH:MM` 两位小时+两位分钟，`00:00`–`23:59`）；
    /// 空数组 = 关闭定时探测（启动后的首次探测与手动查询仍执行）。
    std::vector<std::string> probe_times{"08:30", "12:30", "15:30", "18:00", "20:30"};
    int32_t probe_timeout_ms = 1500;       ///< 单台服务器的探测超时，[100, 10000]
    int32_t manual_min_interval_sec = 10;  ///< 手动查询最小间隔，[0, 3600]；0 = 不限流
    int32_t offset_warn_ms = 100;          ///< 偏差 warning 档阈值，[1, 60000] 且 < error
    int32_t offset_error_ms = 1000;        ///< 偏差 error 档阈值，(warn, 600000]
    int32_t bound_alarm_ms = 200;          ///< 误差界质量判据，[1, 60000]；独立于偏差判定
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SysmonClockConfig,
                                                enabled,
                                                servers,
                                                probe_times,
                                                probe_timeout_ms,
                                                manual_min_interval_sec,
                                                offset_warn_ms,
                                                offset_error_ms,
                                                bound_alarm_ms)
};

/// RTN_SYSMON_CONFIG payload / SET_SYSMON_CONFIG 合并后的完整配置（契约 sysmon §配置）。
struct SysmonConfig {
    SysmonSamplingConfig sampling;
    SysmonDiskConfig disk;
    SysmonNetworkConfig network;
    SysmonHistoryConfig history;
    SysmonRealtimeConfig realtime;
    SysmonThresholdsConfig thresholds;
    SysmonClockConfig clock;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SysmonConfig, sampling, disk, network, history, realtime, thresholds, clock)

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
