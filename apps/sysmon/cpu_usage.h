#ifndef DZTRADER_SYSMON_CPU_USAGE_H_
#define DZTRADER_SYSMON_CPU_USAGE_H_

/**
 * @file cpu_usage.h
 * @brief CPU 使用率纯函数（契约 sysmon §2.7 计算语义）。
 *
 * 采集层只返回原始累计计数器，百分比/速率一律由本文件的纯函数从两次采样差值计算：
 * - 首次采样返回 std::nullopt（不得用 0 冒充）
 * - 32 位计数器回绕：上次与本次值均落在 32 位范围内且本次更小时按 2^32 回绕计算
 * - 其它下降视为计数器重置，本次输出不可用（下一采样周期重建基线）
 * - 结果钳制到 [0, 1]
 */

#include <cstdint>
#include <optional>

#include <dztrader/platform/system_collector.h>

namespace dztrader::sysmon {

/// 32 位计数器回绕跨度（2^32）。
inline constexpr uint64_t kCpuCounterWrapSpan = 1ULL << 32;

/**
 * @brief 由两次原始累计计数器计算 CPU 使用率。
 *
 * 使用率 = 1 − Δidle/Δtotal，钳制到 [0, 1]。
 *
 * @param prev 上次采样（全 0 视为首次采样）
 * @param cur  本次采样
 * @return 使用率；首次采样/计数器重置/Δtotal 为 0 时返回 std::nullopt
 */
[[nodiscard]] std::optional<double> compute_cpu_usage(const platform::RawCpuTimes& prev,
                                                      const platform::RawCpuTimes& cur) noexcept;

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_CPU_USAGE_H_
