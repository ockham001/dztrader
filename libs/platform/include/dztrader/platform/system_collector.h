#ifndef DZTRADER_PLATFORM_SYSTEM_COLLECTOR_H_
#define DZTRADER_PLATFORM_SYSTEM_COLLECTOR_H_

/**
 * @file system_collector.h
 * @brief 平台无关的系统指标采集接缝（契约 sysmon）。
 *
 * 采集层只返回**原始累计计数器**与**原始系统信息**：
 * - 不含百分比、不含增量、不做速率计算（由纯函数 `compute_cpu_usage` 承担）
 * - 不返回平台条件分支（两平台实现返回同一 struct，缺失值以空串/0 表示）
 *
 * 参考实现 psutil（BSD-3）仅作规格与算法参考，见仓库根 `THIRD_PARTY_NOTICES.md`。
 * 本文件与两个平台实现不翻译其代码，仅按相同 OS API 语义重新实现。
 *
 * 线程模型：实现无状态、可在单线程事件循环中调用；两个平台实现均不抛异常
 * （系统调用失败时返回 0/空值，由上层按可用性描述标记不可用）。
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

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

/// 原始 CPU 累计时间计数器（单位由平台决定，只允许同源两次采样做差）。
struct RawCpuTimes {
    /// 累计 CPU 总时间（Linux：user+nice+system+idle+iowait+irq+softirq+steal，
    /// 不含 guest/guest_nice——二者已包含在 user/nice 中；Windows：kernel+user）。
    uint64_t total_ticks = 0;
    /// 累计空闲时间（Linux：idle+iowait，即 iowait 不计入忙；Windows：idle）。
    uint64_t idle_ticks = 0;
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

    /// 采集一次 CPU 累计计数器。
    /// @return 采集失败（如 /proc 不可读）返回 std::nullopt，上层跳过本次计量并记缺口。
    virtual std::optional<RawCpuTimes> collect_cpu_times() = 0;

protected:
    SystemCollector() = default;
};

/// 创建当前平台的采集器实现（契约 sysmon）。
std::unique_ptr<SystemCollector> make_system_collector();

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_SYSTEM_COLLECTOR_H_
