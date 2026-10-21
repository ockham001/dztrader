// 真机 oracle 测试（两平台）：SystemCollector 接缝 + 纯函数在真实机器上的行为不变量
// （契约 sysmon §2.7；#70 AC「两平台真机 oracle」）。
//
// 与 libs/platform/tests/system_collector_test.cpp 的分工：那边断言采集层原始计数器；
// 这里从 dzsysmon 的消费面断言「百分比落在合法区间」与「首次采样不可用而非 0」。

#include "cpu_usage.h"

#include <dztrader/platform/system_collector.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <optional>
#include <thread>

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
    const auto context_switches_rate =
        compute_rate(*first->context_switches, *second->context_switches, elapsed_seconds);
    const auto interrupts_rate =
        compute_rate(*first->interrupts, *second->interrupts, elapsed_seconds);
    ASSERT_TRUE(context_switches_rate.has_value());
    ASSERT_TRUE(interrupts_rate.has_value());
    EXPECT_TRUE(std::isfinite(*context_switches_rate));
    EXPECT_TRUE(std::isfinite(*interrupts_rate));
}

}  // namespace
}  // namespace dztrader::sysmon
