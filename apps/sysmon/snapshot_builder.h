#ifndef DZTRADER_SYSMON_SNAPSHOT_BUILDER_H_
#define DZTRADER_SYSMON_SNAPSHOT_BUILDER_H_

/**
 * @file snapshot_builder.h
 * @brief RTN_SYSMON_SNAPSHOT payload 构造（契约 sysmon §2）。
 *
 * 本票（#76、#77）填系统信息、CPU 总量/每核/饱和标记/负载/上下文切换与中断速率、每核频率、
 * 温度、内存与交换、卷容量、共享内存目录占用、每物理盘 I/O、每网卡累计量与速率
 * （`hide_virtual` 过滤在快照构造阶段完成）。已列出的指标域后续只填值、不改形状。
 *
 * 纯函数：不读时钟、不读配置全局状态，输入即快照的全部事实来源，便于用假数据测试。
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/platform/sysmon.h>
#include <dztrader/platform/system_collector.h>

namespace dztrader::sysmon {

/// CPU 区块的测量结果与能力标记（快照构造的唯一 CPU 事实来源）。
///
/// `*_collected` 表示采集层能力是否可用（不是本帧数值是否已就绪）：首次采样时
/// `total_usage_ratio` / 每核 `usage_ratio` / 速率为 null，但对应可用性仍为 available。
struct CpuMetrics {
    /// 配置关闭 `sampling.groups.cpu`（优先级最高：CPU 数值清空 + 四个可用性键
    /// disabled_by_config）。 不含 `cpu.frequency`——它由独立组 `cpu_frequency` 控制（契约 sysmon
    /// §3.1.1）。
    bool disabled = false;
    std::optional<double> total_usage_ratio;     ///< CPU 总使用率；nullopt = 首次采样/不可用
    bool cores_collected = false;                ///< 每核序列能力是否可用（粘性）
    std::vector<platform::SysmonCpuCore> cores;  ///< 每核使用率与饱和标记
    std::vector<uint32_t> saturated_cores;       ///< 当前持续饱和的核 index 集合
    bool load_collected = false;  ///< 系统负载能力是否可用（粘性；与数值是否就绪无关）
    std::optional<platform::SysmonLoadAverage> load_average;  ///< 系统负载（口径见 semantic）
    bool rates_collected = false;  ///< 上下文切换/中断累计计数器能力是否可用（粘性）
    std::optional<double> context_switches_per_sec;
    std::optional<double> interrupts_per_sec;
    bool frequency_collected = false;  ///< 每核频率能力是否可用（粘性，契约 sysmon §2.7）
    bool frequency_disabled = false;   ///< 配置关闭 cpu_frequency 组（优先级最高）
    /// 本帧每核频率读数；nullopt = 本帧未读到（能力可用但未就绪，或不可用）。
    std::optional<std::vector<platform::SysmonCoreFrequency>> core_frequencies;
};

/// 温度传感器的测量结果与能力标记（#73；温度为瞬时 gauge，不做差值/聚合）。
struct TemperatureMetrics {
    bool collected = false;  ///< 温度能力是否可用（粘性）
    bool disabled = false;   ///< 配置关闭 temperature 组（优先级最高）
    /// 本帧温度读数；nullopt = 本帧未读到（能力可用但未就绪，或不可用）。
    std::optional<std::vector<platform::SysmonTemperatureInfo>> sensors;
};

/// 内存/交换的测量结果与能力标记（#74；两者均为瞬时 gauge，每次计量刷新）。
///
/// `collected` / `swap_collected` 表示采集能力（粘性，不是本帧数值是否就绪）；
/// `swap_supported` 表示本机确有交换分区/页面文件（`total_bytes > 0`）：本帧读取成功时
/// 按本帧读数更新（运行时移除交换后可用性随之变为 unsupported，契约 sysmon §2.5），
/// 本帧读取失败时保持上次值。
struct MemorySwapMetrics {
    bool disabled = false;   ///< 配置关闭 memory 组（优先级最高）
    bool collected = false;  ///< 内存采集能力是否可用（粘性）
    /// 本帧原始内存读数；nullopt = 本帧未读到（能力可用但本帧失败，或不可用）。
    std::optional<platform::RawMemoryInfo> memory;
    bool swap_disabled = false;   ///< 配置关闭 swap 组（优先级最高）
    bool swap_collected = false;  ///< 交换采集能力是否可用（粘性）
    bool swap_supported = false;  ///< 本机有交换分区/页面文件（粘性）
    /// 本帧原始交换读数；nullopt = 本帧未读到（能力可用但本帧失败，或不可用）。
    std::optional<platform::RawSwapInfo> swap;
};

/// 磁盘卷与共享内存目录的测量结果与能力标记（#75）。
///
/// `volumes_collected` / `shm_dir_collected` 为**能力粘性**标记（契约 sysmon §2.5）：
/// 一次成功读到后保持 true，此后单帧失败只把对应域置 null。
/// 单个卷不可读**不改写** `volumes_collected`——该卷以 `available=false` 进数组（§2.4），
/// 只有一次一个卷都枚举不到时才是整体采集失败。
struct DiskMetrics {
    /// 配置关闭 sampling.groups.volumes（优先级最高：域为 null + disabled_by_config）。
    bool volumes_disabled = false;
    bool volumes_collected = false;  ///< 卷采集能力是否可用（粘性）
    /// 本帧原始卷读数；nullopt = 本帧未采集到（能力可用但失败，或不可用）。
    std::optional<std::vector<platform::RawVolumeInfo>> volumes;

    bool shm_dir_collected = false;  ///< 共享内存目录采集能力是否可用（粘性）
    /// 本帧共享内存目录占用；nullopt = 本帧未读到（能力可用但失败，或不可用）。
    std::optional<platform::RawShmDirInfo> shm_dir;

    /// 配置关闭 sampling.groups.disk_io（优先级最高：域为 null + disabled_by_config）。
    bool disk_io_disabled = false;
    bool disk_io_collected = false;  ///< 每物理盘 I/O 采集能力是否可用（粘性）
    /// 本帧每物理盘 I/O 派生值（`compute_disk_io` 的结果）；nullopt = 本帧未采到
    /// （能力可用但失败，或不可用；空数组 = 可用且本机无物理盘）。
    std::optional<std::vector<platform::SysmonDiskIoInfo>> disk_io;
    /// 本帧采集失败原因（采集层给出；能力从未确认时用于可用性映射，不按平台名猜）。
    std::optional<std::string> disk_io_unavailable_reason;

    /// 配置 `disk.devices` 设备范围：空数组 = 全部整物理盘；非空时只输出 `device`
    /// 与列表**精确匹配**的 `disk_io[]` 条目（契约 sysmon §3.1）。过滤发生在**快照构造**
    /// 阶段（与 `network.hide_virtual` 同款）：不改采集、不重置速率基线；
    /// 过滤后为空数组仍 `available=true`（"范围内没有盘"与"采集失败"是不同事实）。
    std::vector<std::string> devices;
};

/**
 * 每网卡的测量结果与能力标记（#77）。
 *
 * `network_collected` 为**能力粘性**标记（契约 sysmon §2.5）：一次成功枚举到网卡后保持
 * true，此后单帧读取失败只把域置 null。`hide_virtual` 过滤发生在**快照构造**阶段
 * （不在采集/纯函数里），因此改开关不需要重新采集、也不影响速率基线（§2.7）。
 * 全部网卡都被 `hide_virtual` 滤掉时输出**空数组**且 `available=true`——"全部被过滤"
 * 与"采集失败"是不同事实（§2.5 明文）。
 */
struct NetworkMetrics {
    /// 配置关闭 sampling.groups.network（优先级最高：域为 null + disabled_by_config）。
    bool network_disabled = false;
    bool network_collected = false;  ///< 网卡枚举能力是否可用（粘性）
    /// 本帧每网卡累计量与速率（`compute_network_rates` 的结果，含虚拟网卡）；nullopt =
    /// 本帧未采到（能力可用但失败，或不可用；空数组 = 可用且本机无网络接口）。
    std::optional<std::vector<platform::SysmonNetworkInfo>> network;
    /// 本帧采集失败原因（采集层给出；能力从未确认时用于可用性映射，不按平台名猜）。
    std::optional<std::string> network_unavailable_reason;
    /// 配置 `network.hide_virtual`：true 时过滤掉 `is_virtual = true` 的条目（默认 true）。
    bool hide_virtual = true;
};

/**
 * @brief 构造一帧实时快照。
 *
 * @param source               发送方进程名（无 instance_id 帧的来源显式携带，general §5）
 * @param system_info          采集到的原始系统信息（进程启动时采集一次）
 * @param cpu_metrics          CPU 区块的测量结果与能力标记
 * @param temperature_metrics  温度传感器的测量结果与能力标记
 * @param memory_metrics       内存/交换的测量结果与能力标记
 * @param disk_metrics         卷/共享内存目录/每物理盘 I/O 的测量结果与能力标记
 *                             （#75、#76）
 * @param network_metrics      每网卡累计量与速率、能力标记与 hide_virtual（#77）
 * @param measure_interval_ms  当前计量间隔（毫秒）
 * @param last_sample_unix_ms  最近一次计量成功的墙钟时间（Unix 毫秒，0 = 尚无成功样本）
 * @param missed_ticks         累计错过的计量 tick 数
 * @param now_unix_ms          快照生成时刻（Unix 毫秒）
 * @return 可直接序列化的快照
 */
[[nodiscard]] platform::SysmonSnapshot build_snapshot(std::string_view source,
                                                      const platform::RawSystemInfo& system_info,
                                                      const CpuMetrics& cpu_metrics,
                                                      const TemperatureMetrics& temperature_metrics,
                                                      const MemorySwapMetrics& memory_metrics,
                                                      const DiskMetrics& disk_metrics,
                                                      const NetworkMetrics& network_metrics,
                                                      int32_t measure_interval_ms,
                                                      int64_t last_sample_unix_ms,
                                                      uint64_t missed_ticks,
                                                      int64_t now_unix_ms);

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_SNAPSHOT_BUILDER_H_
