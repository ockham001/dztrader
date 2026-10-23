#ifndef DZTRADER_SYSMON_CPU_USAGE_H_
#define DZTRADER_SYSMON_CPU_USAGE_H_

/**
 * @file cpu_usage.h
 * @brief CPU 使用率/速率纯函数（契约 sysmon §2.7 计算语义）。
 *
 * 采集层只返回原始累计计数器，百分比/速率一律由本文件的纯函数从两次采样差值计算：
 * - 首次采样返回 std::nullopt（不得用 0 冒充）
 * - 32 位计数器回绕：上次与本次值均落在 32 位范围内且本次更小时按 2^32 回绕计算
 * - 其它下降视为计数器重置，本次输出不可用（下一采样周期重建基线）
 * - 结果钳制到 [0, 1]
 */

#include <cstdint>
#include <optional>
#include <vector>

#include <dztrader/platform/system_collector.h>

namespace dztrader::sysmon {

/// 32 位计数器回绕跨度（2^32）。
inline constexpr uint64_t kCpuCounterWrapSpan = 1ULL << 32;

/**
 * @brief 计算单个累计计数器的增量（契约 sysmon §2.7 的回绕/重置规则）。
 *
 * 本次 ≥ 上次 ⇒ 正常增量；否则仅当 `wrap_span != 0` 且两次值都落在跨度内时按跨度回绕，
 * 其余下降视为计数器重置。
 *
 * @param prev      上次累计计数
 * @param cur       本次累计计数
 * @param wrap_span 该计数器的 32 位回绕跨度；0 = 64 位计数器（下降一律视为重置）
 * @return 增量；计数器重置时返回 std::nullopt
 */
[[nodiscard]] std::optional<uint64_t> counter_delta(uint64_t prev,
                                                    uint64_t cur,
                                                    uint64_t wrap_span) noexcept;

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

/**
 * @brief 由两次原始每核累计计数器计算每核使用率（契约 sysmon §2.3、§2.7）。
 *
 * 每个核用与总量相同的 `1 − Δidle/Δtotal` 公式。核序列长度或 index 序列不一致
 * （热插拔/处理器组变化）时视为基线失效：返回与 @p cur 等长的全 nullopt，
 * 调用方据此重建基线（契约 sysmon §2.7）。
 *
 * @param prev 上次采样的每核计数器
 * @param cur  本次采样的每核计数器
 * @return 与 @p cur 等长的使用率；nullopt = 该核本次不可用
 */
[[nodiscard]] std::vector<std::optional<double>> compute_core_usages(
    const std::vector<platform::RawCpuCoreTimes>& prev,
    const std::vector<platform::RawCpuCoreTimes>& cur) noexcept;

/**
 * @brief 由两次原始累计计数器计算每秒速率（契约 sysmon §2.7）。
 *
 * 速率 = Δcount / 实际经过秒数；回绕/重置规则见 counter_delta。
 *
 * @param prev            上次累计计数
 * @param cur             本次累计计数
 * @param elapsed_seconds 两次采样之间实际经过的秒数（单调时钟）
 * @param wrap_span       该计数器的 32 位回绕跨度；默认 CPU 口径（2^32），
 *                        0 = 64 位计数器（下降一律视为重置）
 * @return 每秒速率；计数器重置或 elapsed_seconds ≤ 0 时返回 std::nullopt
 */
[[nodiscard]] std::optional<double> compute_rate(uint64_t prev,
                                                 uint64_t cur,
                                                 double elapsed_seconds,
                                                 uint64_t wrap_span = kCpuCounterWrapSpan) noexcept;

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_CPU_USAGE_H_
