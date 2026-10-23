#ifndef DZTRADER_PLATFORM_SYSTEM_COLLECTOR_H_
#define DZTRADER_PLATFORM_SYSTEM_COLLECTOR_H_

/**
 * @file system_collector.h
 * @brief 平台无关的系统指标采集接缝（契约 sysmon）。
 *
 * 采集层只返回**原始累计计数器**、**原始系统信息**与**原始瞬时读数**（负载/温度/频率）：
 * - 不含百分比、不含增量、不做速率计算（由纯函数 `compute_cpu_usage` 承担）
 * - 不返回平台条件分支（两平台实现返回同一 struct，缺失值以空串/0/std::nullopt 表示）
 *
 * 参考实现 psutil（BSD-3）仅作规格与算法参考，见仓库根 `THIRD_PARTY_NOTICES.md`。
 * 本文件与两个平台实现不翻译其代码，仅按相同 OS API 语义重新实现。
 *
 * 线程模型：实现可在单线程事件循环中调用、不抛异常（系统调用失败时返回 0/空值或
 * std::nullopt，由上层按可用性描述标记不可用）。Windows 的负载代理实现持有 PDH
 * 查询句柄与指数加权状态（进程内单线程访问）。
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dztrader::platform {

/// 原始系统信息快照（采集一次，进程生命周期内基本不变）。
struct RawSystemInfo {
    std::string os_family;       ///< "windows" / "linux"
    std::string os_version;      ///< 人类可读版本（如 "Windows 10 Pro" / "Ubuntu 24.04.1 LTS"）
    std::string kernel_version;  ///< Windows 为 NT 版本号（如 "10.0.19045"）；Linux 为内核 release
    std::string arch;            ///< 架构标识（"x86_64"）
    std::string hostname;        ///< 主机名（取不到时为空串）
    std::string cpu_model;       ///< CPU 型号（取不到时为空串）
    uint32_t logical_cores = 0;  ///< 逻辑核数（取不到时 0）
    uint32_t os_build = 0;       ///< Windows build 号（区分 Win10/Win11）；Linux 为 0
    int64_t boot_time_unix_sec = 0;  ///< 启动时间（Unix 秒；取不到时 0）
};

/// 单核累计 CPU 时间计数器（单位与 RawCpuTimes 相同，只允许同源两次采样做差）。
struct RawCpuCoreTimes {
    /// 核 index（OS CPU 编号：Linux `cpuN` 的 N；Windows 处理器序号）。不保证连续。
    uint32_t index = 0;
    /// 累计 CPU 总时间（Linux：同全局口径；Windows：kernel+user，kernel 已含 idle）。
    uint64_t total_ticks = 0;
    /// 累计空闲时间（Linux：idle+iowait；Windows：idle）。
    uint64_t idle_ticks = 0;
};

/// 原始 CPU 累计计数器（全局 + 每核 + 上下文切换/中断）。
struct RawCpuTimes {
    /// 累计 CPU 总时间（Linux：user+nice+system+idle+iowait+irq+softirq+steal，
    /// 不含 guest/guest_nice——二者已包含在 user/nice 中；Windows：kernel+user）。
    uint64_t total_ticks = 0;
    /// 累计空闲时间（Linux：idle+iowait，即 iowait 不计入忙；Windows：idle）。
    uint64_t idle_ticks = 0;
    /// 每核累计计数器；空 = 本平台/本次采集不可用（契约 sysmon §2.3）。
    std::vector<RawCpuCoreTimes> cores;
    /// 累计上下文切换次数；std::nullopt = 本平台/本次采集不可用。
    std::optional<uint64_t> context_switches;
    /// 累计中断次数；std::nullopt = 本平台/本次采集不可用。
    std::optional<uint64_t> interrupts;
};

/// `RawLoadAverage.semantic` 的取值（契约 sysmon §2.3）：口径标识，消费方据此渲染。
inline constexpr const char* SYSMON_LOAD_SEMANTIC_LOAD_AVERAGE = "load_average";
inline constexpr const char* SYSMON_LOAD_SEMANTIC_PROCESSOR_QUEUE = "processor_queue";

/// 原始系统负载（口径由平台决定，`semantic` 标明，契约 sysmon §2.3）。
struct RawLoadAverage {
    std::optional<double> one;      ///< 1 分钟（Linux）；处理器队列长度代理值（Windows）
    std::optional<double> five;     ///< 5 分钟（Linux）；Windows 为同一代理量的衰减平均
    std::optional<double> fifteen;  ///< 15 分钟（Linux）；Windows 为同一代理量的衰减平均
    /// 口径标识：Linux `"load_average"`；Windows `"processor_queue"`（代理量，语义不同）。
    std::string semantic;
};

/// 单个温度传感器原始读数（契约 sysmon §2.4）。
struct RawTemperature {
    /// 传感器稳定标识：Linux 为 `<hwmon name>#<hwmonN>:<label>`（无 label 时为
    /// `<hwmon name>#<hwmonN>:tempN`；含 hwmon 目录名，保证帧内唯一且同启动周期稳定）。
    std::string sensor;
    /// 摄氏温度。
    double celsius = 0.0;
};

/// 单核当前频率原始读数（契约 sysmon §2.3）。
struct RawCoreFrequency {
    /// 核 index（OS CPU 编号，与 `RawCpuCoreTimes::index` 同源）。不保证连续。
    uint32_t index = 0;
    /// 当前频率（MHz）。
    uint32_t mhz = 0;
};

/// 原始内存读数（契约 sysmon §2.4；瞬时 gauge，不做差值/聚合）。
///
/// 两平台共用 `available` 口径（Windows 上"空闲"与"可用"是同一个量，
/// 即 `PhysicalAvailable`；不得改用 Linux 的 `MemFree` 语义）。
struct RawMemoryInfo {
    uint64_t total_bytes = 0;      ///< 物理内存总量（> 0；为 0 视为采集失败）
    uint64_t available_bytes = 0;  ///< 可用内存总量（无交换即可分配的估计值）
    /// 可回收文件页缓存（Linux `Cached + SReclaimable`；Windows `SystemCache`）。
    /// std::nullopt = 本平台不提供/本次未读到。
    std::optional<uint64_t> cached_bytes;
    /// 内核文件系统元数据缓存（仅 Linux `Buffers`）；Windows 恒为 std::nullopt。
    std::optional<uint64_t> buffers_bytes;
};

/// 原始卷读数（契约 sysmon §2.4；采集层只给原始容量事实，不做使用率计算）。
///
/// `collect_volumes()` 返回的每个条目都**已经解决了"卷存在但读不到"的区分**：
/// 读不到容量的卷也必须出现在返回数组里，此时 `total_bytes`/`free_bytes` 为 nullopt，
/// 由上层映射为 `available=false` + reason，不得用 0 冒充。
struct RawVolumeInfo {
    /// 规范化挂载点（Linux 挂载目录；Windows 盘符根，如 `C:\`）。单帧内唯一。
    std::string mount;
    /// 卷总容量（字节）；std::nullopt = 卷存在但容量读不到（无权限/不可访问）。
    std::optional<uint64_t> total_bytes;
    /// 卷剩余容量（字节）；std::nullopt 语义同 `total_bytes`。
    std::optional<uint64_t> free_bytes;
    /// 容量读不到时的原因（英文、小写开头、无句号）；可读时为空串。
    /// 取 `no_permission`（权限不足）或 `collect_failed`（其它读取失败）。
    std::string reason;
};

/// 关注路径的落卷解析结果（契约 sysmon §2.4）。
///
/// 采集层负责路径规范化（跟随符号链接）与卷挂载点匹配（Linux 最长前缀、大小写敏感；
/// Windows 大小写不敏感、分隔符统一）；解析失败时 `mount` 为 std::nullopt 且
/// `reason` 必填——**不得**猜一个卷或回退到 `/`。
struct RawWatchPathInfo {
    std::string path;                  ///< 解析用路径（绝对路径，符号链接已跟随）
    std::optional<std::string> mount;  ///< 所在卷挂载点；std::nullopt = 解析失败
    /// 解析失败原因（英文、小写开头、无句号）；成功时为空串。
    std::string reason;
};

/// 原始共享内存目录占用（契约 sysmon §2.4）。
struct RawShmDirInfo {
    std::string path;          ///< 共享内存根目录绝对路径
    uint64_t total_bytes = 0;  ///< 目录下全部普通文件字节数之和（递归）
    uint64_t page_files = 0;   ///< 页文件数量（递归，`<数字>.dat`；不含 meta.dat）
    /// 最旧页文件的年龄（秒，0 = 刚写入）；std::nullopt = 无页文件。
    std::optional<int64_t> oldest_file_age_sec;
};

/// 原始交换分区/页文件读数（契约 sysmon §2.4）。
struct RawSwapInfo {
    /// 交换总量；0 = 本机无交换分区/无页面文件（采集本身成功，不是失败）。
    uint64_t total_bytes = 0;
    /// 未使用的交换量；total_bytes = 0 时为 0。
    uint64_t free_bytes = 0;
};

/// 原始每物理盘 I/O 累计计数器（契约 sysmon §2.4；吞吐/IOPS/延迟/队列深度由纯函数计算）。
///
/// 时间字段统一为**微秒**：Linux `/proc/diskstats` 的毫秒计数器 ×1000，
/// Windows `DISK_PERFORMANCE` 的 100ns 计数器 ÷10。Linux 的毫秒计数器是 32 位，
/// 其微秒表示的回绕跨度是 `(1 << 32) * 1000` 微秒（契约 sysmon §2.7 的回绕处理）；
/// 计数/字节字段在 Windows 上分别来自 32 位 DWORD / 64 位 LARGE_INTEGER。
struct RawDiskIoInfo {
    /// 设备标识：Linux 为内核块设备名（`sda` / `nvme0n1`）；Windows 为 `PhysicalDrive0`
    /// 形式（与参考实现一致的物理盘编号，不含 `\\.\` 前缀）。
    std::string device;
    uint64_t read_count = 0;     ///< 累计完成的读请求数
    uint64_t write_count = 0;    ///< 累计完成的写请求数
    uint64_t read_bytes = 0;     ///< 累计读取字节数
    uint64_t write_bytes = 0;    ///< 累计写入字节数
    uint64_t read_time_us = 0;   ///< 累计读请求服务时间（微秒）
    uint64_t write_time_us = 0;  ///< 累计写请求服务时间（微秒）
    /// 累计设备忙时间（微秒；"至少有一个 I/O 在处理"的墙钟时间）；
    /// std::nullopt = 本平台不提供/本次未读到。
    std::optional<uint64_t> busy_time_us;
    /// 采样时刻的在途请求数（瞬时 gauge；不是区间平均队列深度）；
    /// std::nullopt = 本平台不提供/本次未读到。
    std::optional<uint64_t> in_flight;
};

/// 系统指标采集接缝。
///
/// 唯一实现入口：`make_system_collector()`（按编译平台返回对应实现）。
/// 测试用假采集器实现本接口即可确定性驱动快照编码与速率计算。
class SystemCollector {
public:
    virtual ~SystemCollector() = default;

    SystemCollector(const SystemCollector&) = delete;
    SystemCollector& operator=(const SystemCollector&) = delete;

    /// 采集系统信息（进程启动时调用一次；实现内部可缓存）。
    virtual RawSystemInfo collect_system_info() = 0;

    /// 采集一次 CPU 累计计数器（全局 + 每核 + 上下文切换/中断）。
    /// @return 采集失败（如 /proc 不可读）返回 std::nullopt，上层跳过本次计量并记缺口。
    virtual std::optional<RawCpuTimes> collect_cpu_times() = 0;

    /// 采集一次系统负载（Linux 真实 loadavg；Windows 处理器队列长度代理）。
    /// @return 采集失败/本平台不可用返回 std::nullopt，上层按可用性描述标记不可用。
    virtual std::optional<RawLoadAverage> collect_load_average() = 0;

    /// 采集温度传感器读数（Linux hwmon；Windows 不提供）。
    /// @return 本平台不支持 / 无温度传感器 / 全部读取失败返回 std::nullopt；
    ///         至少一个传感器可读时返回非空数组（读取失败的单个传感器不进入数组）。
    virtual std::optional<std::vector<RawTemperature>> collect_temperatures() = 0;

    /// 采集每核当前频率（Linux cpufreq；Windows 不提供）。
    /// @return 本平台不支持 / 无 cpufreq / 全部读取失败返回 std::nullopt；
    ///         至少一个核可读时返回非空数组（读取失败的单个核不进入数组）。
    virtual std::optional<std::vector<RawCoreFrequency>> collect_core_frequencies() = 0;

    /// 采集内存读数（瞬时 gauge；契约 sysmon §2.4）。
    /// @return 读取失败/总量不可用返回 std::nullopt；成功时 total_bytes > 0。
    virtual std::optional<RawMemoryInfo> collect_memory() = 0;

    /// 采集交换分区/页文件读数（瞬时 gauge；契约 sysmon §2.4）。
    /// @return 读取失败返回 std::nullopt；本机无交换时返回 total_bytes = 0 的读数
    ///         （"无交换"不是采集失败，由上层可用性映射标记 unsupported）。
    virtual std::optional<RawSwapInfo> collect_swap() = 0;

    /// 采集本机卷列表与容量（契约 sysmon §2.4；Linux 过滤伪文件系统，Windows 覆盖固定盘）。
    ///
    /// 单个卷容量读不到（无权限/不可访问）时该条目仍须出现、`total_bytes`/`free_bytes`
    /// 为 std::nullopt；只有**一个卷都枚举不到**时才返回 std::nullopt（整体采集失败）。
    /// 同一挂载点只保留一条。
    virtual std::optional<std::vector<RawVolumeInfo>> collect_volumes() = 0;

    /// 把关注路径解析到所在卷（契约 sysmon §2.4；默认共享内存目录与数据库目录）。
    ///
    /// 每条路径都必须有结果条目：解析失败时 `mount` 为 std::nullopt 且 `reason` 非空
    /// （`path_not_found` / `no_permission` / `collect_failed`）。返回空数组表示调用方
    /// 没有关注路径（不是失败）。
    virtual std::vector<RawWatchPathInfo> collect_watch_paths(
        const std::vector<std::string>& paths) = 0;

    /// 采集共享内存目录占用（契约 sysmon §2.4；页清理是否生效的可观测项）。
    /// @param shm_dir 共享内存根目录（由调用方按 `paths::shm()` 传入，便于测试用临时目录驱动）
    /// @return 目录不存在/不可读返回 std::nullopt（不得返回全 0 的对象伪装成功）。
    virtual std::optional<RawShmDirInfo> collect_shm_dir(const std::string& shm_dir) = 0;

    /// 采集每物理盘 I/O 累计计数器（契约 sysmon §2.4、§2.7）。
    ///
    /// 采集范围只含整块物理设备：Linux 以 `/sys/block` 的整盘条目为准（分区不在其下），
    /// 并排除 loop/ram/zram/dm-/md/sr 等虚拟或光学设备；Windows 遍历 `\\.\PhysicalDriveN`
    /// 并用 `IOCTL_DISK_PERFORMANCE` 读取物理盘累计计数器（该能力依赖性能计数器已开启与
    /// 访问权限，不可用是常态，见契约 sysmon §2.4）。
    ///
    /// 单个设备读不到时该设备**不出现**在结果里（契约 disk_io[] 没有 per-device 不可用表示）；
    /// **一个设备都读不到**时返回 std::nullopt，由上层按可用性映射标记不可用
    /// （Windows 计数器需特权/未开启；Linux `/proc` 不可读）。
    /// @return 空数组 = 采集成功但本机没有物理盘（Linux；不是失败——Windows 打不开任何
    ///         盘号时无法区分"没有盘"与"全部无权限"，恒以 std::nullopt 表示不可用）。
    virtual std::optional<std::vector<RawDiskIoInfo>> collect_disk_io() = 0;

protected:
    SystemCollector() = default;
};

/// 创建当前平台的采集器实现（契约 sysmon）。
std::unique_ptr<SystemCollector> make_system_collector();

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_SYSTEM_COLLECTOR_H_
