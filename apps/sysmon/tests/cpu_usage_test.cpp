#include "cpu_usage.h"

#include <dztrader/platform/system_collector.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

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

}  // namespace
}  // namespace dztrader::sysmon
