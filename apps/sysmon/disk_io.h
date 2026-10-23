#ifndef DZTRADER_SYSMON_DISK_IO_H_
#define DZTRADER_SYSMON_DISK_IO_H_

/**
 * @file disk_io.h
 * @brief 每物理盘 I/O 派生值纯函数（契约 sysmon §2.4、§2.7）。
 *
 * 采集层只返回原始累计计数器（`RawDiskIoInfo`），吞吐/IOPS/延迟/队列深度/利用率
 * 一律由本文件的纯函数从两次采样差值计算：
 * - 首次采样（无基线）或采样窗口非法时全部派生字段为 `std::nullopt`（不得用 0 冒充，
 *   含瞬时 gauge `queue_depth`——保证"首次采样整块不可用"这一契约语义）
 * - 计数器回绕：仅当上次与本次值均落在回绕跨度内且本次更小时按跨度回绕计算；
 *   其余下降视为计数器重置，该字段本次输出不可用（下一采样周期重建基线）
 * - 设备集合可在帧间变化：新设备无基线、消失的设备不再输出；基线按设备标识匹配
 * - 利用率与延迟的派生口径见契约 sysmon §2.4、§2.7
 */

#include <cstdint>
#include <vector>

#include <dztrader/platform/sysmon.h>
#include <dztrader/platform/system_collector.h>

namespace dztrader::sysmon {

/// 时间类累计计数器的回绕跨度（微秒）：Linux 内核的毫秒计数器是 32 位，
/// 采集层换算为微秒后跨度为 `(1 << 32) * 1000`（契约 sysmon §2.7）。
inline constexpr uint64_t kDiskTimeCounterWrapSpanUs = (1ULL << 32) * 1000ULL;

/// 计数/字节类累计计数器的回绕跨度（32 位计数器：Windows 的 `ReadCount`/`WriteCount`
/// 为 DWORD；Linux 同名计数器为 64 位，按契约回绕规则同样以该跨度兜底）。
inline constexpr uint64_t kDiskCounterWrapSpan = 1ULL << 32;

/// 利用率合理性上限：Δ忙时间超过计量窗口 1.5 倍视为计数器口径异常
/// （如 Windows `QueryTime` 受系统时间跳变影响），本帧 `utilization` 输出 `null`，
/// 不得钳制成 100% 冒充"满载"（契约 sysmon §2.7）。
inline constexpr double kDiskUtilizationTolerance = 1.5;

/**
 * @brief 由两次原始累计计数器计算每物理盘派生值（契约 sysmon §2.4 disk_io[]）。
 *
 * 输出条目只覆盖 @p cur 中出现的设备（消失的设备不再输出），顺序与 @p cur 一致。
 * 每条目的字段口径：
 * - `read_bytes_per_sec` / `write_bytes_per_sec` = Δ字节 / 实际经过秒数
 * - `read_iops` / `write_iops` = Δ完成请求数 / 实际经过秒数
 * - `latency_ms` = Δ(读服务时间 + 写服务时间) / Δ(读请求数 + 写请求数)，单位毫秒；
 *   平均每个已完成请求的驻留时间（含排队）
 * - `queue_depth` = 本次采样时刻的在途请求数（瞬时 gauge）
 * - `utilization` = Δ忙时间 / 计量窗口，钳制到 `[0,1]`
 *
 * @param prev            上次采样的原始计数器（空 = 首次采样）
 * @param cur             本次采样的原始计数器
 * @param elapsed_seconds 两次采样之间实际经过的秒数（单调时钟）
 * @return 与 @p cur 等长的派生值；无基线/计数器重置/窗口非法的字段为 `std::nullopt`
 */
[[nodiscard]] std::vector<platform::SysmonDiskIoInfo> compute_disk_io(
    const std::vector<platform::RawDiskIoInfo>& prev,
    const std::vector<platform::RawDiskIoInfo>& cur,
    double elapsed_seconds) noexcept;

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_DISK_IO_H_
