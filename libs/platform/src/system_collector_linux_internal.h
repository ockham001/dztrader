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
#include <string>
#include <string_view>
#include <vector>

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

// ===== /proc/mounts 卷枚举与落卷解析（契约 sysmon §2.4；Linux 实现内部接缝）=====

/// `/proc/mounts` 中的一条挂载记录（已过滤伪文件系统、已做八进制转义还原）。
struct MountEntry {
    /// 挂载点（已反转义，如 `/`、`/home`）。
    std::string mount_point;
    /// 文件系统类型（如 `ext4`、`xfs`、`btrfs`、`overlay`）。
    std::string fs_type;
    /// `stat()` 得到的设备号（`st_dev`）；0 = 无法取得（挂载点不可访问）。
    /// 同一设备可有多个挂载点（bind mount），去重与落卷匹配都以设备号为准。
    uint64_t device = 0;
};

/// 解析 `/proc/mounts` 文本，过滤伪文件系统并去掉重复挂载点（契约 sysmon §2.4）。
///
/// 过滤规则：只保留"真实存储"文件系统——排除内核伪文件系统（proc/sysfs/cgroup* 等）、
/// 内存文件系统（tmpfs/ramfs/devtmpfs/devpts/mqueue/hugetlbfs）与 autofs/overlay 等
/// 无独立容量语义的挂载。结果按挂载点长度降序（供最长前缀匹配直接使用），
/// 同挂载点只保留首次出现的条目。
///
/// @param include_pseudo true 时**不做**伪文件系统过滤（仅用于落卷诊断：路径落在被过滤的
///        文件系统上时，需要区分"路径不存在"与"卷不在采集范围"）
[[nodiscard]] std::vector<MountEntry> parse_mounts(std::string_view content,
                                                   bool include_pseudo = false);

/// 判断文件系统类型是否属于"伪文件系统/非真实存储"（契约 sysmon §2.4 的过滤清单）。
[[nodiscard]] bool is_pseudo_filesystem(std::string_view fs_type);

/// 给挂载记录补上设备号（`stat()` 取 `st_dev`；取不到留 0）。
/// @return 补全后的记录；挂载点不可 `stat()` 时 `device` 为 0（调用方按不可读处理）。
[[nodiscard]] std::vector<MountEntry> attach_devices(std::vector<MountEntry> entries);

/// 按底层设备号去重（bind mount 会让同一设备出现多次），每个设备保留**最短挂载点**
/// 作为该卷的代表（`/` 优先于 `/home/user`；同长度取字典序最小以保证确定性）。
///
/// 卷的语义是"一块底层存储"（契约 sysmon §2.4），bind mount 只是同一块存储的另一个
/// 视图；重复列出会让 UI 出现多个容量完全相同的卷。`device = 0` 的记录无法判重，全部保留。
[[nodiscard]] std::vector<MountEntry> deduplicate_by_device(std::vector<MountEntry> entries);

/// 在已知挂载点集合中做最长前缀匹配，返回路径所在卷的挂载点（契约 sysmon §2.4）。
///
/// `normalized_path` 必须已是绝对路径；匹配按**路径分量边界**（`/home` 不会匹配 `/home2`）。
/// 找不到匹配返回 std::nullopt（调用方据此报 `path_not_found`，不得回退到 `/`）。
[[nodiscard]] std::optional<std::string> match_mount_point(
    std::string_view normalized_path, const std::vector<std::string>& mount_points);

/// 规范化路径（`weakly_canonical`：解析 `.`/`..` 与符号链接，保留不存在的尾部）。
/// @return 规范化后的绝对路径；无法规范化（权限/IO 错误）返回 std::nullopt。
[[nodiscard]] std::optional<std::string> normalize_path(const std::string& path);

/// 把 `/proc/meminfo` 原始值归一为交换读数（契约 sysmon §2.4）。
/// @return `SwapTotal` 或 `SwapFree` 缺失/非法时返回 `std::nullopt`（采集失败）；
///         本机无交换时返回 `total_bytes = 0` 的读数（不是失败）。
[[nodiscard]] std::optional<RawSwapInfo> normalize_swap(const MeminfoValues& values);

}  // namespace dztrader::platform::system_collector_linux_internal

#endif  // DZTRADER_PLATFORM_SYSTEM_COLLECTOR_LINUX_INTERNAL_H_
