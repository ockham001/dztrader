#include "timer.h"

#include <gtest/gtest.h>

#include <chrono>

namespace dztrader::sysmon {
namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr milliseconds kInterval{1000};

/// 固定基准时刻，测试不读真实时钟。
const Clock::time_point kLast = Clock::time_point{} + milliseconds{10000};

TEST(TimerTest, OnTimeKeepsCadence) {
    const auto tick = advance_deadline(kLast, kInterval, kLast);
    EXPECT_EQ(tick.next, kLast + kInterval);
    EXPECT_EQ(tick.missed, 0U);
}

TEST(TimerTest, SlightDelayBelowOneIntervalKeepsCadence) {
    // 回调比 deadline 晚 500ms：仍按 last+interval 排下一个 deadline（不累积漂移）
    const auto tick = advance_deadline(kLast, kInterval, kLast + milliseconds{500});
    EXPECT_EQ(tick.next, kLast + kInterval);
    EXPECT_EQ(tick.missed, 0U);
}

TEST(TimerTest, ExactlyAtNextDeadlineIsNotMissed) {
    // now 恰等于下一个 deadline：该 tick 仍可立即执行，不算错过
    const auto tick = advance_deadline(kLast, kInterval, kLast + kInterval);
    EXPECT_EQ(tick.next, kLast + kInterval);
    EXPECT_EQ(tick.missed, 0U);
}

TEST(TimerTest, LateOneTickSkipsAndCountsOne) {
    // 晚 1.5 个间隔：跳过 1 个整 tick，next 为 last+2*interval
    const auto tick = advance_deadline(kLast, kInterval, kLast + milliseconds{1500});
    EXPECT_EQ(tick.next, kLast + 2 * kInterval);
    EXPECT_EQ(tick.missed, 1U);
}

TEST(TimerTest, LateMultipleTicksSkipsAll) {
    // 晚 3.5 个间隔：跳过 3 个整 tick，next 为 last+4*interval
    const auto tick = advance_deadline(kLast, kInterval, kLast + milliseconds{3500});
    EXPECT_EQ(tick.next, kLast + 4 * kInterval);
    EXPECT_EQ(tick.missed, 3U);
}

TEST(TimerTest, NonPositiveIntervalDoesNotAdvance) {
    // 防御分支：非法间隔不推进（避免死循环），由配置校验保证不会走到这里
    const auto tick = advance_deadline(kLast, milliseconds{0}, kLast + milliseconds{5000});
    EXPECT_EQ(tick.next, kLast);
    EXPECT_EQ(tick.missed, 0U);
}

}  // namespace
}  // namespace dztrader::sysmon
