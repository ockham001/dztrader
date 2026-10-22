// 06-sysmon 采集层真机 oracle 测试：断言两平台实现从真实 OS 系统调用拿到的原始数据满足契约不变量。
#include <dztrader/core/path.h>
#include <dztrader/platform/system_collector.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // GetCurrentProcessId：临时目录名去重（与 libs/shm/tests/test_util.h 同做法）
#endif

#if defined(__linux__)
#include "system_collector_linux_internal.h"  // 内部解析接缝（MemAvailable 异常防御）

#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstring>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace dztrader::test {

using namespace dztrader::platform;

/// 当前进程号（两平台；与 libs/shm/tests/test_util.h 的既有做法一致）。
/// 用于给临时目录名去重，避免并行跑同一测试可执行文件时互相踩目录。
uint64_t current_process_id() {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

namespace {

/// 采样窗口下限（按每核折算）：Linux USER_HZ=100，取 10 tick（≥0.1s/核）；
/// Windows 为 100ns 单位，取 5,000,000（0.5s/核），跨过 GetSystemTimes/NtQuery 的
/// 时钟中断粒度（~15.6ms）与两者之间的调用偏移。
#if defined(_WIN32)
inline constexpr uint64_t kMinDeltaPerCore = 5'000'000ULL;
#else
inline constexpr uint64_t kMinDeltaPerCore = 10ULL;
#endif

/// 每核增量之和与全局增量的允许偏差固定下限（按每核折算）：Linux 2 tick；
/// Windows 200,000（20ms），吸收两次系统调用之间的时钟中断粒度偏移。相对项取 5%。
#if defined(_WIN32)
inline constexpr uint64_t kToleranceFloorPerCore = 200'000ULL;
#else
inline constexpr uint64_t kToleranceFloorPerCore = 2ULL;
#endif

/// 条件轮询直到累计计数器推进（最多约 2s）；不以固定 sleep 作为同步手段
/// （docs/unit-test-standard.md §1.5）。
std::optional<RawCpuTimes> collect_until_advanced(SystemCollector& collector,
                                                  const RawCpuTimes& first,
                                                  uint64_t min_total_delta = 1) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        auto second = collector.collect_cpu_times();
        if (second.has_value() && second->total_ticks >= first.total_ticks + min_total_delta) {
            return second;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::nullopt;
}

/// 各核 total 增量之和；序列长度不一致或出现下降（计数器重置）时返回 0。
uint64_t core_total_delta_sum(const RawCpuTimes& first, const RawCpuTimes& second) {
    if (first.cores.size() != second.cores.size()) {
        return 0;
    }
    uint64_t sum = 0;
    for (std::size_t i = 0; i < first.cores.size(); ++i) {
        if (second.cores[i].total_ticks < first.cores[i].total_ticks) {
            return 0;
        }
        sum += second.cores[i].total_ticks - first.cores[i].total_ticks;
    }
    return sum;
}

uint64_t sum_tolerance(uint64_t delta, std::size_t cores) {
    return std::max<uint64_t>(delta / 20, static_cast<uint64_t>(cores) * kToleranceFloorPerCore);
}

/// 条件轮询直到负载采样可用（最多约 2s；Windows 的 PDH 首次采样可能尚无有效数据）。
std::optional<RawLoadAverage> load_average_until_available(SystemCollector& collector) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        auto load = collector.collect_load_average();
        if (load.has_value()) {
            return load;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
}

}  // namespace

TEST(SystemCollectorTest, SystemInfoMatchesRealMachine) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const RawSystemInfo info = collector->collect_system_info();

    // 两平台共有不变量（契约 sysmon §2.2）：主机名、OS 版本、内核版本非空，核数 > 0。
    EXPECT_FALSE(info.hostname.empty());
    EXPECT_FALSE(info.os_version.empty());
    EXPECT_FALSE(info.kernel_version.empty());
    EXPECT_GT(info.logical_cores, 0u);

    // 平台分支各自断言等价的编译平台事实。
#if defined(_WIN32)
    EXPECT_EQ(info.os_family, "windows");
    EXPECT_GT(info.os_build, 0u);  // Windows 必须报 build 号（区分 Windows 10/11）
#elif defined(__linux__)
    EXPECT_EQ(info.os_family, "linux");
    EXPECT_EQ(info.os_build, 0u);  // Linux 的 build 恒为 0（契约 sysmon §2.2）
#endif

    // boot_time_unix_sec 必须为正且不晚于当前墙钟。
    const int64_t now_unix_sec = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
    EXPECT_GT(info.boot_time_unix_sec, 0);
    EXPECT_LE(info.boot_time_unix_sec, now_unix_sec);
}

TEST(SystemCollectorTest, CpuTimesAreMonotonicCounters) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto first = collector->collect_cpu_times();
    ASSERT_TRUE(first.has_value()) << "collect_cpu_times must work on a supported host";
    EXPECT_LE(first->idle_ticks, first->total_ticks);

    const auto second = collect_until_advanced(*collector, *first);
    ASSERT_TRUE(second.has_value());
    EXPECT_GT(second->total_ticks, first->total_ticks);
    EXPECT_GE(second->idle_ticks, first->idle_ticks);
    EXPECT_LE(second->idle_ticks, second->total_ticks);
}

TEST(SystemCollectorTest, PerCoreSeriesMatchesLogicalCores) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const RawSystemInfo info = collector->collect_system_info();
#if defined(_WIN32)
    // 契约 sysmon §2.7：NT 每核查询只覆盖调用线程所在处理器组；>64 逻辑核时每核序列不可用。
    if (info.logical_cores > 64) {
        GTEST_SKIP() << "windows processor group limitation: >64 logical cores";
    }
#endif
    const auto times = collector->collect_cpu_times();
    ASSERT_TRUE(times.has_value());

    // 契约 sysmon §2.3：每核序列可用时长度恒等于逻辑核数（不可用时为空数组）。
    EXPECT_FALSE(times->cores.empty()) << "per-core cpu times must work on a supported host";
    EXPECT_EQ(times->cores.size(), static_cast<std::size_t>(info.logical_cores));

    // 每核计数器自洽：idle 不得超过 total；index 互不相同。
    std::vector<uint32_t> indexes;
    for (const auto& core : times->cores) {
        EXPECT_LE(core.idle_ticks, core.total_ticks) << "cpu" << core.index;
        indexes.push_back(core.index);
    }
    EXPECT_LE(indexes.size(), static_cast<std::size_t>(info.logical_cores));
    std::sort(indexes.begin(), indexes.end());
    EXPECT_EQ(std::adjacent_find(indexes.begin(), indexes.end()), indexes.end())
        << "per-core index must be unique";
}

TEST(SystemCollectorTest, PerCoreDeltasSumMatchesTotalWithinTolerance) {
    // 防重复计数 oracle：两次采样之间各核增量之和必须与全局总量增量在容差内一致。
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);
#if defined(_WIN32)
    if (collector->collect_system_info().logical_cores > 64) {
        GTEST_SKIP() << "windows processor group limitation: >64 logical cores";
    }
#endif

    const auto first = collector->collect_cpu_times();
    ASSERT_TRUE(first.has_value());
    ASSERT_FALSE(first->cores.empty());

    // 累计到有意义的采样窗口再比较（每核增量下限见 kMinDeltaPerCore），
    // 避免在 Linux 上因增量太小导致容差失效/告警；窗口内全局与每核都要推进。
    const uint64_t min_total_delta =
        std::max<uint64_t>(static_cast<uint64_t>(first->cores.size()) * kMinDeltaPerCore, 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::optional<RawCpuTimes> second;
    while (std::chrono::steady_clock::now() < deadline) {
        second = collector->collect_cpu_times();
        if (second.has_value() && second->cores.size() == first->cores.size() &&
            second->total_ticks >= first->total_ticks + min_total_delta &&
            core_total_delta_sum(*first, *second) >= min_total_delta) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(second->cores.size(), first->cores.size());

    uint64_t cores_total_delta = 0;
    uint64_t cores_idle_delta = 0;
    for (std::size_t i = 0; i < first->cores.size(); ++i) {
        ASSERT_GE(second->cores[i].total_ticks, first->cores[i].total_ticks) << "core " << i;
        ASSERT_GE(second->cores[i].idle_ticks, first->cores[i].idle_ticks) << "core " << i;
        cores_total_delta += second->cores[i].total_ticks - first->cores[i].total_ticks;
        cores_idle_delta += second->cores[i].idle_ticks - first->cores[i].idle_ticks;
    }
    ASSERT_GT(second->total_ticks, first->total_ticks);
    const uint64_t total_delta = second->total_ticks - first->total_ticks;
    ASSERT_GE(second->idle_ticks, first->idle_ticks);
    const uint64_t idle_delta = second->idle_ticks - first->idle_ticks;

    const auto abs_diff = [](uint64_t a, uint64_t b) { return a > b ? a - b : b - a; };
    EXPECT_LE(abs_diff(cores_total_delta, total_delta),
              sum_tolerance(total_delta, first->cores.size()))
        << "sum(cores delta)=" << cores_total_delta << " total delta=" << total_delta;
    EXPECT_LE(abs_diff(cores_idle_delta, idle_delta),
              sum_tolerance(idle_delta, first->cores.size()))
        << "sum(cores idle delta)=" << cores_idle_delta << " idle delta=" << idle_delta;
}

TEST(SystemCollectorTest, ContextSwitchAndInterruptCountersAreMonotonic) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);
#if defined(_WIN32)
    // 契约 sysmon §2.7：>64 逻辑核时中断累计计数不可用（处理器组限制），该配置下跳过而非失败。
    if (collector->collect_system_info().logical_cores > 64) {
        GTEST_SKIP() << "windows processor group limitation: >64 logical cores";
    }
#endif

    const auto first = collector->collect_cpu_times();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->context_switches.has_value()) << "context switches must be available";
    ASSERT_TRUE(first->interrupts.has_value()) << "interrupts must be available";

    const auto second = collect_until_advanced(*collector, *first);
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(second->context_switches.has_value());
    ASSERT_TRUE(second->interrupts.has_value());
    EXPECT_GE(*second->context_switches, *first->context_switches);
    EXPECT_GE(*second->interrupts, *first->interrupts);
}

TEST(SystemCollectorTest, LoadAverageReflectsPlatformSemantics) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto load = load_average_until_available(*collector);
    ASSERT_TRUE(load.has_value()) << "load average proxy must be available on a supported host";
    ASSERT_TRUE(load->one.has_value());
    EXPECT_TRUE(std::isfinite(*load->one));

    // 契约 sysmon §2.3：Linux 为真实 loadavg，Windows 为处理器队列长度代理量。
#if defined(_WIN32)
    EXPECT_EQ(load->semantic, "processor_queue");
    ASSERT_TRUE(load->five.has_value());
    ASSERT_TRUE(load->fifteen.has_value());
#elif defined(__linux__)
    EXPECT_EQ(load->semantic, "load_average");
    ASSERT_TRUE(load->five.has_value());
    ASSERT_TRUE(load->fifteen.has_value());
#endif
}

// 契约 sysmon §2.4：Linux 有 hwmon 传感器时返回非空读数且数值合理；无传感器（CI/容器）
// 报告不可用（std::nullopt）而不是失败。Windows 本版本不提供温度（必须为 std::nullopt）。
TEST(SystemCollectorTest, TemperatureReadingsFollowContract) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto temperatures = collector->collect_temperatures();
#if defined(_WIN32)
    EXPECT_FALSE(temperatures.has_value()) << "windows must not provide temperatures";
#elif defined(__linux__)
    if (!temperatures.has_value()) {
        GTEST_SKIP() << "no readable hwmon temperature sensor on this host";
    }
    EXPECT_FALSE(temperatures->empty());
    std::vector<std::string> sensors;
    for (const auto& reading : *temperatures) {
        // sensor 稳定标识形如 "<name>#<hwmonN>:<label>" / "...:tempN"，必须非空且帧内唯一
        // （标识含 hwmon 目录名，契约 sysmon §2.4；病态重复 label 追加 -<n> 兜底）。
        EXPECT_FALSE(reading.sensor.empty());
        // 锁定标识格式：含 hwmon 目录名与 label/tempN 分隔符（防回退成不含 #hwmonN 的旧格式）。
        EXPECT_NE(reading.sensor.find('#'), std::string::npos) << reading.sensor;
        EXPECT_NE(reading.sensor.find(':'), std::string::npos) << reading.sensor;
        sensors.push_back(reading.sensor);
        // 真机温度必须是有限摄氏度，且落在物理上可能的工业/环境范围内。
        EXPECT_TRUE(std::isfinite(reading.celsius)) << reading.sensor;
        EXPECT_GE(reading.celsius, -50.0) << reading.sensor;
        EXPECT_LE(reading.celsius, 200.0) << reading.sensor;
    }
    std::sort(sensors.begin(), sensors.end());
    EXPECT_EQ(std::adjacent_find(sensors.begin(), sensors.end()), sensors.end())
        << "temperature sensor identifiers must be unique within one snapshot";
#endif
}

// 契约 sysmon §2.3：Linux 有 cpufreq 时返回非空每核频率（mhz > 0、index 唯一且升序）；
// 无 cpufreq（CI/容器）报告不可用（std::nullopt）而不是失败。Windows 不提供每核频率。
TEST(SystemCollectorTest, CoreFrequenciesFollowContract) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto frequencies = collector->collect_core_frequencies();
#if defined(_WIN32)
    EXPECT_FALSE(frequencies.has_value()) << "windows must not provide core frequencies";
#elif defined(__linux__)
    if (!frequencies.has_value()) {
        GTEST_SKIP() << "no readable cpufreq scaling_cur_freq on this host";
    }
    EXPECT_FALSE(frequencies->empty());
    std::vector<uint32_t> indexes;
    for (const auto& frequency : *frequencies) {
        // mhz 为当前频率（MHz，> 0）；index 为 OS CPU 编号（与 cpu.cores[].index 同源）。
        EXPECT_GT(frequency.mhz, 0u) << "cpu" << frequency.index;
        indexes.push_back(frequency.index);
    }
    // 实现按 index 升序输出，且同一核最多出现一次。
    EXPECT_TRUE(std::is_sorted(indexes.begin(), indexes.end())) << "core indexes must be sorted";
    EXPECT_EQ(std::adjacent_find(indexes.begin(), indexes.end()), indexes.end())
        << "per-core index must be unique";
#endif
}

// #74 真机 oracle：内存读数必须满足契约 sysmon §2.4 的口径不变量（两平台同一口径）。
TEST(SystemCollectorTest, MemoryInfoFollowsContract) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto memory = collector->collect_memory();
    ASSERT_TRUE(memory.has_value()) << "collect_memory must work on a supported host";
    EXPECT_GT(memory->total_bytes, 0u);
    EXPECT_LE(memory->available_bytes, memory->total_bytes);
    if (memory->cached_bytes.has_value()) {
        EXPECT_LE(*memory->cached_bytes, memory->total_bytes);
    }
    if (memory->buffers_bytes.has_value()) {
        EXPECT_LE(*memory->buffers_bytes, memory->total_bytes);
    }
#if defined(_WIN32)
    // Windows 提供 SystemCache 缓存，但不提供 buffers（契约 sysmon §2.4）。
    EXPECT_TRUE(memory->cached_bytes.has_value());
    EXPECT_FALSE(memory->buffers_bytes.has_value());
#elif defined(__linux__)
    EXPECT_TRUE(memory->cached_bytes.has_value());
    EXPECT_TRUE(memory->buffers_bytes.has_value());
#endif
}

// #74 真机 oracle：交换读取成功且 free ≤ total；无交换分区/页面文件的机器 total = 0
// （采集成功，不是失败；由可用性映射标记 unsupported）。
TEST(SystemCollectorTest, SwapInfoFollowsContract) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto swap = collector->collect_swap();
    ASSERT_TRUE(swap.has_value()) << "swap read must succeed even without swap space";
    EXPECT_LE(swap->free_bytes, swap->total_bytes);
    // 字节量是 kB/页的整数倍：total 与 free 之差必须能被 1024 整除（Linux；Windows 页大小更小）。
#if defined(__linux__)
    EXPECT_EQ(swap->total_bytes % 1024u, 0u);
    EXPECT_EQ(swap->free_bytes % 1024u, 0u);
#endif
}

#if defined(__linux__)
namespace {

using dztrader::platform::system_collector_linux_internal::MemAvailableFallback;
using dztrader::platform::system_collector_linux_internal::normalize_memory;
using dztrader::platform::system_collector_linux_internal::normalize_swap;
using dztrader::platform::system_collector_linux_internal::parse_meminfo;

/// 典型 `/proc/meminfo` 内容（单位 kB；行为契约 sysmon §2.4、§2.7）。
constexpr std::string_view kNormalMeminfo =
    "MemTotal:       32797152 kB\n"
    "MemFree:         1000000 kB\n"
    "MemAvailable:   16398576 kB\n"
    "Buffers:          500000 kB\n"
    "Cached:          8000000 kB\n"
    "SReclaimable:     400000 kB\n"
    "SwapTotal:       8388604 kB\n"
    "SwapFree:        6291453 kB\n";

TEST(MeminfoParserTest, ParsesAndNormalizesNormalValues) {
    const auto values = parse_meminfo(kNormalMeminfo);
    const auto normalized = normalize_memory(values);
    ASSERT_TRUE(normalized.memory.has_value());
    EXPECT_EQ(normalized.fallback, MemAvailableFallback::None);
    EXPECT_EQ(normalized.memory->total_bytes, 32'797'152ULL * 1024);
    EXPECT_EQ(normalized.memory->available_bytes, 16'398'576ULL * 1024);
    // cached = Cached + SReclaimable（可回收页缓存口径）。
    EXPECT_EQ(normalized.memory->cached_bytes.value_or(0), (8'000'000ULL + 400'000ULL) * 1024);
    EXPECT_EQ(normalized.memory->buffers_bytes.value_or(0), 500'000ULL * 1024);

    const auto swap = normalize_swap(values);
    ASSERT_TRUE(swap.has_value());
    EXPECT_EQ(swap->total_bytes, 8'388'604ULL * 1024);
    EXPECT_EQ(swap->free_bytes, 6'291'453ULL * 1024);
}

TEST(MeminfoParserTest, MemAvailableZeroFallsBackAndClampsToTotal) {
    // MemAvailable = 0 是异常值：回退到 free + buffers + cached + sreclaimable 并钳制。
    const auto values = parse_meminfo(
        "MemTotal:        1000 kB\n"
        "MemFree:          100 kB\n"
        "MemAvailable:       0 kB\n"
        "Buffers:           50 kB\n"
        "Cached:           200 kB\n"
        "SReclaimable:      50 kB\n"
        "SwapTotal:          0 kB\n"
        "SwapFree:           0 kB\n");
    const auto normalized = normalize_memory(values);
    EXPECT_EQ(normalized.fallback, MemAvailableFallback::Zero);
    ASSERT_TRUE(normalized.memory.has_value());
    // 100 + 50 + 200 + 50 = 400 kB ≤ total。
    EXPECT_EQ(normalized.memory->available_bytes, 400ULL * 1024);
    EXPECT_LE(normalized.memory->available_bytes, normalized.memory->total_bytes);
}

TEST(MeminfoParserTest, MemAvailableAboveTotalFallsBackAndClamps) {
    const auto values = parse_meminfo(
        "MemTotal:        1000 kB\n"
        "MemFree:          100 kB\n"
        "MemAvailable:    4096 kB\n"
        "Buffers:           50 kB\n"
        "Cached:           200 kB\n"
        "SwapTotal:          0 kB\n"
        "SwapFree:           0 kB\n");
    const auto normalized = normalize_memory(values);
    EXPECT_EQ(normalized.fallback, MemAvailableFallback::AboveTotal);
    ASSERT_TRUE(normalized.memory.has_value());
    EXPECT_EQ(normalized.memory->available_bytes, 350ULL * 1024);
    EXPECT_LE(normalized.memory->available_bytes, normalized.memory->total_bytes);
}

TEST(MeminfoParserTest, MissingMemAvailableUsesFallback) {
    // 老内核无 MemAvailable 行：回退到可回收量之和并记日志（由采集层负责日志）。
    const auto values = parse_meminfo(
        "MemTotal:        1000 kB\n"
        "MemFree:          100 kB\n"
        "Buffers:           50 kB\n"
        "Cached:           200 kB\n"
        "SwapTotal:          0 kB\n"
        "SwapFree:           0 kB\n");
    const auto normalized = normalize_memory(values);
    EXPECT_EQ(normalized.fallback, MemAvailableFallback::Missing);
    ASSERT_TRUE(normalized.memory.has_value());
    EXPECT_EQ(normalized.memory->available_bytes, 350ULL * 1024);
}

TEST(MeminfoParserTest, FallbackWithoutRequiredFieldsFailsSafely) {
    // MemAvailable 异常且缺少回退所需字段：本次内存采集失败（不得拿 0 冒充）。
    const auto values = parse_meminfo(
        "MemTotal:        1000 kB\n"
        "MemAvailable:       0 kB\n"
        "MemFree:          100 kB\n");
    const auto normalized = normalize_memory(values);
    EXPECT_EQ(normalized.fallback, MemAvailableFallback::Zero);
    EXPECT_EQ(normalized.memory, std::nullopt);
}

TEST(MeminfoParserTest, MissingOrZeroTotalFailsSafely) {
    EXPECT_EQ(normalize_memory(parse_meminfo("MemFree: 100 kB\n")).memory, std::nullopt);
    EXPECT_EQ(normalize_memory(parse_meminfo("MemTotal: 0 kB\n")).memory, std::nullopt);
}

TEST(MeminfoParserTest, CachedAndBuffersClampedToTotal) {
    // 异常内容防御：cached/buffers 超过总量时钳制，保证快照校验不会失败。
    const auto values = parse_meminfo(
        "MemTotal:        1000 kB\n"
        "MemAvailable:     500 kB\n"
        "Buffers:         4096 kB\n"
        "Cached:          8192 kB\n");
    const auto normalized = normalize_memory(values);
    ASSERT_TRUE(normalized.memory.has_value());
    EXPECT_EQ(normalized.memory->cached_bytes.value_or(0), 1000ULL * 1024);
    EXPECT_EQ(normalized.memory->buffers_bytes.value_or(0), 1000ULL * 1024);
}

TEST(MeminfoParserTest, MalformedLinesAreIgnored) {
    // 非数字/无冒号/未知键不参与解析；同名字段以首次成功解析为准。
    const auto values = parse_meminfo(
        "garbage line without colon\n"
        "MemTotal:       1000 kB\n"
        "MemTotal:       9999 kB\n"
        "MemAvailable:  500 kB\n"
        "SwapTotal:     abc kB\n"
        "SwapFree:       100 kB\n");
    const auto normalized = normalize_memory(values);
    ASSERT_TRUE(normalized.memory.has_value());
    EXPECT_EQ(normalized.memory->total_bytes, 1000ULL * 1024);
    EXPECT_EQ(normalized.memory->available_bytes, 500ULL * 1024);
    // SwapTotal 非法 → 交换读取失败（std::nullopt），不得用 0 冒充"无交换"。
    EXPECT_EQ(normalize_swap(values), std::nullopt);
}

TEST(MeminfoParserTest, SwapWithoutSwapSpaceIsZeroNotFailure) {
    const auto values =
        parse_meminfo("MemTotal: 1000 kB\nMemAvailable: 500 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
    const auto swap = normalize_swap(values);
    ASSERT_TRUE(swap.has_value());
    EXPECT_EQ(swap->total_bytes, 0u);
    EXPECT_EQ(swap->free_bytes, 0u);
}

TEST(MeminfoParserTest, SwapFreeAboveTotalIsClamped) {
    const auto values = parse_meminfo("SwapTotal: 1000 kB\nSwapFree: 4096 kB\n");
    const auto swap = normalize_swap(values);
    ASSERT_TRUE(swap.has_value());
    EXPECT_EQ(swap->total_bytes, 1000ULL * 1024);
    EXPECT_EQ(swap->free_bytes, 1000ULL * 1024);
}

}  // namespace
// ===== #75 内部接缝：/proc/mounts 解析与落卷匹配（契约 sysmon §2.4）=====
namespace mount_test {
using system_collector_linux_internal::deduplicate_by_device;
using system_collector_linux_internal::is_pseudo_filesystem;
using system_collector_linux_internal::match_mount_point;
using system_collector_linux_internal::MountEntry;
using system_collector_linux_internal::parse_mounts;
using system_collector_linux_internal::resolve_volume_mount_point;
}  // namespace mount_test
using namespace mount_test;  // NOLINT(google-build-using-namespace)：仅测试翻译单元

TEST(MountParserTest, FiltersPseudoFilesystemsAndDeduplicates) {
    // 典型 /proc/mounts 节选：真实存储 + 各种伪文件系统 + 同挂载点重复。
    const std::string content =
        "/dev/sda2 / ext4 rw,relatime 0 0\n"
        "proc /proc proc rw,nosuid 0 0\n"
        "sysfs /sys sysfs rw,nosuid 0 0\n"
        "devtmpfs /dev devtmpfs rw,nosuid 0 0\n"
        "tmpfs /dev/shm tmpfs rw,nosuid 0 0\n"
        "tmpfs /run tmpfs rw,nosuid 0 0\n"
        "cgroup2 /sys/fs/cgroup cgroup2 rw,nosuid 0 0\n"
        "/dev/sdb1 /data xfs rw,relatime 0 0\n"
        "/dev/sdb1 /data xfs rw,relatime 0 0\n"  // bind mount 重复列出
        "overlay /var/lib/docker/overlay2/abc overlay rw 0 0\n"
        "/dev/sdc1 /mnt/my\\040disk ext4 rw 0 0\n"  // 挂载点内含空格（八进制转义）
        "/dev/sdd1 /proc-mount ext4 rw 0 0\n";      // 名字含 proc 但类型是 ext4

    const auto entries = parse_mounts(content);
    std::vector<std::string> mounts;
    for (const auto& entry : entries) {
        mounts.push_back(entry.mount_point);
    }
    // 只保留真实存储，且同挂载点去重。
    EXPECT_EQ(mounts.size(), 4U);
    EXPECT_NE(std::find(mounts.begin(), mounts.end(), "/"), mounts.end());
    EXPECT_NE(std::find(mounts.begin(), mounts.end(), "/data"), mounts.end());
    EXPECT_NE(std::find(mounts.begin(), mounts.end(), "/proc-mount"), mounts.end());
    // 八进制转义必须还原（否则匹配与展示都对不上）。
    EXPECT_NE(std::find(mounts.begin(), mounts.end(), "/mnt/my disk"), mounts.end());
    for (const auto& pseudo : {"/proc", "/sys", "/dev", "/dev/shm", "/run", "/sys/fs/cgroup"}) {
        EXPECT_EQ(std::find(mounts.begin(), mounts.end(), pseudo), mounts.end()) << pseudo;
    }
    EXPECT_EQ(std::count(mounts.begin(), mounts.end(), "/data"), 1);

    // 结果按挂载点长度降序：最长前缀匹配可直接使用。
    EXPECT_TRUE(std::is_sorted(entries.begin(), entries.end(),
                               [](const MountEntry& lhs, const MountEntry& rhs) {
                                   return lhs.mount_point.size() > rhs.mount_point.size();
                               }));
}

TEST(MountParserTest, PseudoFilterCanBeDisabledForDiagnosis) {
    // 落卷诊断需要看被过滤的文件系统（区分"路径不存在"与"卷不在采集范围"）。
    const std::string content = "proc /proc proc rw 0 0\ntmpfs /run tmpfs rw 0 0\n";
    EXPECT_TRUE(parse_mounts(content).empty());
    const auto all = parse_mounts(content, /*include_pseudo=*/true);
    EXPECT_EQ(all.size(), 2U);
}

TEST(MountParserTest, PseudoFilesystemClassification) {
    for (const char* fs :
         {"proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "cgroup", "cgroup2", "debugfs", "tracefs",
          "configfs", "mqueue", "hugetlbfs", "autofs", "overlay", "squashfs", "fuse.sshfs"}) {
        EXPECT_TRUE(is_pseudo_filesystem(fs)) << fs;
    }
    for (const char* fs : {"ext4", "xfs", "btrfs", "zfs", "nfs", "nfs4", "cifs"}) {
        EXPECT_FALSE(is_pseudo_filesystem(fs)) << fs;
    }
}

TEST(MountParserTest, LongestPrefixMatchRespectsPathComponentBoundary) {
    // 挂载点名用 /srv 系构造，避免测试源码里出现硬编码家目录路径
    // （scripts/check_test_portability.py 禁止 POSIX 家目录字面量）。
    const std::vector<std::string> mounts = {"/srv/vol/data", "/srv/vol", "/"};
    // 最长前缀优先。
    EXPECT_EQ(match_mount_point("/srv/vol/data/file", mounts), "/srv/vol/data");
    EXPECT_EQ(match_mount_point("/srv/vol/other", mounts), "/srv/vol");
    EXPECT_EQ(match_mount_point("/srv/vol", mounts), "/srv/vol");
    // 路径分量边界：/srv/vol 不得匹配 /srv/vol2（否则落卷会把 vol2 报成 vol）。
    EXPECT_EQ(match_mount_point("/srv/vol2/file", mounts), "/");
    // 根挂载点兜底。
    EXPECT_EQ(match_mount_point("/opt/other", mounts), "/");
    // 没有根挂载点时匹配不到就是 nullopt（不得猜卷）。
    const std::vector<std::string> no_root = {"/data"};
    EXPECT_EQ(match_mount_point("/opt/other", no_root), std::nullopt);
    EXPECT_EQ(match_mount_point("/data", no_root), "/data");
}

TEST(MountParserTest, MalformedMountLinesAreIgnored) {
    const std::string content =
        "\n"
        "device-only\n"
        "/dev/sda1 relative/path ext4 rw 0 0\n"  // 挂载点非绝对路径
        "/dev/sda2 / ext4 rw 0 0\n";
    const auto entries = parse_mounts(content);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].mount_point, "/");
}

TEST(MountParserTest, DeduplicateByDeviceKeepsCanonicalMountPoint) {
    // bind mount 场景：同一设备被挂到多个挂载点（真实机器上 / 与 /home/user 同设备）。
    MountEntry root;
    root.mount_point = "/";
    root.fs_type = "ext4";
    root.device = 100;
    MountEntry data;
    data.mount_point = "/data";
    data.fs_type = "ext4";
    data.device = 200;
    MountEntry bind;
    bind.mount_point = "/srv/app";
    bind.fs_type = "ext4";
    bind.device = 100;  // 与 / 同设备

    const auto deduplicated = deduplicate_by_device({root, data, bind});
    // 同一设备只保留一条，且保留最短挂载点（/ 是该设备的代表）。
    ASSERT_EQ(deduplicated.size(), 2U);
    std::vector<std::string> mounts;
    for (const auto& entry : deduplicated) {
        mounts.push_back(entry.mount_point);
    }
    EXPECT_NE(std::find(mounts.begin(), mounts.end(), "/"), mounts.end());
    EXPECT_NE(std::find(mounts.begin(), mounts.end(), "/data"), mounts.end());
    EXPECT_EQ(std::find(mounts.begin(), mounts.end(), "/srv/app"), mounts.end());
}

TEST(MountParserTest, DeduplicateKeepsUnknownDevices) {
    // device=0（stat 失败）无法判重，必须全部保留（宁可多列也不能吞掉真实卷）。
    MountEntry first;
    first.mount_point = "/a";
    first.device = 0;
    MountEntry second;
    second.mount_point = "/b";
    second.device = 0;
    const auto deduplicated = deduplicate_by_device({first, second});
    EXPECT_EQ(deduplicated.size(), 2U);
}

// 契约 sysmon §2.4：落卷按**设备身份**归位。bind mount 让同一块存储有多个挂载点视图，
// 路径前缀只能命中其中一个；卷列表里的代表挂载点必须仍然能被归到。
TEST(MountParserTest, VolumeResolutionFallsBackToDeviceIdentity) {
    // 构造一个"卷列表代表是 /data、但路径落在 bind 视图 /srv/app 下"的场景：
    // /data 与 /srv/app 同设备（这里用同一路径伪造两个挂载记录并共用 st_dev）。
    std::error_code ec;
    const auto base = dztrader::paths::home().parent_path() /
                      ("dzsysmon_bind_" + std::to_string(current_process_id()));
    std::filesystem::remove_all(base, ec);
    ASSERT_TRUE(std::filesystem::create_directories(base / "app" / "sub", ec)) << ec.message();

    struct stat info{};
    ASSERT_EQ(::stat((base / "app").c_str(), &info), 0);

    MountEntry representative;
    representative.mount_point = (base / "app").string();  // 卷列表里的代表挂载点
    representative.fs_type = "ext4";
    representative.device = static_cast<uint64_t>(info.st_dev);

    // 路径落在代表挂载点下：应该直接命中（前缀路径）。
    const auto direct =
        resolve_volume_mount_point((base / "app" / "sub").string(), {representative});
    ASSERT_TRUE(direct.has_value());
    EXPECT_EQ(*direct, representative.mount_point);

    // 路径与该卷同设备、但不在代表挂载点前缀下（模拟 bind 视图）：
    // 用设备身份归位，而不是回退到 "/"。
    MountEntry other_device;
    other_device.mount_point = "/";  // 根挂载点条目
    other_device.fs_type = "ext4";
    other_device.device = static_cast<uint64_t>(info.st_dev) + 1;  // 不同设备

    const auto via_device =
        resolve_volume_mount_point((base / "app" / "sub").string(), {other_device, representative});
    ASSERT_TRUE(via_device.has_value());
    EXPECT_EQ(*via_device, representative.mount_point)
        << "must resolve by device identity, not fall back to /";

    std::filesystem::remove_all(base, ec);
}

#endif  // defined(__linux__)

// ===== #75：卷、关注路径与共享内存目录 =====

/// 目录占用统计的临时目录 fixture：每个用例自管临时目录（AGENTS 硬门禁 2 的豁免情形）。
class ShmDirUsageTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::error_code ec;
        base_ = std::filesystem::temp_directory_path(ec) /
                ("dzsysmon_shm_" + std::to_string(current_process_id()) + "_" +
                 std::to_string(counter_++));
        std::filesystem::remove_all(base_, ec);
        ASSERT_TRUE(std::filesystem::create_directories(base_, ec)) << ec.message();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }

    /// 写一个指定大小的普通文件（内容是重复字符，不关心内容）。
    void write_file(const std::filesystem::path& path, std::size_t size) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(output.good()) << path.string();
        std::string payload(size, 'x');
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        ASSERT_TRUE(output.good()) << path.string();
    }

    std::filesystem::path base_;
    static int counter_;
};

int ShmDirUsageTest::counter_ = 0;

TEST_F(ShmDirUsageTest, CountsBytesPageFilesAndOldestAge) {
    // 布局模拟真实共享内存目录：通道子目录内放数字页文件与 meta.dat。
    const auto channel = base_ / "dzevent";
    write_file(channel / "00000000.dat", 1000);
    write_file(channel / "00000001.dat", 2000);
    write_file(channel / "meta.dat", 500);  // 非页文件：计入字节数，不计入页文件数
    write_file(base_ / "notes.txt", 100);   // 任意普通文件：只计字节
    std::error_code ec;
    std::filesystem::create_directories(base_ / "emptychannel", ec);

    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);
    const auto usage = collector->collect_shm_dir(base_.string());
    ASSERT_TRUE(usage.has_value());
    EXPECT_EQ(usage->path, base_.string());
    EXPECT_EQ(usage->total_bytes, 1000U + 2000U + 500U + 100U);
    EXPECT_EQ(usage->page_files, 2U);
    // 三个页文件都刚写入：最旧页文件年龄为 0（不是 null —— null 表示没有页文件）。
    ASSERT_TRUE(usage->oldest_file_age_sec.has_value());
    EXPECT_EQ(*usage->oldest_file_age_sec, 0);
}

TEST_F(ShmDirUsageTest, EmptyDirectoryReportsZeroNotFailure) {
    // 目录存在但为空：0 是真实事实（页清理把旧文件全删了），必须返回对象而不是 null。
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);
    const auto usage = collector->collect_shm_dir(base_.string());
    ASSERT_TRUE(usage.has_value());
    EXPECT_EQ(usage->total_bytes, 0U);
    EXPECT_EQ(usage->page_files, 0U);
    EXPECT_EQ(usage->oldest_file_age_sec, std::nullopt);
}

TEST_F(ShmDirUsageTest, MissingDirectoryIsUnavailableNotZero) {
    // 目录不存在 = 读不到，不得返回全 0 的对象冒充"目录为空"（契约 sysmon §2.4）。
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);
    const auto usage = collector->collect_shm_dir((base_ / "does_not_exist").string());
    EXPECT_EQ(usage, std::nullopt);
}

TEST_F(ShmDirUsageTest, OldestPageAgeUsesPageFilesOnly) {
    // meta.dat 很旧但页文件较新：最旧年龄必须只看页文件（页清理信号，契约 sysmon §2.4）。
    const auto channel = base_ / "dzevent";
    write_file(channel / "meta.dat", 10);
    write_file(channel / "00000000.dat", 10);
    const auto old_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
    std::error_code ec;
    std::filesystem::last_write_time(channel / "meta.dat", old_time, ec);
    ASSERT_FALSE(ec) << "cannot backdate meta.dat: " << ec.message();
    std::filesystem::last_write_time(channel / "00000000.dat", old_time, ec);
    ASSERT_FALSE(ec) << "cannot backdate page file: " << ec.message();

    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);
    const auto usage = collector->collect_shm_dir(base_.string());
    ASSERT_TRUE(usage.has_value());
    EXPECT_EQ(usage->page_files, 1U);
    ASSERT_TRUE(usage->oldest_file_age_sec.has_value());
    // 允许 1 小时的执行波动（CI 慢机），但不接受"漏算 meta.dat 导致年龄为 0"。
    EXPECT_GE(*usage->oldest_file_age_sec, 47 * 3600);
    EXPECT_LE(*usage->oldest_file_age_sec, 49 * 3600);
}

// 契约 sysmon §2.4：卷使用率关系自洽（已用 ≤ 总量、剩余 ≤ 总量、比率与字节一致）。
TEST(SystemCollectorTest, VolumesFollowContractInvariants) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto volumes = collector->collect_volumes();
    ASSERT_TRUE(volumes.has_value()) << "collect_volumes must work on a supported host";
    EXPECT_FALSE(volumes->empty());

    std::vector<std::string> mounts;
    for (const auto& volume : *volumes) {
        EXPECT_FALSE(volume.mount.empty());
        mounts.push_back(volume.mount);
        // mount 单帧内唯一（契约 sysmon §7）。
        // 可读卷必须给全容量事实；不可读卷两个字段都缺且带原因（读不到不是 0）。
        const bool readable = volume.total_bytes.has_value() && volume.free_bytes.has_value();
        if (readable) {
            EXPECT_TRUE(volume.reason.empty()) << volume.mount;
            EXPECT_LE(*volume.free_bytes, *volume.total_bytes) << volume.mount;
        } else {
            EXPECT_FALSE(volume.reason.empty()) << volume.mount;
            EXPECT_EQ(volume.reason == "no_permission" || volume.reason == "collect_failed", true)
                << volume.mount << " reason=" << volume.reason;
        }
    }
    std::sort(mounts.begin(), mounts.end());
    EXPECT_EQ(std::adjacent_find(mounts.begin(), mounts.end()), mounts.end())
        << "volume mount points must be unique within one frame";
#if defined(__linux__)
    // Linux 过滤伪文件系统：proc/sysfs/tmpfs 等不得作为卷出现（契约 sysmon §2.4）。
    for (const auto& mount : mounts) {
        EXPECT_NE(mount, "/proc");
        EXPECT_NE(mount, "/sys");
        EXPECT_NE(mount, "/dev");
        EXPECT_NE(mount, "/dev/shm");
    }
#endif
}

// 契约 sysmon §2.4：落卷解析把路径对到真实存在的卷；路径不存在时如实报告失败。
TEST(SystemCollectorTest, WatchPathsResolveToVolumes) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto volumes = collector->collect_volumes();
    ASSERT_TRUE(volumes.has_value());
    std::vector<std::string> mount_points;
    for (const auto& volume : *volumes) {
        mount_points.push_back(volume.mount);
    }

    // 用当前工作目录与其下不存在的名字驱动：前者必须解析成功，后者必须失败。
    std::error_code ec;
    const auto working_dir = std::filesystem::current_path(ec);
    ASSERT_FALSE(ec) << ec.message();
    const auto missing = (working_dir / "dzsysmon_missing_path_for_test").string();
    const std::vector<std::string> paths = {working_dir.string(), missing};

    const auto resolved = collector->collect_watch_paths(paths);
    ASSERT_EQ(resolved.size(), paths.size());

    // 已存在的绝对路径必须落到某个真实卷上，且该卷在同帧 volumes 里（契约 sysmon §7）。
    EXPECT_EQ(resolved[0].path, working_dir.string());
    ASSERT_TRUE(resolved[0].mount.has_value()) << "existing directory must resolve to a volume";
    EXPECT_NE(std::find(mount_points.begin(), mount_points.end(), *resolved[0].mount),
              mount_points.end())
        << "resolved mount must be listed in volumes: " << *resolved[0].mount;

    // 不存在的路径：不得猜卷、不得回退到 "/"，必须给出 reason（契约 sysmon §2.4）。
    EXPECT_FALSE(resolved[1].mount.has_value());
    EXPECT_FALSE(resolved[1].reason.empty());
}

// 真机 oracle：落卷解析必须跟随符号链接——报告的必须是**目标**所在卷，而不是链接名
// 所在卷（两者刻意放在同一次断言里比较，否则测试会"通过但证明不了跟随")。
TEST(SystemCollectorTest, WatchPathsFollowSymlinksToTargetVolume) {
#if defined(_WIN32)
    GTEST_SKIP() << "symlink creation requires developer mode/admin on windows";
#else
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec) /
                      ("dzsysmon_symlink_" + std::to_string(current_process_id()));
    std::filesystem::remove_all(base, ec);
    ASSERT_TRUE(std::filesystem::create_directories(base, ec)) << ec.message();

    // 目标目录与链接名放在**不同**的卷上（目标在 $DZTRADER_HOME/shm 所在卷，
    // 链接名放在临时目录卷 —— 两者在很多机器上恰好不同卷，相同也没关系：
    // 下面用"目标路径自身的落卷"作为 oracle 比较，结论对两种情况都成立）。
    const auto target = dztrader::paths::shm();
    const auto link = base / "link_to_shm";
    if (::symlink(target.c_str(), link.c_str()) != 0) {
        std::filesystem::remove_all(base, ec);
        GTEST_SKIP() << "cannot create symlink on this filesystem: " << std::strerror(errno);
    }

    const auto resolved = collector->collect_watch_paths({link.string()});
    // 目标路径本身的落卷 = 期望值（oracle）。
    const auto target_resolved = collector->collect_watch_paths({target.string()});
    std::filesystem::remove_all(base, ec);

    ASSERT_EQ(resolved.size(), 1U);
    ASSERT_EQ(target_resolved.size(), 1U);
    ASSERT_TRUE(target_resolved[0].mount.has_value()) << "shm dir must resolve to a volume";

    // 关键断言：链接被跟随，落卷等于目标落卷（而不是链接名所在目录的卷）。
    EXPECT_EQ(resolved[0].mount, target_resolved[0].mount)
        << "symlink must resolve to the target volume: link=" << link.string()
        << " target=" << target.string();
    // 规范化后路径不再包含链接名（weakly_canonical 解析了链接）。
    EXPECT_EQ(resolved[0].path.find("link_to_shm"), std::string::npos)
        << "path must be canonicalized: " << resolved[0].path;
#endif
}

// 契约 sysmon §2.4 的 AC：权限不足时报告不可用，不得产出 0。
// mode 0000 的目录仍能通过 is_directory()，若不做可读性探针就会谎报成"空目录"。
TEST_F(ShmDirUsageTest, UnreadableDirectoryIsUnavailableNotZero) {
#if defined(_WIN32)
    GTEST_SKIP() << "posix permission bits are not enforceable the same way on windows";
#else
    const auto unreadable = base_ / "noread";
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directories(unreadable, ec)) << ec.message();
    write_file(unreadable / "00000000.dat", 4096);
    ASSERT_EQ(::chmod(unreadable.c_str(), 0), 0) << std::strerror(errno);

    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);
    const auto usage = collector->collect_shm_dir(unreadable.string());

    // 恢复权限以便 TearDown 能删掉临时目录。
    ASSERT_EQ(::chmod(unreadable.c_str(), 0700), 0) << std::strerror(errno);

    // 无权限 = 读不到：必须是 nullopt，绝不能是 total_bytes=0 的对象（契约 §2.4）。
    EXPECT_EQ(usage, std::nullopt)
        << "unreadable shm dir must be unavailable, got total_bytes="
        << (usage ? std::to_string(usage->total_bytes) : std::string("<null>"));
#endif
}

// 契约 sysmon §2.4：落卷匹配的大小写语义跨平台不同——Linux 大小写敏感，
// Windows 大小写不敏感（盘符归一化后与 volumes[].mount 可直接相等比较）。
TEST(SystemCollectorTest, WatchPathMatchingRespectsPlatformCaseSemantics) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    // 基准目录必须落在**采集范围内的真实卷**上：临时目录常是 tmpfs（被 §2.4 过滤），
    // 现在的落卷解析会如实把它判为 unsupported（不得回退到 "/"），
    // 那样就测不到大小写语义了。用 DZTRADER_HOME 所在目录（ctest 注入，在项目卷上）。
    std::error_code ec;
    const auto base = dztrader::paths::home().parent_path() /
                      ("dzsysmon_case_" + std::to_string(current_process_id()));
    std::filesystem::remove_all(base, ec);
    ASSERT_TRUE(std::filesystem::create_directories(base / "Data", ec)) << ec.message();

    // 同一个真实目录，用不同大小写拼写。
    const auto exact = (base / "Data").string();
    const auto different_case = (base / "data").string();
    const auto resolved = collector->collect_watch_paths({exact, different_case});
    std::filesystem::remove_all(base, ec);
    ASSERT_EQ(resolved.size(), 2U);

    // 精确拼写必须解析成功（两平台）。
    EXPECT_TRUE(resolved[0].mount.has_value()) << "exact-case path must resolve";

#if defined(_WIN32)
    // Windows：大小写不敏感——另一种拼写同样解析成功，且落卷相等。
    // 这条断言同时覆盖盘符归一化（否则 mount 大小写与 volumes 不一致）。
    EXPECT_TRUE(resolved[1].mount.has_value()) << "windows matching must be case-insensitive";
    if (resolved[0].mount.has_value() && resolved[1].mount.has_value()) {
        EXPECT_EQ(*resolved[0].mount, *resolved[1].mount);
        // 盘符必须统一为大写，才能与 volumes[].mount 直接字符串比较（契约 §2.4/§7）。
        const auto& mount = *resolved[0].mount;
        if (mount.size() >= 1 && std::isalpha(static_cast<unsigned char>(mount[0])) != 0) {
            EXPECT_EQ(mount[0],
                      static_cast<char>(std::toupper(static_cast<unsigned char>(mount[0]))))
                << "drive letter must be normalized to uppercase: " << mount;
        }
    }
#else
    // Linux：大小写敏感——拼错的路径不存在，必须如实报 path_not_found（不猜卷）。
    EXPECT_FALSE(resolved[1].mount.has_value())
        << "linux matching must be case-sensitive, got mount="
        << resolved[1].mount.value_or("<none>");
    EXPECT_EQ(resolved[1].reason, "path_not_found");
#endif
}

}  // namespace dztrader::test
