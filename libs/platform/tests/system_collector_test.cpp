// 06-sysmon 采集层真机 oracle 测试：断言两平台实现从真实 OS 系统调用拿到的原始数据满足契约不变量。
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
        // chip / label 是 additive 字段（#126，契约 sysmon §2.4）：chip 非空且必须等于
        // sensor 中 `#` 之前的部分（hwmon 的 name）；label 允许空串（该通道没有标签）。
        EXPECT_FALSE(reading.chip.empty()) << reading.sensor;
        EXPECT_EQ(reading.sensor.substr(0, reading.sensor.find('#')), reading.chip)
            << reading.sensor;
        // sensor 冒号之后的部分 = label（非空时）或 tempN 通道名（label 为空时）。
        // 病态驱动同一 hwmon 下重复 label 时，采集层给 sensor 追加 `-<n>` 兜底
        // （契约 §2.4），此时通道名是 `<label>-<n>`；因此这里比对**前缀**而不是全等。
        const auto colon = reading.sensor.find(':');
        const std::string channel = reading.sensor.substr(colon + 1);
        if (!reading.label.empty()) {
            EXPECT_EQ(channel.compare(0, reading.label.size(), reading.label), 0)
                << "channel=" << channel << " label=" << reading.label;
        } else {
            EXPECT_EQ(channel.rfind("temp", 0), 0u) << reading.sensor;
        }
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
// ===== #75 内部接缝：/proc/mounts 解析与按设备去重（契约 sysmon §2.4）=====
namespace mount_test {
using system_collector_linux_internal::deduplicate_by_device;
using system_collector_linux_internal::is_pseudo_filesystem;
using system_collector_linux_internal::MountEntry;
using system_collector_linux_internal::parse_mounts;
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
}

TEST(MountParserTest, BareFuseIsFilteredButFuseblkIsARealVolume) {
    // #122：裸 `fuse` 的临时挂载被过滤（GNOME remote desktop 的 cliprdr），
    // `fuseblk`（内核 fuse 模块承载块设备上的真实文件系统，典型是 ntfs-3g 的 NTFS 卷）
    // 必须留下来。
    const std::string content =
        "fuse /run/user/1000/cliprdr-abc fuse rw,nosuid,nodev 0 0\n"
        "fuse.sshfs /srv/remote fuse.sshfs rw 0 0\n"
        "/dev/sda2 / ext4 rw,relatime 0 0\n"
        "/dev/sdb1 /mnt/ntfs fuseblk rw,nosuid 0 0\n";
    const auto entries = parse_mounts(content);
    std::vector<std::string> mounts;
    for (const auto& entry : entries) {
        mounts.push_back(entry.mount_point);
    }
    EXPECT_EQ(mounts.size(), 2U);
    EXPECT_NE(std::find(mounts.begin(), mounts.end(), "/"), mounts.end());
    EXPECT_NE(std::find(mounts.begin(), mounts.end(), "/mnt/ntfs"), mounts.end());
    EXPECT_EQ(std::find(mounts.begin(), mounts.end(), "/run/user/1000/cliprdr-abc"), mounts.end());
    EXPECT_EQ(std::find(mounts.begin(), mounts.end(), "/srv/remote"), mounts.end());
}

TEST(MountParserTest, PseudoFilesystemClassification) {
    for (const char* fs :
         {"proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "cgroup", "cgroup2", "debugfs", "tracefs",
          "configfs", "mqueue", "hugetlbfs", "autofs", "overlay", "squashfs", "fuse.sshfs",
          // #122：裸 `fuse` 不带实现后缀（如 GNOME remote desktop 的 cliprdr 临时挂载），
          // 其 statvfs 返回 0 块，df 默认也不列它。
          "fuse"}) {
        EXPECT_TRUE(is_pseudo_filesystem(fs)) << fs;
    }
    for (const char* fs : {"ext4", "xfs", "btrfs", "zfs", "nfs", "nfs4", "cifs",
                           // #122：`fuseblk` 承载块设备上的真实文件系统（典型是 ntfs-3g 的
                           // NTFS 卷），过滤掉会隐藏真实存储。
                           "fuseblk"}) {
        EXPECT_FALSE(is_pseudo_filesystem(fs)) << fs;
    }
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

#endif  // defined(__linux__)

// ===== #76 内部接缝：/proc/diskstats 解析与物理盘过滤（契约 sysmon §2.4）=====
#if defined(__linux__)
namespace diskstats_test {
using system_collector_linux_internal::is_physical_disk_name;
using system_collector_linux_internal::parse_diskstats;
using system_collector_linux_internal::select_physical_disks;
}  // namespace diskstats_test
using namespace diskstats_test;  // NOLINT(google-build-using-namespace)：仅测试翻译单元

TEST(DiskstatsParserTest, ParsesWholeDiskLineIntoCollectorUnits) {
    // 真实 /proc/diskstats 节选（5.5+ 格式：20 字段，前 11 项统计语义是内核 ABI）。
    const std::string content =
        "   8       0 sda 5979440 1325698 194490474 2837705 4142227 9585008 "
        "412194506 16779003 0 5045106 21121076 11340 0 275034960 13410 474211 1490955\n";

    const auto entries = parse_diskstats(content);
    ASSERT_EQ(entries.size(), 1U);
    const auto& disk = entries[0];
    EXPECT_EQ(disk.device, "sda");
    EXPECT_EQ(disk.read_count, 5979440ULL);
    EXPECT_EQ(disk.write_count, 4142227ULL);
    // 扇区固定按 512 字节换算（内核 ABI 口径）。
    EXPECT_EQ(disk.read_bytes, 194490474ULL * 512ULL);
    EXPECT_EQ(disk.write_bytes, 412194506ULL * 512ULL);
    // 内核的毫秒计数器换算为微秒（采集接缝统一口径）。
    EXPECT_EQ(disk.read_time_us, 2837705ULL * 1000ULL);
    EXPECT_EQ(disk.write_time_us, 16779003ULL * 1000ULL);
    EXPECT_EQ(disk.busy_time_us, 5045106ULL * 1000ULL);
    EXPECT_EQ(disk.in_flight, 0ULL);
}

TEST(DiskstatsParserTest, ParsesLegacyFourteenFieldFormat) {
    // 2.6+ 无 discard/flush 字段的 14 字段整盘行。
    const std::string content = "   8       0 sda 10 20 1024 30 40 50 2048 60 2 70 80\n";
    const auto entries = parse_diskstats(content);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].read_count, 10ULL);
    EXPECT_EQ(entries[0].read_bytes, 1024ULL * 512ULL);
    EXPECT_EQ(entries[0].write_bytes, 2048ULL * 512ULL);
    EXPECT_EQ(entries[0].read_time_us, 30ULL * 1000ULL);
    EXPECT_EQ(entries[0].write_time_us, 60ULL * 1000ULL);
    EXPECT_EQ(entries[0].in_flight, 2ULL);
    EXPECT_EQ(entries[0].busy_time_us, 70ULL * 1000ULL);
}

TEST(DiskstatsParserTest, SkipsMalformedAndLegacyShapedLines) {
    const std::string content =
        "\n"
        "garbage line without numbers\n"
        "   8       0 sda 1 2 3\n"                                         // 老内核分区行（7 字段）
        "   8       0 sdb x 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18\n"  // 非数字统计项
        "   8       0 sdc 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18\n"  // 合法 20 字段
        "   3       0       8 hda 8 8 8 8 8 8 8 8 8 8 8\n"     // 2.4 格式（15 字段，name 在索引 3）
        "   8      16 sdb 11 12 13 14 15 16 17 18 0 19 20\n";  // 合法 14 字段行
    const auto entries = parse_diskstats(content);
    ASSERT_EQ(entries.size(), 2U);
    // 非数字统计项被跳过；合法行按文件顺序保留（覆盖解析失败分支，而不是只靠字段数分支）。
    EXPECT_EQ(entries[0].device, "sdc");
    EXPECT_EQ(entries[1].device, "sdb");
}

TEST(DiskstatsParserTest, PhysicalDiskNameFilterExcludesVirtualAndOpticalDevices) {
    // 物理盘（含虚机/云主机的虚拟磁盘）必须保留。
    for (const char* name :
         {"sda", "sdb", "nvme0n1", "nvme1n2", "vda", "xvda", "mmcblk0", "pmem0", "dasda"}) {
        EXPECT_TRUE(is_physical_disk_name(name)) << name;
    }
    // 聚合设备（dm-/md/bcache）、内存/网络/光学/软驱/虚拟块设备必须排除，
    // 否则与底层物理盘重复计数或把非磁盘计入（契约 sysmon §2.4）。
    for (const char* name :
         {"loop0", "ram0", "zram0", "dm-0", "md0", "md127", "bcache0", "nbd0", "rbd0", "drbd0",
          "sr0", "sg0", "fd0", "pktcdvd0", "ublk0", "vduse0", "zd0"}) {
        EXPECT_FALSE(is_physical_disk_name(name)) << name;
    }
}

TEST(DiskstatsParserTest, SelectPhysicalDisksKeepsCandidateOrderAndDropsPartitions) {
    const std::string content =
        "   8       0 sda 1 0 2048 10 2 0 4096 20 0 30 40\n"
        "   8       1 sda1 1 0 1024 10 2 0 2048 20 0 30 40\n"  // 分区：不在 /sys/block 候选集
        "   8      16 sdb 3 0 4096 30 4 0 8192 40 1 50 60\n";
    const auto entries = parse_diskstats(content);
    ASSERT_EQ(entries.size(), 3U);
    // 候选顺序（/sys/block 排序结果）优先，分区被剔除。
    const auto selected = select_physical_disks(entries, {"sdb", "sda"});
    ASSERT_EQ(selected.size(), 2U);
    EXPECT_EQ(selected[0].device, "sdb");
    EXPECT_EQ(selected[1].device, "sda");
    // 候选设备没有统计行：跳过（不伪造 0）。
    EXPECT_TRUE(select_physical_disks(entries, {"sdc"}).empty());
}
#endif  // defined(__linux__)

// ===== #75：卷与共享内存目录 =====

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

// ===== #76 真机 oracle：每物理盘 I/O 累计计数器（契约 sysmon §2.4、§2.7）=====
TEST(SystemCollectorTest, DiskIoCountersFollowContractInvariants) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto first = collector->collect_disk_io();
    if (!first.unavailable_reason.empty()) {
        // Windows 的物理盘性能计数器依赖计数器开启与访问权限，不可用是常态而非异常
        // （契约 sysmon §2.4）；Linux 的 /proc/diskstats 在支持宿主上必须可读。
#if defined(__linux__)
        FAIL() << "linux /proc/diskstats and /sys/block must be readable on a supported host";
#else
        GTEST_SKIP() << "disk performance counters unavailable on this host "
                        "(counters disabled or access denied)";
#endif
    }
    // 设备标识非空；同一帧内唯一（设备集合可在帧间变化，但不允许重复）。
#if defined(__linux__)
    EXPECT_FALSE(first.disks.empty())
        << "a supported linux host must expose at least one physical disk";
#endif
    std::vector<std::string> devices;
    for (const auto& disk : first.disks) {
        EXPECT_FALSE(disk.device.empty());
        devices.push_back(disk.device);
        // 回绕跨度随采样携带：Linux 计数/字节为 64 位、时间来自 32 位毫秒计数器；
        // Windows 计数为 32 位 DWORD、时间/字节为 64 位（契约 sysmon §2.7）。
#if defined(__linux__)
        EXPECT_EQ(disk.count_wrap_span, 0ULL) << disk.device;
        EXPECT_EQ(disk.time_wrap_span, DISK_IO_TIME_WRAP_SPAN_US) << disk.device;
        EXPECT_TRUE(disk.busy_time_us.has_value()) << disk.device;
        EXPECT_TRUE(disk.in_flight.has_value()) << disk.device;
#elif defined(_WIN32)
        EXPECT_EQ(disk.count_wrap_span, DISK_IO_COUNT_WRAP_SPAN_32) << disk.device;
        EXPECT_EQ(disk.time_wrap_span, 0ULL) << disk.device;
        EXPECT_TRUE(disk.in_flight.has_value()) << disk.device;
#endif
    }
    std::sort(devices.begin(), devices.end());
    EXPECT_EQ(std::adjacent_find(devices.begin(), devices.end()), devices.end())
        << "disk device identifiers must be unique within one frame";

    // 条件轮询直到任一累计计数器推进（最多约 2s）；不以固定 sleep 作为同步手段
    // （docs/unit-test-standard.md §1.5）。空转机器上可能不推进，仍做单调性检查。
    // 只保留最近一次**成功**采样：超时前最后一次采集失败不应让本用例假红。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    RawDiskIoSnapshot second;
    auto counters_advanced = [&first](const std::vector<RawDiskIoInfo>& current) {
        for (const auto& disk : current) {
            for (const auto& previous : first.disks) {
                if (previous.device == disk.device &&
                    disk.read_count + disk.write_count >
                        previous.read_count + previous.write_count) {
                    return true;
                }
            }
        }
        return false;
    };
    while (std::chrono::steady_clock::now() < deadline) {
        const auto sample = collector->collect_disk_io();
        if (sample.unavailable_reason.empty()) {
            second = sample;
            if (counters_advanced(sample.disks)) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_FALSE(second.disks.empty()) << "collect_disk_io must keep working within one sampling";
    // 累计计数器单调不减（两平台同一不变量；32 位计数器在 2s 内不会回绕）。
    for (const auto& current : second.disks) {
        const auto it = std::find_if(
            first.disks.begin(), first.disks.end(),
            [&current](const auto& previous) { return previous.device == current.device; });
        if (it == first.disks.end()) {
            continue;  // 设备集合可在帧间变化
        }
        EXPECT_GE(current.read_count, it->read_count) << current.device;
        EXPECT_GE(current.write_count, it->write_count) << current.device;
        EXPECT_GE(current.read_bytes, it->read_bytes) << current.device;
        EXPECT_GE(current.write_bytes, it->write_bytes) << current.device;
        EXPECT_GE(current.read_time_us, it->read_time_us) << current.device;
        EXPECT_GE(current.write_time_us, it->write_time_us) << current.device;
        // `busy_time_us` 是**派生值**（Windows `QueryTime - IdleTime`、Linux `io_ticks`），
        // 不是原始累计计数器：Windows 上 QueryTime 与 IdleTime 由驱动分别记账、非原子读取，
        // 二者之差会有亚毫秒级抖动（实测 42µs / 191µs 的回落），不是计数器重置。
        // 因此这里只要求「不显著回落」。容差 10ms 的依据：真实计数器重置会让该字段变成
        // null（`QueryTime < IdleTime` 分支）从而跳过本断言，系统时钟回拨则会让它大幅回落，
        // 两者都远大于 10ms；而 10ms 只占 1s 采样窗口的 1%，不会掩盖真实回归。
        if (it->busy_time_us.has_value() && current.busy_time_us.has_value()) {
            constexpr uint64_t kDerivedBusyJitterToleranceUs = 10'000;
            EXPECT_GE(*current.busy_time_us + kDerivedBusyJitterToleranceUs, *it->busy_time_us)
                << current.device;
        }
    }
}

// ===== #77 真机 oracle：每网卡累计计数器与虚拟网卡标记（契约 sysmon §2.4、§2.7）=====
TEST(SystemCollectorTest, NetworkCountersFollowContractInvariants) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto first = collector->collect_network();
    // 两平台的数据源（Linux `/proc/net/dev`；Windows `GetIfTable2`）都不需要特权，
    // 在受支持的宿主上不可用即为实现缺陷（契约 sysmon §2.4 的 collect_failed 只应
    // 出现在数据源真的读不到时）。
    ASSERT_TRUE(first.unavailable_reason.empty())
        << "collect_network must work on a supported host, reason=" << first.unavailable_reason;

    // 接口名非空且帧内唯一（契约 sysmon §7）；八个累计计数器是 uint64 类型本身非负。
    std::vector<std::string> names;
    for (const auto& interface : first.interfaces) {
        EXPECT_FALSE(interface.interface_name.empty());
        names.push_back(interface.interface_name);
#if defined(__linux__)
        // 回环没有硬件设备支撑（`/sys/class/net/lo/device` 不存在）⇒ 必须判为虚拟
        // （契约 sysmon §2.4：结构性判定，不看名字前缀）。
        if (interface.interface_name == "lo") {
            EXPECT_TRUE(interface.is_virtual) << "loopback must be flagged virtual";
        }
#endif
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end())
        << "interface names must be unique within one frame";

    // 单独断言"存在但为 0 流量"的接口不被丢弃：本机至少有一个接口（含 lo），
    // 且 0 计数器是合法读数而不是"读不到"（契约 sysmon §2.4）。
    EXPECT_FALSE(first.interfaces.empty()) << "a supported host must expose at least one interface";

    // 条件轮询直到任一累计计数器推进（最多约 2s）；不以固定 sleep 作为同步手段
    // （docs/unit-test-standard.md §1.5）。空转机器上可能不推进，此时仍做单调性检查。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    RawNetworkSnapshot second;
    auto counters_advanced = [&first](const std::vector<RawNetworkInfo>& current) {
        for (const auto& interface : current) {
            for (const auto& previous : first.interfaces) {
                if (previous.interface_name == interface.interface_name &&
                    interface.rx_bytes + interface.tx_bytes >
                        previous.rx_bytes + previous.tx_bytes) {
                    return true;
                }
            }
        }
        return false;
    };
    while (std::chrono::steady_clock::now() < deadline) {
        const auto sample = collector->collect_network();
        if (sample.unavailable_reason.empty()) {
            second = sample;
            if (counters_advanced(sample.interfaces)) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_FALSE(second.interfaces.empty())
        << "collect_network must keep working within one sampling";
    // 累计计数器单调不减（两平台均为 64 位，2s 内不会回绕；契约 sysmon §2.7）。
    for (const auto& current : second.interfaces) {
        const auto it = std::find_if(first.interfaces.begin(), first.interfaces.end(),
                                     [&current](const auto& previous) {
                                         return previous.interface_name == current.interface_name;
                                     });
        if (it == first.interfaces.end()) {
            continue;  // 接口集合可在帧间变化（热插拔/虚拟网卡增删）
        }
        EXPECT_GE(current.rx_bytes, it->rx_bytes) << current.interface_name;
        EXPECT_GE(current.tx_bytes, it->tx_bytes) << current.interface_name;
        EXPECT_GE(current.rx_packets, it->rx_packets) << current.interface_name;
        EXPECT_GE(current.tx_packets, it->tx_packets) << current.interface_name;
        EXPECT_GE(current.rx_errors, it->rx_errors) << current.interface_name;
        EXPECT_GE(current.tx_errors, it->tx_errors) << current.interface_name;
        EXPECT_GE(current.rx_drops, it->rx_drops) << current.interface_name;
        EXPECT_GE(current.tx_drops, it->tx_drops) << current.interface_name;
    }
}

#if defined(__linux__)
// ===== #77 Linux 内部解析接缝：/proc/net/dev 与虚拟网卡判定（契约 sysmon §2.4）=====

namespace netdev_test {

using dztrader::platform::system_collector_linux_internal::is_virtual_interface;
using dztrader::platform::system_collector_linux_internal::NetDevEntry;
using dztrader::platform::system_collector_linux_internal::parse_net_dev;

/// 典型 `/proc/net/dev` 内容：两行表头 + 回环 + 物理网卡 + docker0 + 全 0 流量网卡。
constexpr std::string_view kNetDev =
    "Inter-|   Receive                                                |  Transmit\n"
    " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop "
    "fifo colls carrier compressed\n"
    "    lo: 335789071 1811550    0    0    0     0          0         0 335789071 1811550    0    "
    "0    0     0       0          0\n"
    "  eth0: 55800260082 60986183    3 47594    0     0          0   3255659 32359188879 48915567"
    "    7    4    0     0       0          0\n"
    "docker0: 2728364   47425    0    0    0     0          0         0 84885587   52319    0  809"
    "    0     0       0          0\n"
    "  wls3:       0       0    0    0    0     0          0         0        0       0    0    4"
    "    0     0       0          0\n";

TEST(NetdevParserTest, ParsesAllCounterColumnsAndSkipsHeaders) {
    const auto entries = parse_net_dev(kNetDev);
    // 两行表头没有冒号后的 16 个数字，必须被跳过（契约 sysmon §2.4）。
    ASSERT_EQ(entries.size(), 4U);

    const auto& lo = entries[0];
    EXPECT_EQ(lo.interface_name, "lo");
    EXPECT_EQ(lo.rx_bytes, 335789071U);
    EXPECT_EQ(lo.rx_packets, 1811550U);
    EXPECT_EQ(lo.rx_errors, 0U);
    EXPECT_EQ(lo.rx_drops, 0U);
    EXPECT_EQ(lo.tx_bytes, 335789071U);
    EXPECT_EQ(lo.tx_packets, 1811550U);
    EXPECT_EQ(lo.tx_errors, 0U);
    EXPECT_EQ(lo.tx_drops, 0U);

    // 收发是两段独立列：rx 段 8 项之后才是 tx 段（错位会在这里暴露）。
    const auto& eth0 = entries[1];
    EXPECT_EQ(eth0.interface_name, "eth0");
    EXPECT_EQ(eth0.rx_bytes, 55800260082U);
    EXPECT_EQ(eth0.rx_packets, 60986183U);
    EXPECT_EQ(eth0.rx_errors, 3U);
    EXPECT_EQ(eth0.rx_drops, 47594U);
    EXPECT_EQ(eth0.tx_bytes, 32359188879U);
    EXPECT_EQ(eth0.tx_packets, 48915567U);
    EXPECT_EQ(eth0.tx_errors, 7U);
    EXPECT_EQ(eth0.tx_drops, 4U);

    EXPECT_EQ(entries[2].interface_name, "docker0");
    EXPECT_EQ(entries[2].rx_bytes, 2728364U);
    EXPECT_EQ(entries[2].tx_bytes, 84885587U);
    EXPECT_EQ(entries[2].tx_drops, 809U);

    // 全 0 流量网卡：0 是真实读数，不是"读不到"（契约 sysmon §2.4 的采集范围）。
    const auto& wls3 = entries[3];
    EXPECT_EQ(wls3.interface_name, "wls3");
    EXPECT_EQ(wls3.rx_bytes, 0U);
    EXPECT_EQ(wls3.tx_bytes, 0U);
    EXPECT_EQ(wls3.tx_drops, 4U);
}

TEST(NetdevParserTest, HeadersOnlyAndMalformedLinesYieldNoEntries) {
    EXPECT_TRUE(parse_net_dev("").empty());
    // 只有表头：不是整体失败，只是没有接口（空数组 = 本机无接口）。
    EXPECT_TRUE(
        parse_net_dev("Inter-|   Receive |  Transmit\n"
                      " face |bytes packets errs drop fifo frame compressed multicast|bytes "
                      "packets errs drop fifo colls carrier compressed\n")
            .empty());
    // 字段不足/非法数字/缺接口名的行逐行跳过，不影响同一文本里的合法行。
    constexpr std::string_view kMixed =
        "    lo: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n"
        "  bad: 1 2 3\n"
        "  bad2: 1 2 3 4 5 6 7 8 9 10 11 x 13 14 15 16\n"
        ": 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n"
        "  eth0: 10 20 30 40 50 60 70 80 90 100 110 120 130 140 150 160\n";
    const auto entries = parse_net_dev(kMixed);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].interface_name, "lo");
    EXPECT_EQ(entries[0].rx_bytes, 1U);
    EXPECT_EQ(entries[0].tx_bytes, 9U);
    EXPECT_EQ(entries[1].interface_name, "eth0");
    EXPECT_EQ(entries[1].rx_bytes, 10U);
    EXPECT_EQ(entries[1].tx_drops, 120U);
}

TEST(NetdevParserTest, InterfaceNameWithColonUsesLastColonAsSeparator) {
    // 老式别名接口名可含冒号：16 个统计字段全是数字，最后一个冒号必是分隔符。
    constexpr std::string_view kAlias = "  eth0:1: 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22\n";
    const auto entries = parse_net_dev(kAlias);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].interface_name, "eth0:1");
    EXPECT_EQ(entries[0].rx_bytes, 7U);
    EXPECT_EQ(entries[0].tx_drops, 18U);
}

TEST(NetdevParserTest, DuplicateInterfaceLinesKeepFirst) {
    constexpr std::string_view kDuplicate =
        "  eth0: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n"
        "  eth0: 100 200 300 400 500 600 700 800 900 1000 1100 1200 1300 1400 1500 1600\n";
    const auto entries = parse_net_dev(kDuplicate);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].rx_bytes, 1U);
}

/// 假 sysfs 根 fixture：用真实目录 + 符号链接伪造 `/sys/class/net` 的两级链接结构，
/// 使虚拟网卡判定不依赖真实网卡名（AGENTS 硬门禁 2 自管临时目录的豁免情形）。
class FakeSysfsTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::error_code ec;
        base_ = std::filesystem::temp_directory_path(ec) /
                ("dzsysmon_net_" + std::to_string(current_process_id()) + "_" +
                 std::to_string(counter_++));
        std::filesystem::remove_all(base_, ec);
        ASSERT_TRUE(std::filesystem::create_directories(base_, ec)) << ec.message();
        // 真实 sysfs 布局：`/sys/class/net/<if>` 是符号链接，指向
        // `/sys/devices/<bus path>/net/<if>`，该目录下的 `device` 也是符号链接。
        net_root_ = base_ / "sys" / "class" / "net";
        ASSERT_TRUE(std::filesystem::create_directories(net_root_, ec)) << ec.message();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }

    /// 造一个"有硬件设备支撑"的接口：`<if>` 链接指向 devpath，devpath/device 链接存在。
    void add_physical_interface(const std::string& name) {
        std::error_code ec;
        const auto devpath =
            base_ / "sys" / "devices" / "pci0000:00" / "0000:00:1f.6" / "net" / name;
        ASSERT_TRUE(std::filesystem::create_directories(devpath, ec)) << ec.message();
        // device 链接指向父级 PCI 设备目录（真实 sysfs 里就指向设备本身）。
        std::filesystem::create_directory_symlink(
            base_ / "sys" / "devices" / "pci0000:00" / "0000:00:1f.6", devpath / "device", ec);
        ASSERT_FALSE(ec) << ec.message();
        std::filesystem::create_symlink(std::filesystem::path("..") / ".." / "devices" /
                                            "pci0000:00" / "0000:00:1f.6" / "net" / name,
                                        net_root_ / name, ec);
        ASSERT_FALSE(ec) << ec.message();
    }

    /// 造一个"无硬件设备支撑"的接口：`<if>` 链接指向 devices/virtual/net/<if>，无 device。
    void add_virtual_interface(const std::string& name) {
        std::error_code ec;
        const auto devpath = base_ / "sys" / "devices" / "virtual" / "net" / name;
        ASSERT_TRUE(std::filesystem::create_directories(devpath, ec)) << ec.message();
        std::filesystem::create_symlink(
            std::filesystem::path("..") / ".." / "devices" / "virtual" / "net" / name,
            net_root_ / name, ec);
        ASSERT_FALSE(ec) << ec.message();
    }

    std::filesystem::path base_;
    std::filesystem::path net_root_;
    static int counter_;
};

int FakeSysfsTest::counter_ = 0;

TEST_F(FakeSysfsTest, DeviceBackedInterfaceIsPhysicalAndOthersAreVirtual) {
    // 名字刻意选成与真实网卡名无关（判定必须是结构性的，契约 sysmon §2.4 禁止名字前缀）。
    add_physical_interface("hwport0");
    add_virtual_interface("logicalport0");
    add_virtual_interface("lo");

    // 跟随两级符号链接后能在 <if>/device 处 stat 到实体 ⇒ 物理。
    EXPECT_FALSE(is_virtual_interface("hwport0", net_root_.string()));
    // 链接链存在但没有 device ⇒ 虚拟（回环/桥接/veth/隧道都属此类）。
    EXPECT_TRUE(is_virtual_interface("logicalport0", net_root_.string()));
    EXPECT_TRUE(is_virtual_interface("lo", net_root_.string()));
    // 接口已消失（热插拔）⇒ 按虚拟处理，不冒充硬件设备。
    EXPECT_TRUE(is_virtual_interface("gone0", net_root_.string()));
    // 无名接口无法判定 ⇒ 按虚拟处理。
    EXPECT_TRUE(is_virtual_interface("", net_root_.string()));
}

TEST_F(FakeSysfsTest, InterfaceEntryItselfMayBeAVirtualSymlink) {
    // 接口目录**不是**符号链接（例如测试里直接建同名目录）时同样按 device 是否存在判定：
    // 判据只取 `<if>/device` 的可达性，不依赖 `<if>` 本身的链接形态。
    std::error_code ec;
    const auto dir = net_root_ / "plaindir0";
    ASSERT_TRUE(std::filesystem::create_directories(dir, ec)) << ec.message();
    EXPECT_TRUE(is_virtual_interface("plaindir0", net_root_.string()));
    ASSERT_TRUE(std::filesystem::create_directories(dir / "device", ec)) << ec.message();
    EXPECT_FALSE(is_virtual_interface("plaindir0", net_root_.string()));
}

TEST_F(FakeSysfsTest, MissingSysfsRootTreatsEveryInterfaceAsVirtual) {
    // sysfs 根不存在（或接口不在其中）：找不到 `<if>/device` ⇒ 判虚拟。
    // 宁可多隐藏一个未知接口，也不把虚拟网卡当硬件设备输出（契约 sysmon §2.4）。
    EXPECT_TRUE(is_virtual_interface("hwport0", (base_ / "no_such_root").string()));
}

}  // namespace netdev_test
#endif  // defined(__linux__)

}  // namespace dztrader::test
