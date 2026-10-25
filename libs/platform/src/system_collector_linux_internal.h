#ifndef DZTRADER_PLATFORM_SYSTEM_COLLECTOR_LINUX_INTERNAL_H_
#define DZTRADER_PLATFORM_SYSTEM_COLLECTOR_LINUX_INTERNAL_H_

/**
 * @file system_collector_linux_internal.h
 * @brief Linux `/proc`、sysfs 解析与归一（系统监控采集层内部接缝）。
 *
 * 仅 Linux 实现与平台单测使用，不属于安装头文件。抽成内部接缝是为了让
 * `MemAvailable` 异常值（缺失 / 为 0 / 大于总量）的防御性回退能被单测确定性覆盖
 * （契约 sysmon §2.7）；平台实现按本接缝归一后再交给 `SystemCollector` 消费方。
 *
 * `/proc/net/dev` 与虚拟网卡判定同样在此：判定依据是 sysfs 结构（`<if>/device` 是否存在），
 * 与网卡名无关，注入假 sysfs 根即可用临时目录确定性覆盖（契约 sysmon §2.4）。
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

// ===== /proc/mounts 卷枚举（契约 sysmon §2.4；Linux 实现内部接缝）=====

/// `/proc/mounts` 中的一条挂载记录（已过滤伪文件系统、已做八进制转义还原）。
struct MountEntry {
    /// 挂载点（已反转义，如 `/`、`/home`）。
    std::string mount_point;
    /// 文件系统类型（如 `ext4`、`xfs`、`btrfs`、`overlay`）。
    std::string fs_type;
    /// `stat()` 得到的设备号（`st_dev`）；0 = 无法取得（挂载点不可访问）。
    /// 同一设备可有多个挂载点（bind mount），去重以设备号为准。
    uint64_t device = 0;
};

/// 解析 `/proc/mounts` 文本，过滤伪文件系统并去掉重复挂载点（契约 sysmon §2.4）。
///
/// 过滤规则：只保留"真实存储"文件系统——排除内核伪文件系统（proc/sysfs/cgroup* 等）、
/// 内存文件系统（tmpfs/ramfs/devtmpfs/devpts/mqueue/hugetlbfs）与 autofs/overlay 等
/// 无独立容量语义的挂载。同挂载点只保留首次出现的条目。
[[nodiscard]] std::vector<MountEntry> parse_mounts(std::string_view content);

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

/// 把 `/proc/meminfo` 原始值归一为交换读数（契约 sysmon §2.4）。
/// @return `SwapTotal` 或 `SwapFree` 缺失/非法时返回 `std::nullopt`（采集失败）；
///         本机无交换时返回 `total_bytes = 0` 的读数（不是失败）。
[[nodiscard]] std::optional<RawSwapInfo> normalize_swap(const MeminfoValues& values);

// ===== /proc/diskstats 每物理盘 I/O 计数器（契约 sysmon §2.4；Linux 实现内部接缝）=====

/// `/proc/diskstats` 中一条记录的原始计数器（已换算为采集接缝口径）。
///
/// 扇区按内核固定口径 ×512 得字节（与参考实现一致）；内核的毫秒计数器 ×1000 得微秒。
struct DiskstatsEntry {
    std::string device;          ///< 内核块设备名（`sda` / `nvme0n1` / `sda1`）
    uint64_t read_count = 0;     ///< 累计完成的读请求数
    uint64_t write_count = 0;    ///< 累计完成的写请求数
    uint64_t read_bytes = 0;     ///< 累计读取字节数（扇区数 × 512）
    uint64_t write_bytes = 0;    ///< 累计写入字节数（扇区数 × 512）
    uint64_t read_time_us = 0;   ///< 累计读服务时间（微秒）
    uint64_t write_time_us = 0;  ///< 累计写服务时间（微秒）
    uint64_t busy_time_us = 0;   ///< 累计设备忙时间（微秒）
    uint64_t in_flight = 0;      ///< 采样时刻在途请求数（瞬时 gauge）
};

/// 解析 `/proc/diskstats` 文本（契约 sysmon §2.4、§2.7）。
///
/// 只解析内核 2.6+ 的整盘行格式（14 字段），以及 4.18+ 的 ≥18 字段格式（新增
/// discard/flush 字段，前 11 项统计语义不变）；7 字段的分区行（老内核）与无法解析的
/// 行直接跳过（不是整体失败）。分区行在新内核上也是完整字段，因此**本函数不做**过滤，
/// 由 `select_physical_disks` 按 `/sys/block` 候选集过滤。
[[nodiscard]] std::vector<DiskstatsEntry> parse_diskstats(std::string_view content);

/// 判断 `/sys/block` 下的块设备名是否属于"物理盘"（契约 sysmon §2.4）。
///
/// 排除：内存/回环设备（loop/ram/zram）、设备映射与软件 RAID 聚合（dm-/md/bcache）、
/// 网络块设备（nbd/rbd/drbd）、光学/软驱/包写入设备（sr/sg/fd/pktcdvd）以及其它
/// 虚拟块设备（ublk/vduse/zd）。物理盘 I/O 计在底层设备上，聚合设备计入会造成重复计数。
[[nodiscard]] bool is_physical_disk_name(std::string_view name);

/// 按 `/sys/block` 候选设备名过滤解析结果（契约 sysmon §2.4）。
///
/// 只保留候选集里的设备（候选来自 `/sys/block` 的整盘条目，分区天然不在其中），
/// 输出顺序与候选顺序一致（候选应已排序，保证帧内顺序确定）；没有统计行的候选跳过。
[[nodiscard]] std::vector<DiskstatsEntry> select_physical_disks(
    std::vector<DiskstatsEntry> entries, const std::vector<std::string>& candidates);

// ===== /proc/net/dev 每网卡累计计数器与虚拟网卡判定（契约 sysmon §2.4；Linux 实现内部接缝）=====

/// `/proc/net/dev` 中一条接口记录的原始累计计数器（契约 sysmon §2.4）。
///
/// 字段口径与 `RawNetworkInfo` 一致：收/发 × 字节/包/错误/丢弃；两平台均为 64 位
/// 计数器（跨度 0 = 不回绕，下降即重置，契约 sysmon §2.7）。
struct NetDevEntry {
    std::string interface_name;  ///< 内核接口名（`/proc/net/dev` 行首冒号前的内容）
    uint64_t rx_bytes = 0;       ///< 累计接收字节数
    uint64_t rx_packets = 0;     ///< 累计接收包数（内核口径含单播与组播/广播）
    uint64_t rx_errors = 0;      ///< 累计接收错误数
    uint64_t rx_drops = 0;       ///< 累计接收丢弃数
    uint64_t tx_bytes = 0;       ///< 累计发送字节数
    uint64_t tx_packets = 0;     ///< 累计发送包数
    uint64_t tx_errors = 0;      ///< 累计发送错误数
    uint64_t tx_drops = 0;       ///< 累计发送丢弃数
};

/// 解析 `/proc/net/dev` 文本（契约 sysmon §2.4）。
///
/// 每行格式为 `<接口名>: <16 个累计计数器>`（收 8 项：bytes packets errs drop fifo frame
/// compressed multicast；发 8 项：bytes packets errs drop fifo colls carrier compressed）。
/// 只取与契约字段同源的 8 个计数器，其余内核附加项忽略。
///
/// 两行表头（`Inter-|` 与 `face |`）与任何非法/截断行**跳过**（不是整体失败）：表头没有
/// 冒号后的 16 个数字，天然被过滤。接口名按**最后一个冒号**切分——内核接口名允许含冒号
/// （老式 `eth0:1` 别名），统计字段全是十进制数字，因此最后一个冒号一定是分隔符。
/// 同一接口名只保留首次出现的行（与 `parse_diskstats` 同口径）。
[[nodiscard]] std::vector<NetDevEntry> parse_net_dev(std::string_view content);

/// 判定网卡是否**虚拟**（不由硬件设备支撑；契约 sysmon §2.4）。
///
/// 结构性判据：sysfs 中 `<sysfs_net_root>/<接口名>/device` 是否存在——有 device 链接表示
/// 该接口背后有真实硬件设备；回环、桥接、veth、docker0、VLAN、隧道等逻辑接口都没有它。
/// **不看名字前缀**（契约明文禁止）。
///
/// `/sys/class/net/<接口名>` 本身是符号链接、其下的 `device` 也是符号链接，
/// 因此必须用跟随符号链接的 `exists()`（`symlink_status()` 不跟随，会把两者都误判成
/// "没有 device"）。判定失败（如 sysfs 不可读）按虚拟处理：宁可多隐藏一个未知接口，
/// 也不把虚拟网卡当作硬件设备输出。
///
/// @param interface_name 内核接口名
/// @param sysfs_net_root sysfs 网络类根目录，默认 `/sys/class/net`；测试可注入假根
///        （契约 sysmon §2.4 的判定因此不依赖真实网卡名）
[[nodiscard]] bool is_virtual_interface(std::string_view interface_name,
                                        std::string_view sysfs_net_root = "/sys/class/net");

}  // namespace dztrader::platform::system_collector_linux_internal

#endif  // DZTRADER_PLATFORM_SYSTEM_COLLECTOR_LINUX_INTERNAL_H_
