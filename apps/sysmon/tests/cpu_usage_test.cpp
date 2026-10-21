#include "cpu_usage.h"

#include <dztrader/platform/system_collector.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace dztrader::sysmon {
namespace {

using platform::RawCpuTimes;

RawCpuTimes make_times(uint64_t total_ticks, uint64_t idle_ticks) {
    RawCpuTimes times;
    times.total_ticks = total_ticks;
    times.idle_ticks = idle_ticks;
    return times;
}

TEST(CpuUsageTest, FirstSampleReturnsNullopt) {
    // prev 全 0 = 首次采样：不得用 0 冒充使用率
    EXPECT_EQ(compute_cpu_usage(make_times(0, 0), make_times(1000, 400)), std::nullopt);
}

TEST(CpuUsageTest, NormalIncrementComputesBusyRatio) {
    // Δtotal=1000, Δidle=400 → 1 - 0.4 = 0.6
    const auto usage = compute_cpu_usage(make_times(1000, 400), make_times(2000, 800));
    ASSERT_TRUE(usage.has_value());
    EXPECT_DOUBLE_EQ(usage.value(), 0.6);
}

TEST(CpuUsageTest, AllIdleIsZero) {
    const auto usage = compute_cpu_usage(make_times(1000, 1000), make_times(3000, 3000));
    ASSERT_TRUE(usage.has_value());
    EXPECT_DOUBLE_EQ(usage.value(), 0.0);
}

TEST(CpuUsageTest, AllBusyIsOne) {
    const auto usage = compute_cpu_usage(make_times(1000, 500), make_times(2000, 500));
    ASSERT_TRUE(usage.has_value());
    EXPECT_DOUBLE_EQ(usage.value(), 1.0);
}

TEST(CpuUsageTest, ClampsIdleExceedingTotal) {
    // 异常计数器（Δidle > Δtotal）钳制到 0，不返回负数
    const auto usage = compute_cpu_usage(make_times(100, 0), make_times(200, 200));
    ASSERT_TRUE(usage.has_value());
    EXPECT_DOUBLE_EQ(usage.value(), 0.0);
}

TEST(CpuUsageTest, WrapsOn32BitCounters) {
    constexpr uint64_t kMax32 = std::numeric_limits<uint32_t>::max();
    // 2^32 回绕：prev.total=2^32-10 → 5，Δtotal=15；prev.idle=2^32-3 → 3，Δidle=6
    const auto usage = compute_cpu_usage(make_times(kMax32 - 9, kMax32 - 2), make_times(5, 3));
    ASSERT_TRUE(usage.has_value());
    EXPECT_DOUBLE_EQ(usage.value(), 0.6);
}

TEST(CpuUsageTest, WrapRequiresBothValuesIn32BitRange) {
    // 64 位计数器下降 → 视为重置，而不是按 2^32 回绕
    const auto usage =
        compute_cpu_usage(make_times(10'000'000'000ULL, 5'000'000'000ULL), make_times(100, 50));
    EXPECT_EQ(usage, std::nullopt);
}

TEST(CpuUsageTest, IdleResetReturnsNullopt) {
    // total 正常递增但 idle 在 64 位范围下降 → 计数器重置
    const auto usage = compute_cpu_usage(make_times(10'000'000'000ULL, 5'000'000'000ULL),
                                         make_times(10'000'001'000ULL, 4'999'999'000ULL));
    EXPECT_EQ(usage, std::nullopt);
}

TEST(CpuUsageTest, ZeroDeltaReturnsNullopt) {
    // Δtotal=0（计数器未推进）不可计算
    EXPECT_EQ(compute_cpu_usage(make_times(100, 40), make_times(100, 40)), std::nullopt);
}

platform::RawCpuCoreTimes make_core(uint32_t index, uint64_t total_ticks, uint64_t idle_ticks) {
    platform::RawCpuCoreTimes core;
    core.index = index;
    core.total_ticks = total_ticks;
    core.idle_ticks = idle_ticks;
    return core;
}

TEST(CpuUsageTest, CoreUsagesComputePerCoreBusyRatio) {
    const std::vector<platform::RawCpuCoreTimes> prev = {make_core(0, 1000, 400),
                                                         make_core(1, 1000, 1000)};
    const std::vector<platform::RawCpuCoreTimes> cur = {make_core(0, 2000, 800),
                                                        make_core(1, 3000, 3000)};
    const auto usages = compute_core_usages(prev, cur);
    ASSERT_EQ(usages.size(), 2U);
    ASSERT_TRUE(usages[0].has_value());
    EXPECT_DOUBLE_EQ(usages[0].value(), 0.6);
    ASSERT_TRUE(usages[1].has_value());
    EXPECT_DOUBLE_EQ(usages[1].value(), 0.0);
}

TEST(CpuUsageTest, CoreCountChangeMakesAllUsagesUnavailable) {
    const std::vector<platform::RawCpuCoreTimes> prev = {make_core(0, 1000, 400)};
    const std::vector<platform::RawCpuCoreTimes> cur = {make_core(0, 2000, 800),
                                                        make_core(1, 1000, 500)};
    const auto usages = compute_core_usages(prev, cur);
    ASSERT_EQ(usages.size(), 2U);
    EXPECT_FALSE(usages[0].has_value());
    EXPECT_FALSE(usages[1].has_value());
}

TEST(CpuUsageTest, CoreIndexChangeMakesAllUsagesUnavailable) {
    // 热插拔后同一位置换了 CPU 编号：不得把不同核的计数器做差。
    const std::vector<platform::RawCpuCoreTimes> prev = {make_core(0, 1000, 400),
                                                         make_core(1, 1000, 500)};
    const std::vector<platform::RawCpuCoreTimes> cur = {make_core(1, 2000, 800),
                                                        make_core(2, 2000, 1000)};
    const auto usages = compute_core_usages(prev, cur);
    ASSERT_EQ(usages.size(), 2U);
    EXPECT_FALSE(usages[0].has_value());
    EXPECT_FALSE(usages[1].has_value());
}

TEST(CpuUsageTest, CoreCounterResetOnlyAffectsThatCore) {
    const std::vector<platform::RawCpuCoreTimes> prev = {make_core(0, 10'000'000'000ULL, 0),
                                                         make_core(1, 1000, 400)};
    const std::vector<platform::RawCpuCoreTimes> cur = {make_core(0, 100, 0),
                                                        make_core(1, 2000, 800)};
    const auto usages = compute_core_usages(prev, cur);
    ASSERT_EQ(usages.size(), 2U);
    EXPECT_FALSE(usages[0].has_value());
    ASSERT_TRUE(usages[1].has_value());
    EXPECT_DOUBLE_EQ(usages[1].value(), 0.6);
}

TEST(CpuUsageTest, RateDividesByElapsedSeconds) {
    // Δcount = 1000，实际经过 2 秒 → 500/s
    const auto rate = compute_rate(1000, 2000, 2.0);
    ASSERT_TRUE(rate.has_value());
    EXPECT_DOUBLE_EQ(rate.value(), 500.0);
}

TEST(CpuUsageTest, RateRequiresPositiveElapsed) {
    EXPECT_EQ(compute_rate(1000, 2000, 0.0), std::nullopt);
    EXPECT_EQ(compute_rate(1000, 2000, -1.0), std::nullopt);
}

TEST(CpuUsageTest, RateWrapsOn32BitCounters) {
    constexpr uint64_t kMax32 = std::numeric_limits<uint32_t>::max();
    // 2^32 回绕：prev=2^32-10 → 5，Δ=15；0.5 秒 → 30/s
    const auto rate = compute_rate(kMax32 - 9, 5, 0.5);
    ASSERT_TRUE(rate.has_value());
    EXPECT_DOUBLE_EQ(rate.value(), 30.0);
}

TEST(CpuUsageTest, RateResetReturnsNullopt) {
    EXPECT_EQ(compute_rate(10'000'000'000ULL, 100, 1.0), std::nullopt);
}

}  // namespace
}  // namespace dztrader::sysmon
