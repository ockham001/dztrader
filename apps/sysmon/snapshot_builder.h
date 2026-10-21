#ifndef DZTRADER_SYSMON_SNAPSHOT_BUILDER_H_
#define DZTRADER_SYSMON_SNAPSHOT_BUILDER_H_

/**
 * @file snapshot_builder.h
 * @brief RTN_SYSMON_SNAPSHOT payload 构造（契约 sysmon §2）。
 *
 * 本票（首版）只填系统信息与 CPU 总使用率：其余指标域输出 `null`/空数组，并在
 * `availability.metrics` 标记 `available=false + reason="not_implemented"`。
 * 已列出的指标域后续只填值、不改形状；新增指标类别按 additive 扩展（契约 sysmon §2）。
 *
 * 纯函数：不读时钟、不读配置全局状态，输入即快照的全部事实来源，便于用假数据测试。
 */

#include <cstdint>
#include <optional>
#include <string_view>

#include <dztrader/platform/sysmon.h>
#include <dztrader/platform/system_collector.h>

namespace dztrader::sysmon {

/**
 * @brief 构造一帧实时快照。
 *
 * @param source               发送方进程名（无 instance_id 帧的来源显式携带，general §5）
 * @param system_info          采集到的原始系统信息（进程启动时采集一次）
 * @param cpu_usage_ratio      CPU 总使用率；std::nullopt = 首次采样/计数器不可用
 * @param measure_interval_ms  当前计量间隔（毫秒）
 * @param last_sample_unix_ms  最近一次计量成功的墙钟时间（Unix 毫秒，0 = 尚无成功样本）
 * @param missed_ticks         累计错过的计量 tick 数
 * @param now_unix_ms          快照生成时刻（Unix 毫秒）
 * @return 可直接序列化的快照
 */
[[nodiscard]] platform::SysmonSnapshot build_snapshot(std::string_view source,
                                                      const platform::RawSystemInfo& system_info,
                                                      std::optional<double> cpu_usage_ratio,
                                                      int32_t measure_interval_ms,
                                                      int64_t last_sample_unix_ms,
                                                      uint64_t missed_ticks,
                                                      int64_t now_unix_ms);

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_SNAPSHOT_BUILDER_H_
