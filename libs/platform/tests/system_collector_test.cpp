// 06-sysmon 采集层真机 oracle 测试：断言两平台实现从真实 OS 系统调用拿到的原始数据满足契约不变量。
#include <dztrader/platform/system_collector.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace dztrader::test {

using namespace dztrader::platform;

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

}  // namespace dztrader::test
