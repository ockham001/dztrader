// 真机 oracle 测试（两平台）：SystemCollector 接缝 + 纯函数在真实机器上的行为不变量
// （契约 sysmon §2.7；#70 AC「两平台真机 oracle」）。
//
// 与 libs/platform/tests/system_collector_test.cpp 的分工：那边断言采集层原始计数器；
// 这里从 dzsysmon 的消费面断言「百分比落在合法区间」与「首次采样不可用而非 0」。

#include "cpu_usage.h"
#include "disk_io.h"
#include "network.h"
#include "snapshot_builder.h"

#include <dztrader/core/path.h>
#include <dztrader/platform/system_collector.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

TEST(SystemCollectorOracleTest, FirstSampleIsUnavailableNotZero) {
    const auto collector = platform::make_system_collector();
    ASSERT_NE(collector, nullptr);
    const auto current = collector->collect_cpu_times();
    ASSERT_TRUE(current.has_value()) << "collect_cpu_times must work on a supported host";

    // 首次采样（无前值）必须返回不可用，而不是 0（契约 sysmon §2.7）。
    const auto usage = compute_cpu_usage(platform::RawCpuTimes{}, *current);
    EXPECT_FALSE(usage.has_value());
}

TEST(SystemCollectorOracleTest, RealSamplesYieldValidUsageRatio) {
    const auto collector = platform::make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto first = collector->collect_cpu_times();
    ASSERT_TRUE(first.has_value());
    EXPECT_LE(first->idle_ticks, first->total_ticks);

    // 累计计数器按 OS 固定粒度推进：条件轮询直到观察到增量（最多约 1s），
    // 不以固定 sleep 作为同步手段（docs/unit-test-standard.md §1.5）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::optional<platform::RawCpuTimes> second;
    while (std::chrono::steady_clock::now() < deadline) {
        second = collector->collect_cpu_times();
        if (second.has_value() && second->total_ticks > first->total_ticks) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(second.has_value());
    ASSERT_GT(second->total_ticks, first->total_ticks);
    EXPECT_GE(second->idle_ticks, first->idle_ticks);
    EXPECT_LE(second->idle_ticks, second->total_ticks);

    const auto usage = compute_cpu_usage(*first, *second);
    ASSERT_TRUE(usage.has_value());
    EXPECT_GE(*usage, 0.0);
    EXPECT_LE(*usage, 1.0);

    // 系统信息真机不变量：采集层能给出非空主机名与正的逻辑核数。
    const auto info = collector->collect_system_info();
    EXPECT_FALSE(info.hostname.empty());
    EXPECT_GT(info.logical_cores, 0U);
}

TEST(SystemCollectorOracleTest, PerCoreUsagesAndRatesValidOnRealMachine) {
    // 每核使用率与上下文切换/中断速率在真实机器上的消费面不变量（契约 sysmon §2.3、§2.7）。
    const auto collector = platform::make_system_collector();
    ASSERT_NE(collector, nullptr);

#if defined(_WIN32)
    if (collector->collect_system_info().logical_cores > 64) {
        GTEST_SKIP() << "windows processor group limitation: >64 logical cores";
    }
#endif
    const auto start = std::chrono::steady_clock::now();
    const auto first = collector->collect_cpu_times();
    ASSERT_TRUE(first.has_value());
    ASSERT_FALSE(first->cores.empty()) << "per-core data must work on a supported host";

    // 条件轮询直到全局与每核计数器均已推进（最多约 2s），不以固定 sleep 作为同步手段。
    // Windows 的 GetSystemTimes/NtQuery 有 ~15.6ms 时钟中断粒度，只看全局会读到
    // 每核尚未推进的窗口，导致每核使用率全不可用而误判失败。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::optional<platform::RawCpuTimes> second;
    while (std::chrono::steady_clock::now() < deadline) {
        second = collector->collect_cpu_times();
        if (second.has_value() && second->cores.size() == first->cores.size() &&
            second->total_ticks > first->total_ticks) {
            uint64_t core_delta = 0;
            for (std::size_t i = 0; i < first->cores.size(); ++i) {
                if (second->cores[i].total_ticks > first->cores[i].total_ticks) {
                    core_delta += second->cores[i].total_ticks - first->cores[i].total_ticks;
                }
            }
            if (core_delta > 0) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const double elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(second->cores.size(), first->cores.size());

    const auto usages = compute_core_usages(first->cores, second->cores);
    ASSERT_EQ(usages.size(), second->cores.size());
    bool any_usage = false;
    for (const auto& usage : usages) {
        if (usage.has_value()) {
            any_usage = true;
            EXPECT_GE(*usage, 0.0);
            EXPECT_LE(*usage, 1.0);
        }
    }
    EXPECT_TRUE(any_usage) << "at least one per-core usage must be computable";

    ASSERT_TRUE(first->context_switches.has_value());
    ASSERT_TRUE(first->interrupts.has_value());
    ASSERT_TRUE(second->context_switches.has_value());
    ASSERT_TRUE(second->interrupts.has_value());
    const auto context_switches_rate = compute_rate(
        *first->context_switches, *second->context_switches, elapsed_seconds, kCpuCounterWrapSpan);
    const auto interrupts_rate =
        compute_rate(*first->interrupts, *second->interrupts, elapsed_seconds, kCpuCounterWrapSpan);
    ASSERT_TRUE(context_switches_rate.has_value());
    ASSERT_TRUE(interrupts_rate.has_value());
    EXPECT_TRUE(std::isfinite(*context_switches_rate));
    EXPECT_TRUE(std::isfinite(*interrupts_rate));
}

// #73 真机 oracle：温度与每核频率的可用性映射必须与采集层事实一致——有设备时可用且有值，
// 无设备时（容器 / 虚拟机 / Windows）报告不可用且快照仍通过契约校验，测试不得失败
// （issue #73 AC：Linux 真机 oracle 在无传感器的 CI 环境下报告不可用而不失败）。
TEST(SystemCollectorOracleTest, SensorAvailabilityReflectsRealHostAndValidates) {
    const auto collector = platform::make_system_collector();
    ASSERT_NE(collector, nullptr);
    const auto info = collector->collect_system_info();

    CpuMetrics cpu_metrics;  // 本测试只关心温度/频率的可用性映射，CPU 其余域不参与断言
    TemperatureMetrics temperature_metrics;
    const auto temperatures = collector->collect_temperatures();
    if (temperatures.has_value()) {
        std::vector<platform::SysmonTemperatureInfo> sensors;
        sensors.reserve(temperatures->size());
        for (const auto& reading : *temperatures) {
            sensors.push_back({reading.sensor, reading.celsius});
        }
        temperature_metrics.collected = true;
        temperature_metrics.sensors = std::move(sensors);
    }
    const auto frequencies = collector->collect_core_frequencies();
    if (frequencies.has_value()) {
        std::vector<platform::SysmonCoreFrequency> items;
        items.reserve(frequencies->size());
        for (const auto& reading : *frequencies) {
            items.push_back({reading.index, reading.mhz});
        }
        cpu_metrics.frequency_collected = true;
        cpu_metrics.core_frequencies = std::move(items);
    }

    const auto snapshot =
        build_snapshot("dzsysmon", info, cpu_metrics, temperature_metrics, {}, DiskMetrics{},
                       NetworkMetrics{}, 1000, 0, 0, 1'800'000'000'000);
    // 可用性（能力标记）必须与采集层事实一致；无设备不是失败，只是不可用。
    EXPECT_EQ(snapshot.availability.metrics.at(platform::SYSMON_METRIC_TEMPERATURE).available,
              temperature_metrics.collected);
    EXPECT_EQ(snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_FREQUENCY).available,
              cpu_metrics.frequency_collected);
    if (temperatures.has_value()) {
        ASSERT_TRUE(snapshot.temperature.has_value());
        EXPECT_EQ(snapshot.temperature->size(), temperatures->size());
    } else {
        EXPECT_EQ(snapshot.temperature, std::nullopt);
        const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_TEMPERATURE);
        ASSERT_TRUE(entry.reason.has_value());
        if (info.os_family == "windows") {
            EXPECT_EQ(entry.reason.value(), "unsupported");
        } else {
            EXPECT_EQ(entry.reason.value(), "no_sensor");
        }
    }
    if (frequencies.has_value()) {
        ASSERT_TRUE(snapshot.cpu.core_frequencies.has_value());
        EXPECT_EQ(snapshot.cpu.core_frequencies->size(), frequencies->size());
    } else {
        EXPECT_EQ(snapshot.cpu.core_frequencies, std::nullopt);
        const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_FREQUENCY);
        ASSERT_TRUE(entry.reason.has_value());
        EXPECT_EQ(entry.reason.value(), "unsupported");
    }
    // 无论设备是否存在，快照都必须通过契约校验（报告不可用而不是产出坏帧）。
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

// #74 真机 oracle：内存/交换口径不变量（契约 sysmon §2.4）——两平台同一口径
// （used = total - available、available ≤ total）；各分量非负由 Raw* 的 uint64_t 类型
// 结构性保证，无需运行时断言。无交换不是失败（total_bytes = 0 + availability
// unsupported），快照仍通过契约校验。
TEST(SystemCollectorOracleTest, MemoryAndSwapInvariantsHoldOnRealMachine) {
    const auto collector = platform::make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto memory = collector->collect_memory();
    ASSERT_TRUE(memory.has_value()) << "collect_memory must work on a supported host";
    EXPECT_GT(memory->total_bytes, 0U);
    EXPECT_LE(memory->available_bytes, memory->total_bytes);
    if (memory->cached_bytes.has_value()) {
        EXPECT_LE(*memory->cached_bytes, memory->total_bytes);
    }
    if (memory->buffers_bytes.has_value()) {
        EXPECT_LE(*memory->buffers_bytes, memory->total_bytes);
    }
#if defined(_WIN32)
    // 契约 sysmon §2.4：Windows 提供 SystemCache 缓存，但不提供 buffers。
    EXPECT_TRUE(memory->cached_bytes.has_value());
    EXPECT_FALSE(memory->buffers_bytes.has_value());
#elif defined(__linux__)
    EXPECT_TRUE(memory->cached_bytes.has_value());
    EXPECT_TRUE(memory->buffers_bytes.has_value());
#endif

    // 无交换分区也是采集成功（total=0），不是读取失败。
    const auto swap = collector->collect_swap();
    ASSERT_TRUE(swap.has_value()) << "swap read must succeed even without swap space";
    EXPECT_LE(swap->free_bytes, swap->total_bytes);

    MemorySwapMetrics memory_metrics;
    memory_metrics.collected = true;
    memory_metrics.memory = memory;
    memory_metrics.swap_collected = true;
    memory_metrics.swap_supported = swap->total_bytes > 0;
    memory_metrics.swap = swap;
    const auto snapshot = build_snapshot("dzsysmon", collector->collect_system_info(), CpuMetrics{},
                                         TemperatureMetrics{}, memory_metrics, DiskMetrics{},
                                         NetworkMetrics{}, 1000, 0, 0, 1'800'000'000'000);
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_MEMORY).available);
    ASSERT_TRUE(snapshot.memory.has_value());
    // 唯一口径：已用 = 总量 − 可用，且各分量非负、可用 ≤ 总量。
    EXPECT_EQ(snapshot.memory->total_bytes, memory->total_bytes);
    EXPECT_EQ(snapshot.memory->used_bytes, memory->total_bytes - memory->available_bytes);
    EXPECT_LE(snapshot.memory->available_bytes, snapshot.memory->total_bytes);
    if (snapshot.memory->used_ratio.has_value()) {
        EXPECT_GE(*snapshot.memory->used_ratio, 0.0);
        EXPECT_LE(*snapshot.memory->used_ratio, 1.0);
    }
    if (snapshot.swap.has_value()) {
        EXPECT_LE(snapshot.swap->used_bytes, snapshot.swap->total_bytes);
        if (snapshot.swap->used_ratio.has_value()) {
            EXPECT_GE(*snapshot.swap->used_ratio, 0.0);
            EXPECT_LE(*snapshot.swap->used_ratio, 1.0);
        }
    }
    // 快照形状/校验必须与契约一致（含无交换机器的 total=0 对象）。
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

// #75 真机 oracle：卷使用率自洽（已用 ≤ 总量、比率与字节一致）、落卷解析对得上同帧卷、
// 共享内存目录占用可读时数值自洽。两平台同一组不变量（契约 sysmon §2.4、§7）。
TEST(SystemCollectorOracleTest, DiskDomainsHoldOnRealMachine) {
    const auto collector = platform::make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto volumes = collector->collect_volumes();
    ASSERT_TRUE(volumes.has_value()) << "collect_volumes must work on a supported host";
    ASSERT_FALSE(volumes->empty());

    // 关注路径取当前工作目录（本机一定存在），落卷必须能在同帧卷列表里找到。
    std::error_code ec;
    const auto working_dir = std::filesystem::current_path(ec);
    ASSERT_FALSE(ec) << ec.message();
    const std::vector<std::string> watch_paths = {working_dir.string()};
    const auto resolved = collector->collect_watch_paths(watch_paths);
    ASSERT_EQ(resolved.size(), 1U);
    ASSERT_TRUE(resolved[0].mount.has_value()) << "working directory must resolve to a volume";

    DiskMetrics disk;
    disk.volumes_collected = true;
    disk.volumes = volumes;
    disk.watch_paths = resolved;
    // 共享内存目录用真实 $DZTRADER_HOME/shm（ctest 经 test-base preset 注入）：
    // 这份数据同时驱动快照与下面的 oracle 比对，口径必须一致。
    const auto shm_dir_path = dztrader::paths::shm();
    disk.shm_dir = collector->collect_shm_dir(shm_dir_path.string());
    disk.shm_dir_collected = disk.shm_dir.has_value();

    const auto snapshot = build_snapshot("dzsysmon", collector->collect_system_info(), CpuMetrics{},
                                         TemperatureMetrics{}, MemorySwapMetrics{}, disk,
                                         NetworkMetrics{}, 1000, 0, 0, 1'800'000'000'000);

    // 卷：可读卷的使用率必须与字节一致（自洽 oracle）；不可读卷不得有容量数值。
    ASSERT_TRUE(snapshot.volumes.has_value());
    bool watched_seen = false;
    for (const auto& volume : *snapshot.volumes) {
        if (volume.available) {
            ASSERT_TRUE(volume.total_bytes.has_value()) << volume.mount;
            ASSERT_TRUE(volume.used_bytes.has_value()) << volume.mount;
            EXPECT_LE(*volume.used_bytes, *volume.total_bytes) << volume.mount;
            if (*volume.total_bytes > 0) {
                ASSERT_TRUE(volume.used_ratio.has_value()) << volume.mount;
                const double expected = static_cast<double>(*volume.used_bytes) /
                                        static_cast<double>(*volume.total_bytes);
                // 只断言"与字节一致"这一可失败的不变量；[0,1] 由 used ≤ total 的钳制
                // 结构性保证，再断言就是永真条件（不做）。
                EXPECT_NEAR(*volume.used_ratio, expected, 1e-9) << volume.mount;
            }
        } else {
            EXPECT_FALSE(volume.total_bytes.has_value()) << volume.mount;
            EXPECT_FALSE(volume.used_bytes.has_value()) << volume.mount;
            EXPECT_FALSE(volume.used_ratio.has_value()) << volume.mount;
            EXPECT_TRUE(volume.reason.has_value()) << volume.mount;
        }
        watched_seen = watched_seen || volume.watched;
    }
    // 落卷成功，必须有一个卷被标记为关注卷（路径感知告警的前提，#80）。
    EXPECT_TRUE(watched_seen);

    ASSERT_TRUE(snapshot.watch_paths.has_value());
    ASSERT_EQ(snapshot.watch_paths->size(), 1U);
    EXPECT_TRUE((*snapshot.watch_paths)[0].available);
    ASSERT_TRUE((*snapshot.watch_paths)[0].mount.has_value());

    // 共享内存目录占用：断言可失败的不变量（不能断言 uint64 >= 0 这类永真条件）：
    //   - 页文件数与最旧年龄是同一批文件的两种视图：有页文件 ⟺ 年龄非空；
    //   - 页文件字节数之和不可能超过目录总字节数（页文件是目录内容的子集）。
    const auto shm_usage = collector->collect_shm_dir(shm_dir_path.string());
    uint64_t page_file_bytes = 0;
    std::error_code walk_ec;
    {
        std::filesystem::recursive_directory_iterator it(
            shm_dir_path, std::filesystem::directory_options::skip_permission_denied, walk_ec);
        const std::filesystem::recursive_directory_iterator end;
        while (!walk_ec && it != end) {
            std::error_code file_ec;
            if (it->is_regular_file(file_ec) && !file_ec) {
                const auto name = it->path().filename().string();
                const bool is_page = name.size() > 4 && name.ends_with(".dat") &&
                                     std::all_of(name.begin(), name.end() - 4, [](unsigned char c) {
                                         return std::isdigit(c) != 0;
                                     });
                if (is_page) {
                    page_file_bytes += static_cast<uint64_t>(it->file_size(file_ec));
                }
            }
            it.increment(walk_ec);
        }
    }
    if (!shm_usage.has_value()) {
        // 目录不存在/不可读：合法降级，此时快照域必须为 null（不是全 0 的对象）。
        EXPECT_EQ(snapshot.shm_dir, std::nullopt);
    } else {
        ASSERT_TRUE(snapshot.shm_dir.has_value());
        EXPECT_EQ(snapshot.shm_dir->path, shm_usage->path);
        EXPECT_EQ(snapshot.shm_dir->page_files, shm_usage->page_files);
        EXPECT_LE(page_file_bytes, snapshot.shm_dir->total_bytes)
            << "page file bytes cannot exceed total directory bytes";
        EXPECT_EQ(snapshot.shm_dir->page_files > 0,
                  snapshot.shm_dir->oldest_file_age_sec.has_value())
            << "page_files and oldest_file_age_sec are two views of the same page-file set";
    }

    // 整帧必须通过契约校验（含 volumes/watch_paths 的交叉校验）。
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

// #76 真机 oracle：首次采样返回不可用而非 0（契约 sysmon §2.7）；采集能力不可用时
// 快照必须报告「不可用」+ 采集层给出的 reason，不得输出 0（Windows 计数器不可用是常态）。
TEST(SystemCollectorOracleTest, DiskIoFirstSampleIsUnavailableNotZero) {
    const auto collector = platform::make_system_collector();
    ASSERT_NE(collector, nullptr);
    const auto first = collector->collect_disk_io();

    const auto info = collector->collect_system_info();
    if (!first.unavailable_reason.empty()) {
        // 采集能力不可用：失败原因由采集层给出，快照构造直接映射（不按平台名猜）。
        DiskMetrics disk_metrics;
        disk_metrics.disk_io_unavailable_reason = first.unavailable_reason;
        const auto snapshot = build_snapshot("dzsysmon", info, CpuMetrics{}, TemperatureMetrics{},
                                             MemorySwapMetrics{}, disk_metrics, NetworkMetrics{},
                                             1000, 0, 0, 1'800'000'000'000);
        EXPECT_EQ(snapshot.disk_io, std::nullopt);
        const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_DISK_IO);
        EXPECT_FALSE(entry.available);
        ASSERT_TRUE(entry.reason.has_value());
        EXPECT_EQ(entry.reason.value(), first.unavailable_reason);
        EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
        return;
    }

    // 首次采样（无基线）：设备条目存在，但全部派生字段 null——不得用 0 冒充。
    const auto derived = compute_disk_io({}, first.disks, 1.0);
    ASSERT_EQ(derived.size(), first.disks.size());
    for (const auto& disk : derived) {
        EXPECT_FALSE(disk.device.empty());
        EXPECT_FALSE(disk.read_bytes_per_sec.has_value()) << disk.device;
        EXPECT_FALSE(disk.write_bytes_per_sec.has_value()) << disk.device;
        EXPECT_FALSE(disk.read_iops.has_value()) << disk.device;
        EXPECT_FALSE(disk.write_iops.has_value()) << disk.device;
        EXPECT_FALSE(disk.latency_ms.has_value()) << disk.device;
        EXPECT_FALSE(disk.queue_depth.has_value()) << disk.device;
        EXPECT_FALSE(disk.utilization.has_value()) << disk.device;
    }

    // 快照层同样必须是"条目存在 + 派生字段 null + 能力可用"（不是整块不可用）。
    DiskMetrics disk_metrics;
    disk_metrics.disk_io_collected = true;
    disk_metrics.disk_io = derived;
    const auto snapshot =
        build_snapshot("dzsysmon", info, CpuMetrics{}, TemperatureMetrics{}, MemorySwapMetrics{},
                       disk_metrics, NetworkMetrics{}, 1000, 0, 0, 1'800'000'000'000);
    ASSERT_TRUE(snapshot.disk_io.has_value());
    EXPECT_EQ(snapshot.disk_io->size(), first.disks.size());
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_DISK_IO).available);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

// #76 真机 oracle：第二次采样用实际经过时间算出的吞吐/IOPS/延迟/利用率落在合法区间，
// 且整帧通过契约校验（契约 sysmon §2.4、§2.7）。
TEST(SystemCollectorOracleTest, DiskIoDerivedValuesHoldOnRealMachine) {
    const auto collector = platform::make_system_collector();
    ASSERT_NE(collector, nullptr);
    const auto first = collector->collect_disk_io();
    if (!first.unavailable_reason.empty()) {
        GTEST_SKIP() << "disk performance counters unavailable on this host";
    }

    // 条件轮询直到任一累计计数器推进（最多约 2s）；不写文件、不以固定 sleep 作为同步手段。
    // 只保留最近一次**成功**采样：超时前最后一次采集失败不应让本用例假红。
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(2);
    platform::RawDiskIoSnapshot second;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto sample = collector->collect_disk_io();
        if (!sample.unavailable_reason.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        second = sample;
        bool advanced = false;
        for (const auto& current : second.disks) {
            for (const auto& previous : first.disks) {
                if (current.device == previous.device &&
                    current.read_count + current.write_count >
                        previous.read_count + previous.write_count) {
                    advanced = true;
                }
            }
        }
        if (advanced) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const double elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_FALSE(second.disks.empty()) << "collect_disk_io must keep working within one sampling";

    const auto derived = compute_disk_io(first.disks, second.disks, elapsed_seconds);
    ASSERT_EQ(derived.size(), second.disks.size());
    bool any_value = false;
    for (const auto& disk : derived) {
        for (const auto* field :
             {&disk.read_bytes_per_sec, &disk.write_bytes_per_sec, &disk.read_iops,
              &disk.write_iops, &disk.latency_ms, &disk.queue_depth}) {
            if (field->has_value()) {
                any_value = true;
                // 派生值必须有限（NaN/Infinity 说明计数窗口或公式异常）；
                // 非负由 Δ计数器 ≥ 0 结构性保证，不再重复断言永真条件。
                EXPECT_TRUE(std::isfinite(**field)) << disk.device;
            }
        }
        if (disk.utilization.has_value()) {
            EXPECT_GE(*disk.utilization, 0.0) << disk.device;
            EXPECT_LE(*disk.utilization, 1.0) << disk.device;
        }
    }
    // 有基线 + 正窗口时速率字段必然可计算；全 null 说明派生链路断开。
    EXPECT_TRUE(any_value) << "derived disk io values must not be all null after a valid window";

    DiskMetrics disk_metrics;
    disk_metrics.disk_io_collected = true;
    disk_metrics.disk_io = derived;
    const auto snapshot = build_snapshot("dzsysmon", collector->collect_system_info(), CpuMetrics{},
                                         TemperatureMetrics{}, MemorySwapMetrics{}, disk_metrics,
                                         NetworkMetrics{}, 1000, 0, 0, 1'800'000'000'000);
    ASSERT_TRUE(snapshot.disk_io.has_value());
    EXPECT_EQ(snapshot.disk_io->size(), second.disks.size());
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

// #77 真机 oracle：首次采样速率返回不可用而非 0（契约 sysmon §2.7）；采集不成功时
// 快照必须报告「不可用」+ 采集层给出的 reason，且整帧仍通过契约校验。
TEST(SystemCollectorOracleTest, NetworkFirstSampleIsUnavailableNotZero) {
    const auto collector = platform::make_system_collector();
    ASSERT_NE(collector, nullptr);
    const auto first = collector->collect_network();
    const auto info = collector->collect_system_info();

    if (!first.unavailable_reason.empty()) {
        // 采集能力不可用：失败原因由采集层给出，快照构造直接映射（不按平台名猜）。
        NetworkMetrics network_metrics;
        network_metrics.network_unavailable_reason = first.unavailable_reason;
        const auto snapshot = build_snapshot("dzsysmon", info, CpuMetrics{}, TemperatureMetrics{},
                                             MemorySwapMetrics{}, DiskMetrics{}, network_metrics,
                                             1000, 0, 0, 1'800'000'000'000);
        EXPECT_EQ(snapshot.network, std::nullopt);
        const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_NETWORK);
        EXPECT_FALSE(entry.available);
        ASSERT_TRUE(entry.reason.has_value());
        EXPECT_EQ(entry.reason.value(), first.unavailable_reason);
        EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
        return;
    }

    // 首次采样（无基线）：条目存在但六个速率 null——不得用 0 冒充。
    const auto derived = compute_network_rates({}, first.interfaces, 1.0);
    ASSERT_EQ(derived.size(), first.interfaces.size());
    for (const auto& interface : derived) {
        EXPECT_FALSE(interface.interface_name.empty());
        EXPECT_FALSE(interface.rx_bytes_per_sec.has_value()) << interface.interface_name;
        EXPECT_FALSE(interface.tx_bytes_per_sec.has_value()) << interface.interface_name;
        EXPECT_FALSE(interface.rx_errors_per_sec.has_value()) << interface.interface_name;
        EXPECT_FALSE(interface.tx_errors_per_sec.has_value()) << interface.interface_name;
        EXPECT_FALSE(interface.rx_drops_per_sec.has_value()) << interface.interface_name;
        EXPECT_FALSE(interface.tx_drops_per_sec.has_value()) << interface.interface_name;
    }

    // 快照层同样必须是"条目存在 + 速率 null + 能力可用"，且默认 hide_virtual 下
    // 不出现 is_virtual=true 的条目（发送方义务，契约 sysmon §7）。
    NetworkMetrics network_metrics;
    network_metrics.network_collected = true;
    network_metrics.network = derived;
    network_metrics.hide_virtual = true;
    const auto snapshot =
        build_snapshot("dzsysmon", info, CpuMetrics{}, TemperatureMetrics{}, MemorySwapMetrics{},
                       DiskMetrics{}, network_metrics, 1000, 0, 0, 1'800'000'000'000);
    ASSERT_TRUE(snapshot.network.has_value());
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_NETWORK).available);
    for (const auto& interface : *snapshot.network) {
        EXPECT_FALSE(interface.is_virtual) << interface.interface_name;
    }
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

// #77 真机 oracle：条件轮询到第二次采样（计数器推进后）后，六个速率非负且有限，
// 且整帧通过契约校验（契约 sysmon §2.4、§2.7）。不写文件、不用固定 sleep 作同步。
TEST(SystemCollectorOracleTest, NetworkRatesHoldOnRealMachine) {
    const auto collector = platform::make_system_collector();
    ASSERT_NE(collector, nullptr);
    const auto first = collector->collect_network();
    if (!first.unavailable_reason.empty()) {
        GTEST_SKIP() << "network counters unavailable on this host";
    }

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(2);
    platform::RawNetworkSnapshot second;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto sample = collector->collect_network();
        if (!sample.unavailable_reason.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        second = sample;
        // 任一 interface 的任一计数器推进即可结束轮询（速率才有可断言的非平凡值）。
        bool advanced = false;
        for (const auto& current : second.interfaces) {
            for (const auto& previous : first.interfaces) {
                if (current.interface_name == previous.interface_name &&
                    (current.rx_bytes > previous.rx_bytes || current.tx_bytes > previous.tx_bytes ||
                     current.rx_packets > previous.rx_packets ||
                     current.tx_packets > previous.tx_packets)) {
                    advanced = true;
                }
            }
        }
        if (advanced) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const double elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_FALSE(second.interfaces.empty())
        << "collect_network must keep working within one sampling";

    const auto derived =
        compute_network_rates(first.interfaces, second.interfaces, elapsed_seconds);
    ASSERT_EQ(derived.size(), second.interfaces.size());
    // 有基线且窗口为正（real elapsed > 0）时，两侧都存在的网卡的速率必然可计算。
    bool any_rate = false;
    for (const auto& interface : derived) {
        for (const auto* field : {&interface.rx_bytes_per_sec, &interface.tx_bytes_per_sec,
                                  &interface.rx_errors_per_sec, &interface.tx_errors_per_sec,
                                  &interface.rx_drops_per_sec, &interface.tx_drops_per_sec}) {
            if (field->has_value()) {
                any_rate = true;
                // 速率必须有限（NaN/Infinity 说明计数窗口或公式异常）；
                // 非负由 Δ计数器 ≥ 0 结构性保证，不再重复断言永真条件（与 disk_io 用例同口径）。
                EXPECT_TRUE(std::isfinite(**field)) << interface.interface_name;
            }
        }
    }
    EXPECT_TRUE(any_rate) << "network rates must not be all null after a valid window";

    NetworkMetrics network_metrics;
    network_metrics.network_collected = true;
    network_metrics.network = derived;
    network_metrics.hide_virtual = false;
    const auto snapshot = build_snapshot("dzsysmon", collector->collect_system_info(), CpuMetrics{},
                                         TemperatureMetrics{}, MemorySwapMetrics{}, DiskMetrics{},
                                         network_metrics, 1000, 0, 0, 1'800'000'000'000);
    ASSERT_TRUE(snapshot.network.has_value());
    EXPECT_EQ(snapshot.network->size(), derived.size());
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

}  // namespace
}  // namespace dztrader::sysmon
