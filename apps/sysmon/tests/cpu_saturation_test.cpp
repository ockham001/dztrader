#include "cpu_saturation.h"

#include <dztrader/platform/sysmon.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace dztrader::sysmon {
namespace {

platform::SysmonCpuCore make_core(uint32_t index, std::optional<double> usage_ratio) {
    platform::SysmonCpuCore core;
    core.index = index;
    core.usage_ratio = usage_ratio;
    core.saturated = false;
    return core;
}

constexpr double kThreshold = 0.9;
constexpr uint32_t kRequired = 3;

TEST(CpuSaturationTest, MarksAfterRequiredConsecutiveSamples) {
    CoreSaturationTracker tracker;
    std::vector<platform::SysmonCpuCore> cores = {make_core(0, 0.95), make_core(1, 0.5)};

    tracker.update(cores, kThreshold, kRequired);
    EXPECT_FALSE(cores[0].saturated);
    EXPECT_TRUE(tracker.saturated_cores().empty());

    tracker.update(cores, kThreshold, kRequired);
    EXPECT_FALSE(cores[0].saturated);

    tracker.update(cores, kThreshold, kRequired);
    EXPECT_TRUE(cores[0].saturated);
    EXPECT_FALSE(cores[1].saturated);
    ASSERT_EQ(tracker.saturated_cores().size(), 1U);
    EXPECT_EQ(tracker.saturated_cores()[0], 0U);
}

TEST(CpuSaturationTest, ClearsImmediatelyWhenUsageDropsBelowThreshold) {
    CoreSaturationTracker tracker;
    std::vector<platform::SysmonCpuCore> cores = {make_core(0, 0.95)};
    for (uint32_t i = 0; i < kRequired; ++i) {
        tracker.update(cores, kThreshold, kRequired);
    }
    ASSERT_TRUE(cores[0].saturated);

    cores[0].usage_ratio = 0.5;
    tracker.update(cores, kThreshold, kRequired);
    EXPECT_FALSE(cores[0].saturated);
    EXPECT_TRUE(tracker.saturated_cores().empty());

    // 解除后必须重新累计满 kRequired 次才会再次标记。
    cores[0].usage_ratio = 0.95;
    tracker.update(cores, kThreshold, kRequired);
    EXPECT_FALSE(cores[0].saturated);
    tracker.update(cores, kThreshold, kRequired);
    tracker.update(cores, kThreshold, kRequired);
    EXPECT_TRUE(cores[0].saturated);
}

TEST(CpuSaturationTest, NullSampleDoesNotResetProgress) {
    CoreSaturationTracker tracker;
    std::vector<platform::SysmonCpuCore> cores = {make_core(0, 0.95)};
    tracker.update(cores, kThreshold, kRequired);
    tracker.update(cores, kThreshold, kRequired);

    cores[0].usage_ratio = std::nullopt;  // 不可用样本：无法观察，不改变连续计数
    tracker.update(cores, kThreshold, kRequired);
    EXPECT_FALSE(cores[0].saturated);

    cores[0].usage_ratio = 0.95;
    tracker.update(cores, kThreshold, kRequired);
    EXPECT_TRUE(cores[0].saturated);
}

TEST(CpuSaturationTest, ThresholdIsInclusive) {
    CoreSaturationTracker tracker;
    std::vector<platform::SysmonCpuCore> cores = {make_core(0, 0.9)};
    tracker.update(cores, kThreshold, 1);
    EXPECT_TRUE(cores[0].saturated);
}

TEST(CpuSaturationTest, CoreSequenceChangeResetsProgress) {
    CoreSaturationTracker tracker;
    std::vector<platform::SysmonCpuCore> cores = {make_core(0, 0.95), make_core(1, 0.95)};
    tracker.update(cores, kThreshold, kRequired);
    tracker.update(cores, kThreshold, kRequired);

    // 热插拔：核数变化 → 全部计数清零，不得沿用旧核的连续次数。
    cores.push_back(make_core(2, 0.95));
    tracker.update(cores, kThreshold, kRequired);
    EXPECT_FALSE(cores[0].saturated);
    EXPECT_FALSE(cores[1].saturated);
    EXPECT_FALSE(cores[2].saturated);
}

TEST(CpuSaturationTest, CoreIndexChangeResetsProgress) {
    CoreSaturationTracker tracker;
    std::vector<platform::SysmonCpuCore> cores = {make_core(0, 0.95)};
    tracker.update(cores, kThreshold, kRequired);
    tracker.update(cores, kThreshold, kRequired);

    cores[0].index = 3;  // 同一位置换了 CPU 编号
    tracker.update(cores, kThreshold, kRequired);
    EXPECT_FALSE(cores[0].saturated);
}

TEST(CpuSaturationTest, ReportsSaturatedCoresInSequenceOrder) {
    CoreSaturationTracker tracker;
    std::vector<platform::SysmonCpuCore> cores = {make_core(0, 0.95), make_core(2, 0.5),
                                                  make_core(4, 0.95)};
    tracker.update(cores, kThreshold, 1);
    ASSERT_EQ(tracker.saturated_cores().size(), 2U);
    EXPECT_EQ(tracker.saturated_cores()[0], 0U);
    EXPECT_EQ(tracker.saturated_cores()[1], 4U);
}

TEST(CpuSaturationTest, ZeroRequiredSamplesTreatedAsOne) {
    CoreSaturationTracker tracker;
    std::vector<platform::SysmonCpuCore> cores = {make_core(0, 0.95)};
    tracker.update(cores, kThreshold, 0);
    EXPECT_TRUE(cores[0].saturated);
}

TEST(CpuSaturationTest, ResetClearsState) {
    CoreSaturationTracker tracker;
    std::vector<platform::SysmonCpuCore> cores = {make_core(0, 0.95)};
    tracker.update(cores, kThreshold, 1);
    ASSERT_TRUE(cores[0].saturated);

    tracker.reset();
    cores[0].saturated = false;
    tracker.update(cores, kThreshold, kRequired);
    EXPECT_FALSE(cores[0].saturated);
    EXPECT_TRUE(tracker.saturated_cores().empty());
}

}  // namespace
}  // namespace dztrader::sysmon
