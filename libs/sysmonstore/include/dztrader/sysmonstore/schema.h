#ifndef DZTRADER_SYSMONSTORE_SCHEMA_H_
#define DZTRADER_SYSMONSTORE_SCHEMA_H_

/**
 * @file schema.h
 * @brief dzsysmon 历史库的**唯一** schema 定义（表名、列、指标键、schema 版本）。
 *
 * 历史库由 `dzsysmon` 唯一写入、`dzweb` 只读（#41 spec「历史存储」）。写入方与读取方
 * 共用本模块，因此 schema 与行编解码只有这一份定义——禁止在任一进程内另写一份建表语句。
 *
 * 表结构分两类（契约 sysmon §2 的快照形状）：
 * - **宽表** `sysmon_scalar`：固定标量指标，一行一个时间桶，每指标 5 列
 *   （`_min` / `_max` / `_avg` / `_last` / `_n`）。`_n` 是该指标在本桶内的有效样本数，
 *   供压缩时对 `_avg` 做加权合并（否则「部分样本不可用」的桶会把均值算偏）。
 * - **长表** `sysmon_dimension`：按维度展开的指标（CPU 核 / 卷 / 物理盘 / 网卡 / 温度传感器），
 *   一行一个 (桶, 域, 维度, 指标)。维度元数是可变的（核数、盘数、网卡数都会随机器与
 *   热插拔变化），塞进固定列会把表结构钉死在某一台机器上。
 *
 * 时间桶：
 * - `level = 0` 原始记录（宽 = `sampling.record_interval_sec`，默认 60，带相位偏移）
 * - `level = 1` 一分钟聚合（压缩产物，`bucket_ts` 按墙钟整分钟对齐，便于与交易事件对照）
 *
 * 复合主键 `(level, bucket_ts)` 让压缩产物与原始记录同表共存且互不覆盖；读取方按
 * `level` 选粒度。
 */

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dztrader::sysmonstore {

/// 历史库 schema 版本。**只增不改**：结构变化必须换新版本号，读取方据此降级
/// （版本不符 ⇒ 历史不可用，不得按旧结构解码、不得崩溃）。
inline constexpr int kSchemaVersion = 1;

// ===== 表名 =====

inline constexpr std::string_view kTableMeta = "sysmon_meta";            ///< 键值元数据
inline constexpr std::string_view kTableScalar = "sysmon_scalar";        ///< 固定标量宽表
inline constexpr std::string_view kTableDimension = "sysmon_dimension";  ///< 按维度展开的长表
inline constexpr std::string_view kTableMark = "sysmon_mark";            ///< 采集中断/时钟跳变标记

/// 元数据表中记录 schema 版本的键。
inline constexpr std::string_view kMetaSchemaVersion = "schema_version";

// ===== 桶层级 =====

inline constexpr int32_t kLevelRaw = 0;     ///< 原始记录（记录间隔 1 秒 ~ 任意）
inline constexpr int32_t kLevelMinute = 1;  ///< 一分钟聚合（压缩产物）

/// 压缩后聚合桶的宽度（秒）。契约 sysmon「历史存储」固定为一分钟。
inline constexpr int64_t kMinuteResolutionSec = 60;

// ===== 固定标量指标（宽表）=====

/// 列存储类型：计数器与字节数为整型（SQLite INTEGER 为 64 位，`usage_ratio` 等为 REAL）。
enum class ScalarType { Real, Integer };

/// 宽表中的一个标量指标：`key` 是逻辑指标键（跨进程/跨 REST 的唯一引用名，
/// 与《帧契约：system》快照字段同形），`column` 是物理列前缀。
struct ScalarMetricSpec {
    std::string_view key;     ///< 逻辑键，如 `memory.used_bytes`
    std::string_view column;  ///< 列前缀，如 `memory_used_bytes`
    ScalarType type = ScalarType::Real;
};

/// 宽表指标全集。**顺序即建表列序**，新增指标追加在尾部（只增不改，配合 schema 版本号）。
inline constexpr std::array<ScalarMetricSpec, 18> kScalarMetrics = {{
    {"cpu.usage_ratio", "cpu_usage_ratio", ScalarType::Real},
    {"cpu.load_average.one", "cpu_load_average_one", ScalarType::Real},
    {"cpu.load_average.five", "cpu_load_average_five", ScalarType::Real},
    {"cpu.load_average.fifteen", "cpu_load_average_fifteen", ScalarType::Real},
    {"cpu.context_switches_per_sec", "cpu_context_switches_per_sec", ScalarType::Real},
    {"cpu.interrupts_per_sec", "cpu_interrupts_per_sec", ScalarType::Real},
    {"memory.total_bytes", "memory_total_bytes", ScalarType::Integer},
    {"memory.available_bytes", "memory_available_bytes", ScalarType::Integer},
    {"memory.used_bytes", "memory_used_bytes", ScalarType::Integer},
    {"memory.used_ratio", "memory_used_ratio", ScalarType::Real},
    {"memory.cached_bytes", "memory_cached_bytes", ScalarType::Integer},
    {"memory.buffers_bytes", "memory_buffers_bytes", ScalarType::Integer},
    {"swap.total_bytes", "swap_total_bytes", ScalarType::Integer},
    {"swap.used_bytes", "swap_used_bytes", ScalarType::Integer},
    {"swap.used_ratio", "swap_used_ratio", ScalarType::Real},
    {"shm_dir.total_bytes", "shm_dir_total_bytes", ScalarType::Integer},
    {"shm_dir.page_files", "shm_dir_page_files", ScalarType::Integer},
    {"shm_dir.oldest_file_age_sec", "shm_dir_oldest_file_age_sec", ScalarType::Integer},
}};

// ===== 按维度展开的指标（长表）=====

/// 一个维度域的指标名集合。
struct DimensionDomainSpec {
    std::string_view domain;                    ///< 域标识，如 `network`
    std::span<const std::string_view> metrics;  ///< 该域的指标名（行内 `metric` 列取值）
};

inline constexpr std::string_view kCpuCoreDomain = "cpu_core";
inline constexpr std::string_view kVolumeDomain = "volume";
inline constexpr std::string_view kDiskIoDomain = "disk_io";
inline constexpr std::string_view kNetworkDomain = "network";
inline constexpr std::string_view kTemperatureDomain = "temperature";

inline constexpr std::string_view kCpuCoreMetrics[] = {"usage_ratio", "mhz", "saturated"};
inline constexpr std::string_view kVolumeMetrics[] = {"total_bytes", "used_bytes", "used_ratio",
                                                      "watched"};
inline constexpr std::string_view kDiskIoMetrics[] = {
    "read_bytes_per_sec", "write_bytes_per_sec", "read_iops",  "write_iops",
    "latency_ms",         "queue_depth",         "utilization"};
inline constexpr std::string_view kNetworkMetrics[] = {
    "rx_bytes_total",   "tx_bytes_total",   "rx_packets_total",  "tx_packets_total",
    "rx_errors_total",  "tx_errors_total",  "rx_drops_total",    "tx_drops_total",
    "rx_bytes_per_sec", "tx_bytes_per_sec", "rx_errors_per_sec", "tx_errors_per_sec",
    "rx_drops_per_sec", "tx_drops_per_sec", "is_virtual"};
inline constexpr std::string_view kTemperatureMetrics[] = {"celsius"};

/// 维度域全集（顺序即校验顺序；新增域追加在尾部）。
inline constexpr std::array<DimensionDomainSpec, 5> kDimensionDomains = {{
    {kCpuCoreDomain, std::span<const std::string_view>(kCpuCoreMetrics)},
    {kVolumeDomain, std::span<const std::string_view>(kVolumeMetrics)},
    {kDiskIoDomain, std::span<const std::string_view>(kDiskIoMetrics)},
    {kNetworkDomain, std::span<const std::string_view>(kNetworkMetrics)},
    {kTemperatureDomain, std::span<const std::string_view>(kTemperatureMetrics)},
}};

// ===== 标记（采集中断 / 重启 / 时钟跳变）=====

/// 窗口样本不足或计量 tick 被跳过（缺口）；`detail` 为 `<实际样本数>/<跳过的 tick 数>`。
inline constexpr std::string_view kMarkSampleGap = "sample_gap";
/// 进程重启（含自动重启）：重启窗口内的数据缺失，绝不伪造连续数据。
/// `detail` 为 `<recover|manual>[/<缺口秒数>]`（缺口算不出时省略）。
inline constexpr std::string_view kMarkRestart = "restart";
/// 检测到墙钟跳变（NTP 步进/手工改时间）；`detail` 为带符号的跳变毫秒数。
inline constexpr std::string_view kMarkClockJump = "clock_jump";

inline constexpr std::array<std::string_view, 3> kMarkKinds = {kMarkSampleGap, kMarkRestart,
                                                               kMarkClockJump};

// ===== 查询辅助 =====

/// 按逻辑键查宽表指标定义；未知键返回 nullptr。
[[nodiscard]] const ScalarMetricSpec* find_scalar_metric(std::string_view key) noexcept;

/// 按列前缀查宽表指标定义；未知列返回 nullptr。
[[nodiscard]] const ScalarMetricSpec* find_scalar_metric_by_column(
    std::string_view column) noexcept;

/// 按域标识查维度域定义；未知域返回 nullptr。
[[nodiscard]] const DimensionDomainSpec* find_dimension_domain(std::string_view domain) noexcept;

/// `metric` 是否属于该域（写入前校验，避免脏指标名落库）。
[[nodiscard]] bool dimension_domain_has_metric(std::string_view domain,
                                               std::string_view metric) noexcept;

/// 标记 kind 是否合法。
[[nodiscard]] bool is_known_mark_kind(std::string_view kind) noexcept;

/**
 * @brief 建表 DDL（幂等，全部 `IF NOT EXISTS`）。
 *
 * 由上面的指标注册表机械生成——新增指标只需改注册表，DDL、insert/select 列清单、
 * 行编解码三处不会漂移。
 */
[[nodiscard]] std::string build_schema_ddl();

}  // namespace dztrader::sysmonstore

#endif  // DZTRADER_SYSMONSTORE_SCHEMA_H_
