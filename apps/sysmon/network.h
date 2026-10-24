#ifndef DZTRADER_SYSMON_NETWORK_H_
#define DZTRADER_SYSMON_NETWORK_H_

/**
 * @file network.h
 * @brief 每网卡速率派生值纯函数（契约 sysmon §2.4 `network[]`、§2.7）。
 *
 * 采集层只返回原始累计计数器（`RawNetworkInfo`），六个速率字段由本文件的纯函数从
 * 两次采样差值计算：
 * - 首次采样（无基线）或采样窗口非法时六个速率全部为 `std::nullopt`（不得用 0 冒充）
 * - 基线按 `interface_name` 匹配（不是数组下标）：本帧新出现的网卡无基线、消失的网卡
 *   不再输出，因此热插拔/虚拟网卡增删不会让其它网卡的速率错位
 * - 八个 `*_total` 按本次采样原样填：累计计数器没有"不可用"语义（契约 sysmon §7）
 * - 计数器算术复用 `cpu_usage.h` 的 `counter_delta`/`compute_rate`，不重复实现；
 *   两平台网络计数器均为 64 位（跨度 0 = 下降即重置）
 * - `hide_virtual` 的过滤发生在**快照构造**阶段，不在本函数：改开关不需要重新采集，
 *   也不影响速率基线（契约 sysmon §2.7）
 */

#include <vector>

#include <dztrader/platform/sysmon.h>
#include <dztrader/platform/system_collector.h>

namespace dztrader::sysmon {

/**
 * @brief 由两次原始累计计数器计算每网卡累计量与速率（契约 sysmon §2.4 network[]）。
 *
 * 输出条目只覆盖 @p cur 中出现的网卡（消失的网卡不再输出），顺序与 @p cur 一致。
 * 每条目的字段口径：
 * - 八个 `*_total` 直接取 @p cur 的累计计数器（不做差值）
 * - 六个 `*_per_sec` = Δ累计计数器 / 实际经过秒数（单调时钟）；首次采样、本帧新出现的
 *   网卡、计数器重置或窗口非法时为 `std::nullopt`
 * - `is_virtual` 取 @p cur 的结构性判定（过滤留待快照构造）
 *
 * @param prev            上次成功采样的原始计数器（空 = 首次采样）
 * @param cur             本次采样的原始计数器
 * @param elapsed_seconds 两次**成功采样**之间实际经过的秒数（单调时钟）
 * @return 与 @p cur 等长的派生值；无基线/计数器重置/窗口非法的速率为 `std::nullopt`
 */
[[nodiscard]] std::vector<platform::SysmonNetworkInfo> compute_network_rates(
    const std::vector<platform::RawNetworkInfo>& prev,
    const std::vector<platform::RawNetworkInfo>& cur,
    double elapsed_seconds) noexcept;

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_NETWORK_H_
