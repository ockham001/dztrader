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
 *
 * hwmon 温度扫描同理：注入假 hwmon 根即可确定性覆盖病态读数过滤（#258，契约 sysmon §2.4），
 * 不依赖真机驱动是否暴露无效通道。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
///
/// 输出顺序是确定性的（挂载点长度升序、同长度按字典序），但**不是**契约语义：
/// 契约 sysmon §2.4 不要求 `volumes[]` 顺序，消费方按使用率等自行排序。不要为
/// "恢复某种顺序"而加排序——它只是去重挑选代表的副产物。
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

// ===== hwmon 温度传感器扫描（契约 sysmon §2.4；Linux 实现内部接缝）=====

/// 一次 hwmon 扫描的归一结果。
struct TemperatureScanResult {
    /// 通过合理性窗口的读数；没有任何可读且合理的通道时为 `std::nullopt`
    /// （与 `SystemCollector::collect_temperatures()` 的失败降级口径一致）。
    std::optional<std::vector<RawTemperature>> readings;
    /// 因换算后超出合理性窗口而被跳过的通道数（调用方据此去重记日志）。
    /// 读取失败/内容非法的通道**不计入**——它们不是"读数越界"。
    uint32_t implausible_channels = 0;
};

/// 扫描 hwmon 根目录读取温度传感器读数（契约 sysmon §2.4）。
///
/// 每个 hwmon 目录以 `name` 文件内容为传感器前缀（缺失时回退目录名，如 "hwmon0"）；
/// `tempN_input` 为毫摄氏度，除以 1000 得摄氏温度；标识为
/// `<name>#<hwmonN>:<label>`，无 label 时为 `<name>#<hwmonN>:tempN`。hwmon 目录名唯一，
/// 因此标识在同一启动周期内稳定，且不随其它同名芯片出现/消失变化。单个通道读取失败
/// 只跳过该条；没有任何通过合理性窗口的通道时 `readings` 为 `std::nullopt`
/// （失败降级，不抛异常）。
///
/// **合理性过滤（#258）**：换算后超出契约 sysmon §2.4 温度合理性窗口的通道视为病态读数
/// （典型是未初始化通道的 `-273.15 °C` 占位值），跳过该通道并计入 `implausible_channels`；
/// 过滤是逐通道的，同一芯片的其它正常通道照常返回。
///
/// 读数同时携带 `chip`（hwmon `name`）与 `label`（`tempN_label` 原值，缺失为空串），
/// 供展示层分组与友好命名（#126）——标识的拼接规则仍只在本函数一处。
///
/// @param hwmon_root hwmon 类根目录，默认 `/sys/class/hwmon`；测试可注入假根，
///        用临时目录确定性覆盖病态读数过滤，不依赖真机驱动
[[nodiscard]] TemperatureScanResult read_temperatures(
    const std::filesystem::path& hwmon_root = "/sys/class/hwmon");

/// 病态温度读数告警的去重状态机（契约 sysmon §2.4：状态首次出现时记一条 WARN，
/// 持续存在不逐帧刷屏，恢复正常后重置）。
///
/// @param implausible_channels 本帧被合理性窗口过滤的通道数（0 = 本帧没有病态读数）
/// @param logged 调用方持有的去重状态；函数按本帧结果就地更新
/// @return 本帧是否应记一条 WARN
[[nodiscard]] bool should_log_implausible_temperatures(uint32_t implausible_channels, bool& logged);

// ===== /proc/<pid>/comm 同步服务进程扫描（契约 sysmon §2.9；Linux 实现内部接缝）=====

/// 本版本检测的时间同步服务清单（契约 sysmon §2.9 平台口径的单一真相源）。
///
/// **顺序即 `daemons[]` 的输出顺序**（固定，使同一状态在任何机器上产出同一帧）。
/// 只含三种 Linux 常见服务：`chronyd` / `ntpd` / `systemd-timesyncd`；不检测非主流服务
/// （契约 §8「没有的东西」）。
inline constexpr std::array<std::string_view, 3> kTimeSyncServiceNames = {"chronyd", "ntpd",
                                                                          "systemd-timesyncd"};

/// 内核 `comm` 字段的可观察长度上限：`TASK_COMM_LEN` 为 16，含结尾 `'\0'`，
/// 因此**至多 15 个字符**（`/proc/<pid>/comm` 返回 comm + '\n'）。
/// 契约 sysmon §2.9 的 `systemd-timesyncd` 实际被截断为 `systemd-timesyn`，检测按
/// 这个事实匹配（而不是把未截断名当作唯一写法）。
inline constexpr std::size_t kTaskCommMaxLength = 15;

/// 扫描进程表，返回每个可读 PID 的进程名（`/proc/<pid>/comm` 去空白后的内容）。
///
/// 不可读的 PID 目录（无 `comm`、权限不足）与非数字目录（`self` / `sys` / `net`）
/// 直接跳过；进程表整体不可读时返回空数组（不抛异常）。输出顺序不保证，
/// 消费方按服务清单排序（见 `matches_time_sync_service`）。
///
/// @param proc_root `/proc` 根目录，默认 `/proc`；测试可注入假根，用临时目录确定性
///        覆盖精确匹配与截断匹配，不依赖真机恰好跑着同步服务
[[nodiscard]] std::vector<std::string> scan_process_names(
    const std::filesystem::path& proc_root = "/proc");

/// 从进程名列表里挑出正在运行的同步服务（契约 sysmon §2.9）。
///
/// **按进程名精确匹配**（不按前缀、不按命令行猜测）：`chronyd-watcher` / `mychronyd` /
/// `chronyc` 都不算。对 `systemd-timesyncd` 额外接受内核截断形态 `systemd-timesyn`
/// （`kTaskCommMaxLength` 的可观察事实）。
///
/// 输出：只含**在运行**的服务（`running` 恒为 true），按 `kTimeSyncServiceNames` 顺序，
/// 每种服务最多一条（同一服务起两个进程只列一次——`daemons[]` 是「有哪些服务」）。
[[nodiscard]] std::vector<RawTimeSyncService> matches_time_sync_service(
    const std::vector<std::string>& process_names);

/// 读内核「时钟是否已同步」判据（契约 sysmon §2.9）：`adjtimex` 只读模式
/// （`modes = 0`，普通用户可读）取 `status & STA_UNSYNC` 取反。
///
/// @return 读不到（调用失败/平台不支持）时 `std::nullopt`——「读不到」是独立事实，
///         不得用 `false` 冒充
[[nodiscard]] std::optional<bool> read_kernel_synced();

/// 把 `adjtimex` 的 `status` 位图映射为「内核认为时钟已同步」（契约 sysmon §2.9）。
///
/// 纯函数：判据只有 `STA_UNSYNC`（未置位 = 已同步），其它标志位（`STA_PLL` / `STA_NANO`
/// 等）不影响结论。抽出来是为了让映射方向可确定性断言——写反会让「已同步」与「未同步」
/// 互换，真机上只表现为卡片颜色不对，没有任何编译期信号。
///
/// @param adjtimex_status `struct timex::status` 原值
[[nodiscard]] bool kernel_synced_from_status(int adjtimex_status);

}  // namespace dztrader::platform::system_collector_linux_internal

#endif  // DZTRADER_PLATFORM_SYSTEM_COLLECTOR_LINUX_INTERNAL_H_
