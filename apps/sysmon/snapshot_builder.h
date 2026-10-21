#ifndef DZTRADER_SYSMON_SNAPSHOT_BUILDER_H_
#define DZTRADER_SYSMON_SNAPSHOT_BUILDER_H_

/**
 * @file snapshot_builder.h
 * @brief RTN_SYSMON_SNAPSHOT payload 构造（契约 sysmon §2）。
 *
 * 本票（#72）填系统信息、CPU 总量/每核/饱和标记/负载/上下文切换与中断速率；
 * 内存/磁盘/网络等其余指标域仍输出 `null`/空数组并在 `availability.metrics` 标记
 * `available=false + reason="not_implemented"`。已列出的指标域后续只填值、不改形状。
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
    std::optional<double> total_usage_ratio;     ///< CPU 总使用率；nullopt = 首次采样/不可用
    bool cores_collected = false;                ///< 每核序列能力是否可用（粘性）
    std::vector<platform::SysmonCpuCore> cores;  ///< 每核使用率与饱和标记
    std::vector<uint32_t> saturated_cores;       ///< 当前持续饱和的核 index 集合
    bool load_collected = false;  ///< 系统负载能力是否可用（粘性；与数值是否就绪无关）
    std::optional<platform::SysmonLoadAverage> load_average;  ///< 系统负载（口径见 semantic）
    bool rates_collected = false;  ///< 上下文切换/中断累计计数器能力是否可用（粘性）
    std::optional<double> context_switches_per_sec;
    std::optional<double> interrupts_per_sec;
};

/**
 * @brief 构造一帧实时快照。
 *
 * @param source               发送方进程名（无 instance_id 帧的来源显式携带，general §5）
 * @param system_info          采集到的原始系统信息（进程启动时采集一次）
 * @param cpu_metrics          CPU 区块的测量结果与能力标记
 * @param measure_interval_ms  当前计量间隔（毫秒）
 * @param last_sample_unix_ms  最近一次计量成功的墙钟时间（Unix 毫秒，0 = 尚无成功样本）
 * @param missed_ticks         累计错过的计量 tick 数
 * @param now_unix_ms          快照生成时刻（Unix 毫秒）
 * @return 可直接序列化的快照
 */
[[nodiscard]] platform::SysmonSnapshot build_snapshot(std::string_view source,
                                                      const platform::RawSystemInfo& system_info,
                                                      const CpuMetrics& cpu_metrics,
                                                      int32_t measure_interval_ms,
                                                      int64_t last_sample_unix_ms,
                                                      uint64_t missed_ticks,
                                                      int64_t now_unix_ms);

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_SNAPSHOT_BUILDER_H_
