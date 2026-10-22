#ifndef DZTRADER_PLATFORM_SYSTEM_COLLECTOR_LINUX_INTERNAL_H_
#define DZTRADER_PLATFORM_SYSTEM_COLLECTOR_LINUX_INTERNAL_H_

/**
 * @file system_collector_linux_internal.h
 * @brief Linux `/proc/meminfo` 解析与归一（系统监控采集层内部接缝）。
 *
 * 仅 Linux 实现与平台单测使用，不属于安装头文件。抽成内部接缝是为了让
 * `MemAvailable` 异常值（缺失 / 为 0 / 大于总量）的防御性回退能被单测确定性覆盖
 * （契约 sysmon §2.7）；平台实现按本接缝归一后再交给 `SystemCollector` 消费方。
 */

#include <cstdint>
#include <optional>
#include <string_view>

#include <dztrader/platform/system_collector.h>

namespace dztrader::platform::system_collector_linux_internal {

/// `/proc/meminfo` 中与内存/交换相关的原始值（单位 kB）。
///
/// 字段缺失或内容非法时为 `std::nullopt`；同名字段出现多次时以首次解析成功为准。
struct MeminfoValues {
    std::optional<uint64_t> mem_total_kb;
    std::optional<uint64_t> mem_free_kb;
    std::optional<uint64_t> mem_available_kb;
    std::optional<uint64_t> buffers_kb;
    std::optional<uint64_t> cached_kb;
    std::optional<uint64_t> sreclaimable_kb;
    std::optional<uint64_t> swap_total_kb;
    std::optional<uint64_t> swap_free_kb;
};

/// `MemAvailable` 异常时采用的回退方式（供调用方记日志，契约 sysmon §2.7）。
enum class MemAvailableFallback {
    None,        ///< MemAvailable 正常，未回退
    Missing,     ///< MemAvailable 行缺失
    Zero,        ///< MemAvailable 为 0
    AboveTotal,  ///< MemAvailable 大于 MemTotal
};

/// 内存归一的中间结果。
struct NormalizedMemory {
    /// 归一后的内存读数；`std::nullopt` = 总量不可用或回退也无法计算。
    std::optional<RawMemoryInfo> memory;
    /// 本帧是否因 MemAvailable 异常而回退（调用方据此记日志）。
    MemAvailableFallback fallback = MemAvailableFallback::None;
};

/// 解析 `/proc/meminfo` 文本。
[[nodiscard]] MeminfoValues parse_meminfo(std::string_view content);

/// 把 `/proc/meminfo` 原始值归一为 `RawMemoryInfo`（契约 sysmon §2.4、§2.7）。
///
/// `MemAvailable` 缺失、为 0 或大于总量时回退到
/// `MemFree + Buffers + (Cached + SReclaimable)` 并钳制到 `[0, total]`；
/// 回退所需字段缺失时 `memory` 为 `std::nullopt`。`cached_bytes` / `buffers_bytes`
/// 同样钳制到总量，避免异常值把快照校验打红。
[[nodiscard]] NormalizedMemory normalize_memory(const MeminfoValues& values);

/// 把 `/proc/meminfo` 原始值归一为交换读数（契约 sysmon §2.4）。
/// @return `SwapTotal` 或 `SwapFree` 缺失/非法时返回 `std::nullopt`（采集失败）；
///         本机无交换时返回 `total_bytes = 0` 的读数（不是失败）。
[[nodiscard]] std::optional<RawSwapInfo> normalize_swap(const MeminfoValues& values);

}  // namespace dztrader::platform::system_collector_linux_internal

#endif  // DZTRADER_PLATFORM_SYSTEM_COLLECTOR_LINUX_INTERNAL_H_
