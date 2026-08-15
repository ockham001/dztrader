#include <dztrader/date_time/time.h>

#include <chrono>
#include <sstream>
#include <gtest/gtest.h>

using namespace dztrader;

class TimeTest : public ::testing::Test {
protected:
    Time midnight_;
    Time noon_{12, 0, 0};
    Time market_open_{9, 30, 0};
    Time night_session_{21, 0, 0};
};

TEST_F(TimeTest, DefaultConstructorIsMidnight) {
    Time t;
    EXPECT_EQ(t.hour(), 0);
    EXPECT_EQ(t.minute(), 0);
    EXPECT_EQ(t.second(), 0);
    EXPECT_EQ(t.millisec(), 0);
    EXPECT_EQ(t.millisecs_since_midnight(), 0);
}

TEST_F(TimeTest, ConstructorFromMillisecsSinceMidnight) {
    Time t{0};
    EXPECT_EQ(t, midnight_);
    Time t2{86399999};
    EXPECT_EQ(t2.hour(), 23);
    EXPECT_EQ(t2.minute(), 59);
    EXPECT_EQ(t2.second(), 59);
    EXPECT_EQ(t2.millisec(), 999);
}

TEST_F(TimeTest, ConstructorFromHMS) {
    Time t{9, 30, 15, 500};
    EXPECT_EQ(t.hour(), 9);
    EXPECT_EQ(t.minute(), 30);
    EXPECT_EQ(t.second(), 15);
    EXPECT_EQ(t.millisec(), 500);
}

TEST_F(TimeTest, ConstructorFromHMSDefaultSecMs) {
    Time t{9, 30};
    EXPECT_EQ(t.hour(), 9);
    EXPECT_EQ(t.minute(), 30);
    EXPECT_EQ(t.second(), 0);
    EXPECT_EQ(t.millisec(), 0);
}

TEST_F(TimeTest, ConstructorFromChronoHMS) {
    Time t{std::chrono::hours{9}, std::chrono::minutes{30},
           std::chrono::seconds{15}, std::chrono::milliseconds{500}};
    EXPECT_EQ(t.hour(), 9);
    EXPECT_EQ(t.minute(), 30);
    EXPECT_EQ(t.second(), 15);
    EXPECT_EQ(t.millisec(), 500);
}

TEST_F(TimeTest, ConstructorFromHhMmSs) {
    auto dur = std::chrono::hours{9} + std::chrono::minutes{30} +
               std::chrono::seconds{15} + std::chrono::milliseconds{500};
    auto hms = std::chrono::hh_mm_ss{dur};
    Time t{hms};
    EXPECT_EQ(t.hour(), 9);
    EXPECT_EQ(t.minute(), 30);
    EXPECT_EQ(t.second(), 15);
    EXPECT_EQ(t.millisec(), 500);
}

TEST_F(TimeTest, Accessors) {
    Time t{14, 35, 45, 123};
    EXPECT_EQ(t.hour(), 14);
    EXPECT_EQ(t.minute(), 35);
    EXPECT_EQ(t.second(), 45);
    EXPECT_EQ(t.millisec(), 123);
}

TEST_F(TimeTest, Subsecond) {
    Time t{0, 0, 0, 500};
    EXPECT_EQ(t.subsecond(), std::chrono::milliseconds{500});
}

TEST_F(TimeTest, TimeSinceMidnight) {
    Time t{1, 30, 0, 0};
    EXPECT_EQ(t.time_since_midnight<std::chrono::milliseconds>().count(), 5400000);
    EXPECT_EQ(t.time_since_midnight<std::chrono::seconds>().count(), 5400);
    EXPECT_EQ(t.time_since_midnight<std::chrono::minutes>().count(), 90);
    EXPECT_EQ(t.time_since_midnight<std::chrono::hours>().count(), 1);
}

TEST_F(TimeTest, SecondsMinutesHoursSinceMidnight) {
    Time t{9, 30, 15, 500};
    EXPECT_EQ(t.seconds_since_midnight(), 34215);
    EXPECT_EQ(t.minutes_since_midnight(), 570);
    EXPECT_EQ(t.hours_since_midnight(), 9);
}

TEST_F(TimeTest, AddHoursPositive) {
    auto t = market_open_.add_hours(1);
    EXPECT_EQ(t.hour(), 10);
    EXPECT_EQ(t.minute(), 30);
}

TEST_F(TimeTest, AddHoursNegative) {
    auto t = market_open_.add_hours(-1);
    EXPECT_EQ(t.hour(), 8);
    EXPECT_EQ(t.minute(), 30);
}

TEST_F(TimeTest, AddMinutes) {
    auto t = market_open_.add_minutes(30);
    EXPECT_EQ(t.hour(), 10);
    EXPECT_EQ(t.minute(), 0);
}

TEST_F(TimeTest, AddSeconds) {
    auto t = Time{9, 30, 0}.add_seconds(30);
    EXPECT_EQ(t.second(), 30);
}

TEST_F(TimeTest, AddMillisecs) {
    auto t = Time{9, 30, 0, 0}.add_millisecs(500);
    EXPECT_EQ(t.millisec(), 500);
}

TEST_F(TimeTest, WrapAroundAdd) {
    Time t{23, 0, 0};
    auto r = t.add_hours(2);
    EXPECT_EQ(r.hour(), 1);
    EXPECT_EQ(r.minute(), 0);
}

TEST_F(TimeTest, WrapAroundSubtract) {
    Time t{0, 0, 0, 0};
    auto r = t.add_millisecs(-1);
    EXPECT_EQ(r.hour(), 23);
    EXPECT_EQ(r.minute(), 59);
    EXPECT_EQ(r.second(), 59);
    EXPECT_EQ(r.millisec(), 999);
}

TEST_F(TimeTest, HoursToPositive) {
    Time a{9, 0, 0};
    Time b{11, 0, 0};
    EXPECT_EQ(a.hours_to(b), 2);
}

TEST_F(TimeTest, HoursToNegative) {
    Time a{11, 0, 0};
    Time b{9, 0, 0};
    EXPECT_EQ(a.hours_to(b), -2);
}

TEST_F(TimeTest, MinutesTo) {
    Time a{9, 0, 0};
    Time b{9, 30, 0};
    EXPECT_EQ(a.minutes_to(b), 30);
}

TEST_F(TimeTest, SecondsTo) {
    Time a{9, 30, 0};
    Time b{9, 30, 30};
    EXPECT_EQ(a.seconds_to(b), 30);
}

TEST_F(TimeTest, MillisecsTo) {
    Time a{0, 0, 0, 0};
    Time b{0, 0, 0, 500};
    EXPECT_EQ(a.millisecs_to(b), 500);
}

TEST_F(TimeTest, MillisecsToZero) {
    EXPECT_EQ(midnight_.millisecs_to(midnight_), 0);
}

TEST_F(TimeTest, OperatorPlusDuration) {
    Time t{10, 0, 0};
    auto r = t + std::chrono::hours{2};
    EXPECT_EQ(r.hour(), 12);
}

TEST_F(TimeTest, OperatorMinusDuration) {
    Time t{10, 0, 0};
    auto r = t - std::chrono::hours{2};
    EXPECT_EQ(r.hour(), 8);
}

TEST_F(TimeTest, OperatorPlusAssign) {
    Time t{10, 0, 0};
    t += std::chrono::hours{2};
    EXPECT_EQ(t.hour(), 12);
}

TEST_F(TimeTest, OperatorMinusAssign) {
    Time t{10, 0, 0};
    t -= std::chrono::hours{2};
    EXPECT_EQ(t.hour(), 8);
}

TEST_F(TimeTest, OperatorTimeDifference) {
    Time a{9, 0, 0};
    Time b{10, 30, 0};
    auto diff = b - a;
    EXPECT_EQ(diff.count(), 5400000);
}

TEST_F(TimeTest, ComparisonOperators) {
    Time a{9, 0, 0};
    Time b{10, 0, 0};
    EXPECT_EQ(a, a);
    EXPECT_NE(a, b);
    EXPECT_LT(a, b);
    EXPECT_LE(a, b);
    EXPECT_LE(a, a);
    EXPECT_GT(b, a);
    EXPECT_GE(b, a);
    EXPECT_GE(a, a);
}

TEST_F(TimeTest, FromHhMmSsValid) {
    auto t = Time::from_hh_mm_ss(9, 30, 15, 500);
    EXPECT_EQ(t.hour(), 9);
    EXPECT_EQ(t.minute(), 30);
    EXPECT_EQ(t.second(), 15);
    EXPECT_EQ(t.millisec(), 500);
}

TEST_F(TimeTest, FromHhMmSsInvalidHour) {
    EXPECT_THROW(Time::from_hh_mm_ss(-1, 0, 0), std::invalid_argument);
    EXPECT_THROW(Time::from_hh_mm_ss(24, 0, 0), std::invalid_argument);
}

TEST_F(TimeTest, FromHhMmSsInvalidMinute) {
    EXPECT_THROW(Time::from_hh_mm_ss(0, -1, 0), std::invalid_argument);
    EXPECT_THROW(Time::from_hh_mm_ss(0, 60, 0), std::invalid_argument);
}

TEST_F(TimeTest, FromHhMmSsInvalidSecond) {
    EXPECT_THROW(Time::from_hh_mm_ss(0, 0, -1), std::invalid_argument);
    EXPECT_THROW(Time::from_hh_mm_ss(0, 0, 60), std::invalid_argument);
}

TEST_F(TimeTest, FromHhMmSsInvalidMillisec) {
    EXPECT_THROW(Time::from_hh_mm_ss(0, 0, 0, -1), std::invalid_argument);
    EXPECT_THROW(Time::from_hh_mm_ss(0, 0, 0, 1000), std::invalid_argument);
}

TEST_F(TimeTest, FromHhMmSsChronoInvalid) {
    EXPECT_THROW(Time::from_hh_mm_ss(std::chrono::hours{24}, std::chrono::minutes{0}),
                 std::invalid_argument);
}

TEST_F(TimeTest, FromHhMmSsHmsInvalid) {
    auto dur = std::chrono::hours{25};
    auto hms = std::chrono::hh_mm_ss{dur};
    EXPECT_THROW(Time::from_hh_mm_ss(hms), std::invalid_argument);
}

TEST_F(TimeTest, FromMillisecsSinceMidnightValid) {
    auto t = Time::from_millisecs_since_midnight(0);
    EXPECT_EQ(t, midnight_);
    auto t2 = Time::from_millisecs_since_midnight(86399999);
    EXPECT_EQ(t2.hour(), 23);
}

TEST_F(TimeTest, FromMillisecsSinceMidnightInvalid) {
    EXPECT_THROW(Time::from_millisecs_since_midnight(-1), std::invalid_argument);
    EXPECT_THROW(Time::from_millisecs_since_midnight(86400000), std::invalid_argument);
}

TEST_F(TimeTest, ToStringDefaultFormat) {
    Time t{9, 30, 15, 500};
    EXPECT_EQ(t.to_string(), "09:30:15.500");
}

TEST_F(TimeTest, ToStringCustomFormat) {
    Time t{9, 30, 15};
    EXPECT_EQ(t.to_string("%R"), "09:30");
}

TEST_F(TimeTest, ToStringFractionZero) {
    Time t{9, 30, 15, 500};
    EXPECT_EQ(t.to_string("%S", 0), "15");
}

TEST_F(TimeTest, ToStringFractionPositive) {
    Time t{9, 30, 15, 123};
    EXPECT_EQ(t.to_string("%S", 3), "15.123");
}

TEST_F(TimeTest, ToStringFractionAutoNoSubsecond) {
    Time t{9, 30, 15, 0};
    EXPECT_EQ(t.to_string("%T", -1), "09:30:15");
}

TEST_F(TimeTest, ToStringStringTypeTemplate) {
    Time t{9, 30, 15};
    auto s = t.to_string<std::string>();
    EXPECT_EQ(s, "09:30:15");
}

TEST_F(TimeTest, FromStringNormal) {
    auto t = Time::from_string("09:30:15");
    EXPECT_EQ(t.hour(), 9);
    EXPECT_EQ(t.minute(), 30);
    EXPECT_EQ(t.second(), 15);
}

TEST_F(TimeTest, FromStringWithSubsecond) {
    auto t = Time::from_string("09:30:15.500");
    EXPECT_EQ(t.millisec(), 500);
}

TEST_F(TimeTest, FromStringMissingComponentsDefault) {
    auto t = Time::from_string("09:30", "%H:%M");
    EXPECT_EQ(t.hour(), 9);
    EXPECT_EQ(t.minute(), 30);
    EXPECT_EQ(t.second(), 0);
    EXPECT_EQ(t.millisec(), 0);
}

TEST_F(TimeTest, FromStringNoTimeComponents) {
    EXPECT_THROW(Time::from_string("2024", "%Y"), std::invalid_argument);
}

TEST_F(TimeTest, FromStringInvalidTime) {
    EXPECT_THROW(Time::from_string("25:00:00"), std::out_of_range);
}

TEST_F(TimeTest, OperatorStream) {
    Time t{9, 30, 15};
    std::ostringstream oss;
    oss << t;
    EXPECT_EQ(oss.str(), "09:30:15");
}

TEST_F(TimeTest, MillisecondTraversal) {
    // 全交易日遍历（86400 次迭代）: 校验 hour/minute/second 与 ms 的数学关系。
    // 初始提交时被禁用（DISABLED_ 前缀, 无失败记录）; 实测 86400 次迭代毫秒级完成,
    // 恢复启用。
    for (int32_t ms = 0; ms < 86400000; ms += 1000) {
        Time t = Time::from_millisecs_since_midnight(ms);
        EXPECT_EQ(t.hour(), ms / 3600000);
        EXPECT_EQ(t.minute(), (ms % 3600000) / 60000);
        EXPECT_EQ(t.second(), (ms % 60000) / 1000);
        EXPECT_EQ(t.millisec(), 0);
    }
}

// 补充: 非零毫秒分量覆盖 (MillisecondTraversal 步进 1000ms, millisec() 恒为 0,
// 未覆盖 ms%1000!=0 的取值路径)。
TEST_F(TimeTest, MillisecondComponentNonZero) {
    constexpr int32_t base = 9 * 3600000 + 30 * 60000 + 15 * 1000;
    Time t = Time::from_millisecs_since_midnight(base + 123);
    EXPECT_EQ(t.hour(), 9);
    EXPECT_EQ(t.minute(), 30);
    EXPECT_EQ(t.second(), 15);
    EXPECT_EQ(t.millisec(), 123);
    EXPECT_EQ(t.millisecs_since_midnight(), base + 123);
}
