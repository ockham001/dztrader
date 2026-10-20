#include "date_time_exhaustive_cases.h"

#include <dztrader/date_time/date_time.h>

#include <cerrno>
#include <chrono>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <gtest/gtest.h>

using namespace dztrader;
using namespace dztrader::strategy_api::test;

namespace {

/// @brief localtime 失败注入：执行 system_to_local 的 fallback 分支。
bool always_failing_local_time(std::tm&, std::time_t) noexcept { return false; }

/// @brief mktime 失败注入：执行 local_to_system 的 fallback 分支。
std::time_t always_failing_mktime(std::tm*) {
    errno = 0;
    return static_cast<std::time_t>(-1);
}

}  // namespace

class DateTimeTest : public ::testing::Test {
protected:
    DateTime epoch_dt_;
    DateTime market_open_{Date{2024, 1, 2}, Time{9, 30, 0}};
};

TEST_F(DateTimeTest, DefaultConstructorIsEpoch) {
    DateTime dt;
    EXPECT_EQ(dt.nanosecs_since_epoch(), 0);
    EXPECT_EQ(dt.year(), 1970);
    EXPECT_EQ(dt.month(), 1);
    EXPECT_EQ(dt.day(), 1);
    EXPECT_EQ(dt.hour(), 0);
}

TEST_F(DateTimeTest, ConstructorFromNanosecs) {
    DateTime dt{1'000'000'000LL};
    EXPECT_EQ(dt.seconds_since_epoch(), 1);
}

TEST_F(DateTimeTest, ConstructorFromNanosecsNegative) {
    DateTime dt{-1'000'000'000LL};
    EXPECT_EQ(dt.seconds_since_epoch(), -1);
}

TEST_F(DateTimeTest, ConstructorFromNanosecsZero) {
    DateTime dt{0LL};
    EXPECT_EQ(dt, epoch_dt_);
}

TEST_F(DateTimeTest, ConstructorFromChronoDuration) {
    DateTime dt{std::chrono::seconds{3600}};
    EXPECT_EQ(dt.hour(), 1);
    EXPECT_EQ(dt.minute(), 0);
}

TEST_F(DateTimeTest, ConstructorFromDateAndTime) {
    Date d{2024, 6, 15};
    Time t{14, 30, 0};
    DateTime dt{d, t};
    EXPECT_EQ(dt.date(), d);
    EXPECT_EQ(dt.time(), t);
}

TEST_F(DateTimeTest, ConstructorFromYMDAndHMS) {
    auto ymd = std::chrono::year_month_day{std::chrono::year{2024}, std::chrono::month{6},
                                           std::chrono::day{15}};
    auto dur = std::chrono::hours{14} + std::chrono::minutes{30};
    auto hms = std::chrono::hh_mm_ss{dur};
    DateTime dt{ymd, hms};
    EXPECT_EQ(dt.year(), 2024);
    EXPECT_EQ(dt.month(), 6);
    EXPECT_EQ(dt.day(), 15);
    EXPECT_EQ(dt.hour(), 14);
    EXPECT_EQ(dt.minute(), 30);
}

TEST_F(DateTimeTest, DateAndTime) {
    Date d{2024, 6, 15};
    Time t{14, 30, 45, 123};
    DateTime dt{d, t};
    EXPECT_EQ(dt.date(), d);
    EXPECT_EQ(dt.time(), t);
}

TEST_F(DateTimeTest, YearMonthDayWeekday) {
    Date d{2024, 1, 5};
    DateTime dt{d, Time{0, 0, 0}};
    EXPECT_EQ(dt.year(), 2024);
    EXPECT_EQ(dt.month(), 1);
    EXPECT_EQ(dt.day(), 5);
    EXPECT_EQ(dt.weekday(), Weekday::Friday);
}

TEST_F(DateTimeTest, HourMinuteSecondMillisecMicrosecNanosec) {
    DateTime dt = DateTime::from_ymd_hms(2024, 6, 15, 14, 30, 45, 123, 456, 789);
    EXPECT_EQ(dt.hour(), 14);
    EXPECT_EQ(dt.minute(), 30);
    EXPECT_EQ(dt.second(), 45);
    EXPECT_EQ(dt.millisec(), 123);
    EXPECT_EQ(dt.microsec(), 456);
    EXPECT_EQ(dt.nanosec(), 789);
}

TEST_F(DateTimeTest, SubsecondsNano) {
    DateTime dt = DateTime::from_ymd_hms(2024, 6, 15, 14, 30, 45, 123, 456, 789);
    auto ns = dt.subseconds<std::chrono::nanoseconds>();
    EXPECT_EQ(ns.count(), 123456789);
}

TEST_F(DateTimeTest, SubsecondsMicro) {
    DateTime dt = DateTime::from_ymd_hms(2024, 6, 15, 14, 30, 45, 123, 456, 789);
    auto us = dt.subseconds<std::chrono::microseconds>();
    EXPECT_EQ(us.count(), 123456);
}

TEST_F(DateTimeTest, SubsecondsMilli) {
    DateTime dt = DateTime::from_ymd_hms(2024, 6, 15, 14, 30, 45, 123, 456, 789);
    auto ms = dt.subseconds<std::chrono::milliseconds>();
    EXPECT_EQ(ms.count(), 123);
}

TEST_F(DateTimeTest, SinceEpochVarious) {
    // 2024-01-01T00:00:00Z 的精确常量；单位或量级错误必须变红。
    DateTime dt{Date{2024, 1, 1}, Time{0, 0, 0}};
    EXPECT_EQ(dt.days_since_epoch(), 19723);
    EXPECT_EQ(dt.hours_since_epoch(), 473352);
    EXPECT_EQ(dt.minutes_since_epoch(), 28401120);
    EXPECT_EQ(dt.seconds_since_epoch(), 1704067200);
    EXPECT_EQ(dt.millisecs_since_epoch(), 1704067200000LL);
    EXPECT_EQ(dt.microsecs_since_epoch(), 1704067200000000LL);
    EXPECT_EQ(dt.nanosecs_since_epoch(), 1704067200000000000LL);
}

TEST_F(DateTimeTest, SinceMidnightVarious) {
    DateTime dt{Date{2024, 1, 1}, Time{1, 30, 15, 500}};
    EXPECT_EQ(dt.hours_since_midnight(), 1);
    EXPECT_EQ(dt.minutes_since_midnight(), 90);
    EXPECT_EQ(dt.seconds_since_midnight(), 5415);
    EXPECT_EQ(dt.millisecs_since_midnight(), 5415500);
    EXPECT_EQ(dt.microsecs_since_midnight(), 5415500000LL);
    EXPECT_EQ(dt.nanosecs_since_midnight(), 5415500000000LL);
}

TEST_F(DateTimeTest, NegativeNanosecondDecomposition) {
    // date_time.h 声明 subseconds() 始终非负；-1 ns 必须分解为 1969-12-31 23:59:59.999999999。
    DateTime dt{-1};
    EXPECT_EQ(dt.subseconds<std::chrono::nanoseconds>().count(), 999999999);
    EXPECT_EQ(dt.hour(), 23);
    EXPECT_EQ(dt.minute(), 59);
    EXPECT_EQ(dt.second(), 59);
    EXPECT_EQ(dt.nanosec(), 999);
    EXPECT_EQ(dt.year(), 1969);
    EXPECT_EQ(dt.month(), 12);
    EXPECT_EQ(dt.day(), 31);
}

TEST_F(DateTimeTest, TimestampInt64) {
    DateTime dt = DateTime::from_timestamp(int64_t{1704067200});
    EXPECT_EQ(dt.timestamp<int64_t>(), 1704067200);
}

TEST_F(DateTimeTest, TimestampDouble) {
    DateTime dt = DateTime::from_timestamp(1704067200.5);
    auto ts = dt.timestamp<double>();
    EXPECT_NEAR(ts, 1704067200.5, 0.001);
}

TEST_F(DateTimeTest, TimestampTimeT) {
    DateTime dt = DateTime::from_timestamp(std::time_t{1704067200});
    EXPECT_EQ(dt.timestamp<std::time_t>(), 1704067200);
}

TEST_F(DateTimeTest, HhMmSs) {
    DateTime dt{Date{2024, 1, 1}, Time{14, 30, 45}};
    auto hms = dt.hh_mm_ss<std::chrono::nanoseconds>();
    EXPECT_EQ(hms.hours().count(), 14);
    EXPECT_EQ(hms.minutes().count(), 30);
    EXPECT_EQ(hms.seconds().count(), 45);
}

TEST_F(DateTimeTest, YearMonthDayAndWeekdayAccessors) {
    Date d{2024, 1, 5};
    DateTime dt{d, Time{0, 0, 0}};
    auto ymd = dt.year_month_day();
    EXPECT_EQ(static_cast<int>(ymd.year()), 2024);
    auto ymwd = dt.year_month_weekday();
    EXPECT_EQ(ymwd.weekday().iso_encoding(), 5);
}

TEST_F(DateTimeTest, AddNanosecs) {
    auto dt = market_open_.add_nanosecs(1);
    EXPECT_EQ(dt.nanosec(), 1);
}

TEST_F(DateTimeTest, AddMicrosecs) {
    auto dt = market_open_.add_microsecs(1);
    EXPECT_EQ(dt.microsec(), 1);
}

TEST_F(DateTimeTest, AddMillisecs) {
    auto dt = market_open_.add_millisecs(500);
    EXPECT_EQ(dt.millisec(), 500);
}

TEST_F(DateTimeTest, AddSeconds) {
    auto dt = market_open_.add_seconds(30);
    EXPECT_EQ(dt.second(), 30);
}

TEST_F(DateTimeTest, AddMinutes) {
    auto dt = market_open_.add_minutes(30);
    EXPECT_EQ(dt.hour(), 10);
    EXPECT_EQ(dt.minute(), 0);
}

TEST_F(DateTimeTest, AddHours) {
    auto dt = market_open_.add_hours(1);
    EXPECT_EQ(dt.hour(), 10);
}

TEST_F(DateTimeTest, AddDays) {
    auto dt = market_open_.add_days(1);
    EXPECT_EQ(dt.day(), 3);
}

TEST_F(DateTimeTest, AddMonths) {
    Date d{2024, 1, 15};
    auto dt = DateTime{d, Time{12, 0, 0}}.add_months(1);
    EXPECT_EQ(dt.month(), 2);
    EXPECT_EQ(dt.day(), 15);
    EXPECT_EQ(dt.hour(), 12);
}

TEST_F(DateTimeTest, AddYears) {
    auto dt = market_open_.add_years(1);
    EXPECT_EQ(dt.year(), 2025);
}

TEST_F(DateTimeTest, DaysToPositive) {
    DateTime a{Date{2024, 1, 1}, Time{0, 0, 0}};
    DateTime b{Date{2024, 1, 11}, Time{0, 0, 0}};
    EXPECT_EQ(a.days_to(b), 10);
}

TEST_F(DateTimeTest, DaysToNegative) {
    DateTime a{Date{2024, 1, 11}, Time{0, 0, 0}};
    DateTime b{Date{2024, 1, 1}, Time{0, 0, 0}};
    EXPECT_EQ(a.days_to(b), -10);
}

TEST_F(DateTimeTest, DaysToZero) { EXPECT_EQ(market_open_.days_to(market_open_), 0); }

TEST_F(DateTimeTest, OperatorPlusDuration) {
    auto dt = market_open_ + std::chrono::hours{1};
    EXPECT_EQ(dt.hour(), 10);
}

TEST_F(DateTimeTest, OperatorMinusDuration) {
    auto dt = market_open_ - std::chrono::hours{1};
    EXPECT_EQ(dt.hour(), 8);
}

TEST_F(DateTimeTest, OperatorPlusMonths) {
    Date d{2024, 1, 31};
    auto dt = DateTime{d, Time{12, 0, 0}} + std::chrono::months{1};
    EXPECT_EQ(dt.month(), 2);
    EXPECT_EQ(dt.day(), 29);
    EXPECT_EQ(dt.hour(), 12);
}

TEST_F(DateTimeTest, OperatorPlusYears) {
    auto dt = market_open_ + std::chrono::years{1};
    EXPECT_EQ(dt.year(), 2025);
}

TEST_F(DateTimeTest, OperatorPlusAssign) {
    DateTime dt = market_open_;
    dt += std::chrono::hours{1};
    EXPECT_EQ(dt.hour(), 10);
}

TEST_F(DateTimeTest, OperatorMinusAssign) {
    DateTime dt = market_open_;
    dt -= std::chrono::hours{1};
    EXPECT_EQ(dt.hour(), 8);
}

TEST_F(DateTimeTest, OperatorDateTimeDifference) {
    DateTime a{Date{2024, 1, 1}, Time{9, 0, 0}};
    DateTime b{Date{2024, 1, 1}, Time{10, 30, 0}};
    auto diff = b - a;
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(diff).count(), 5400);
}

TEST_F(DateTimeTest, ComparisonOperators) {
    DateTime a{Date{2024, 1, 1}, Time{9, 0, 0}};
    DateTime b{Date{2024, 1, 1}, Time{10, 0, 0}};
    EXPECT_EQ(a, a);
    EXPECT_NE(a, b);
    EXPECT_LT(a, b);
    EXPECT_LE(a, b);
    EXPECT_GT(b, a);
    EXPECT_GE(b, a);
}

TEST_F(DateTimeTest, ToStringDefaultFormat) {
    DateTime dt{Date{2024, 3, 15}, Time{14, 30, 45}};
    EXPECT_EQ(dt.to_string(), "2024-03-15 14:30:45");
}

TEST_F(DateTimeTest, ToStringCustomFormat) {
    DateTime dt{Date{2024, 3, 15}, Time{14, 30, 45}};
    EXPECT_EQ(dt.to_string("%F"), "2024-03-15");
    EXPECT_EQ(dt.to_string("%R"), "14:30");
}

TEST_F(DateTimeTest, ToStringFraction) {
    DateTime dt = DateTime::from_ymd_hms(2024, 3, 15, 14, 30, 45, 123);
    EXPECT_EQ(dt.to_string("%T", 3), "14:30:45.123");
}

TEST_F(DateTimeTest, ToStringFractionZero) {
    DateTime dt{Date{2024, 3, 15}, Time{14, 30, 45}};
    EXPECT_EQ(dt.to_string("%T", 0), "14:30:45");
}

TEST_F(DateTimeTest, ToStringStringTypeTemplate) {
    DateTime dt{Date{2024, 3, 15}, Time{14, 30, 45}};
    auto s = dt.to_string<std::string>();
    EXPECT_EQ(s, "2024-03-15 14:30:45");
}

TEST_F(DateTimeTest, FromStringFull) {
    auto dt = DateTime::from_string("2024-03-15 14:30:45");
    EXPECT_EQ(dt.year(), 2024);
    EXPECT_EQ(dt.month(), 3);
    EXPECT_EQ(dt.day(), 15);
    EXPECT_EQ(dt.hour(), 14);
    EXPECT_EQ(dt.minute(), 30);
    EXPECT_EQ(dt.second(), 45);
}

TEST_F(DateTimeTest, FromStringWithSubsecond) {
    auto dt = DateTime::from_string("2024-03-15 14:30:45.123456789");
    EXPECT_EQ(dt.millisec(), 123);
    EXPECT_EQ(dt.microsec(), 456);
    EXPECT_EQ(dt.nanosec(), 789);
}

TEST_F(DateTimeTest, FromStringMissingDateComponents) {
    EXPECT_THROW(DateTime::from_string("14:30:45", "%H:%M:%S"), std::invalid_argument);
}

TEST_F(DateTimeTest, FromStringInvalidDate) {
    EXPECT_THROW(DateTime::from_string("2024-02-30 14:30:45"), std::invalid_argument);
}

TEST_F(DateTimeTest, FromStringDateOnly) {
    auto dt = DateTime::from_string("2024-03-15", "%F");
    EXPECT_EQ(dt.year(), 2024);
    EXPECT_EQ(dt.month(), 3);
    EXPECT_EQ(dt.day(), 15);
    EXPECT_EQ(dt.hour(), 0);
    EXPECT_EQ(dt.minute(), 0);
    EXPECT_EQ(dt.second(), 0);
}

TEST_F(DateTimeTest, FromYmdHmsChronoValid) {
    auto ymd = std::chrono::year_month_day{std::chrono::year{2024}, std::chrono::month{6},
                                           std::chrono::day{15}};
    auto dur = std::chrono::hours{14} + std::chrono::minutes{30};
    auto hms = std::chrono::hh_mm_ss{dur};
    auto dt = DateTime::from_ymd_hms(ymd, hms);
    EXPECT_EQ(dt.year(), 2024);
    EXPECT_EQ(dt.hour(), 14);
}

TEST_F(DateTimeTest, FromYmdHmsChronoInvalidDate) {
    auto ymd = std::chrono::year_month_day{std::chrono::year{2024}, std::chrono::month{2},
                                           std::chrono::day{30}};
    auto hms = std::chrono::hh_mm_ss{std::chrono::hours{0}};
    EXPECT_THROW(DateTime::from_ymd_hms(ymd, hms), std::invalid_argument);
}

TEST_F(DateTimeTest, FromYmdHmsChronoInvalidTime) {
    auto ymd = std::chrono::year_month_day{std::chrono::year{2024}, std::chrono::month{1},
                                           std::chrono::day{1}};
    auto dur = std::chrono::hours{25};
    auto hms = std::chrono::hh_mm_ss{dur};
    EXPECT_THROW(DateTime::from_ymd_hms(ymd, hms), std::invalid_argument);
}

TEST_F(DateTimeTest, FromYmdHmsIntValid) {
    auto dt = DateTime::from_ymd_hms(2024, 6, 15, 14, 30, 45, 123, 456, 789);
    EXPECT_EQ(dt.year(), 2024);
    EXPECT_EQ(dt.millisec(), 123);
    EXPECT_EQ(dt.microsec(), 456);
    EXPECT_EQ(dt.nanosec(), 789);
}

TEST_F(DateTimeTest, FromYmdHmsIntInvalidHour) {
    EXPECT_THROW(DateTime::from_ymd_hms(2024, 1, 1, 25), std::invalid_argument);
}

TEST_F(DateTimeTest, FromYmdHmsIntInvalidMinute) {
    EXPECT_THROW(DateTime::from_ymd_hms(2024, 1, 1, 0, 60), std::invalid_argument);
}

TEST_F(DateTimeTest, FromYmdHmsIntInvalidSecond) {
    EXPECT_THROW(DateTime::from_ymd_hms(2024, 1, 1, 0, 0, 60), std::invalid_argument);
}

TEST_F(DateTimeTest, FromYmdHmsIntInvalidMillisec) {
    EXPECT_THROW(DateTime::from_ymd_hms(2024, 1, 1, 0, 0, 0, 1000), std::invalid_argument);
}

TEST_F(DateTimeTest, FromTimestampInt) {
    auto dt = DateTime::from_timestamp(int64_t{1704067200});
    EXPECT_EQ(dt.seconds_since_epoch(), 1704067200);
}

TEST_F(DateTimeTest, FromTimestampNegative) {
    auto dt = DateTime::from_timestamp(int64_t{-1});
    EXPECT_EQ(dt.seconds_since_epoch(), -1);
}

TEST_F(DateTimeTest, FromTimestampZero) {
    auto dt = DateTime::from_timestamp(int64_t{0});
    EXPECT_EQ(dt, epoch_dt_);
}

TEST_F(DateTimeTest, FromTimestampDouble) {
    auto dt = DateTime::from_timestamp(1704067200.5);
    EXPECT_EQ(dt.seconds_since_epoch(), 1704067200);
    EXPECT_EQ(dt.millisec(), 500);
}

TEST_F(DateTimeTest, SystemNow) {
    const auto before = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto now = DateTime::system_now();
    const auto after = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    EXPECT_GE(now.nanosecs_since_epoch(), before);
    EXPECT_LE(now.nanosecs_since_epoch(), after);
}

TEST_F(DateTimeTest, LocalNow) {
    const auto before = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto now = DateTime::local_now();
    const auto after = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    // local_now 是本地墙钟编码，不能直接与 UTC 窗口比较；与同一窗口内
    // system_to_local 的结果对齐，仍能捕获单位/量级错误。
    const auto expected = DateTime::system_to_local(DateTime{(before + after) / 2});
    const int64_t diff = now.nanosecs_since_epoch() - expected.nanosecs_since_epoch();
    EXPECT_LT(diff, 1000000000LL);
    EXPECT_GT(diff, -1000000000LL);
}

TEST_F(DateTimeTest, SystemToLocalRoundTrip) {
    auto sys = DateTime::system_now();
    auto local = DateTime::system_to_local(sys);
    auto sys2 = DateTime::local_to_system(local);
    EXPECT_EQ(sys.nanosecs_since_epoch(), sys2.nanosecs_since_epoch());
}

// ========== C2 回归测试：时区 fallback 路径交叉验证 ==========
// mktime/localtime_s 在合法时间戳下不会失败，fallback 无法由输入触发；生产实现
// 通过模板参数暴露注入点（date_time.h），以下用例注入失败实现，直接执行生产
// fallback 分支并与正常路径交叉比对（正常路径是独立实现，不是测试内复刻）。
TEST_F(DateTimeTest, SystemToLocalFallbackMatchesNormalPath) {
    DateTime sys{Date{2024, 6, 15}, Time{12, 30, 45, 500}};
    const auto normal = DateTime::system_to_local(sys);
    const auto fallback = DateTime::system_to_local<always_failing_local_time>(sys);
    EXPECT_EQ(normal.nanosecs_since_epoch(), fallback.nanosecs_since_epoch());
}

TEST_F(DateTimeTest, LocalToSystemFallbackMatchesNormalPath) {
    DateTime local{Date{2024, 6, 15}, Time{12, 30, 45, 500}};
    const auto normal = DateTime::local_to_system(local);
    const auto fallback = DateTime::local_to_system<always_failing_mktime>(local);
    EXPECT_EQ(normal.nanosecs_since_epoch(), fallback.nanosecs_since_epoch());
}

TEST_F(DateTimeTest, SystemToLocalRoundTripMultipleTimestamps) {
    // 仅断言 system→local→system 往返恒等。DST 语义依赖宿主时区与平台 tzdata，
    // 无法在 Linux/Windows CI 上固定，本用例不声称覆盖 DST。
    struct Ymd {
        int32_t y;
        int32_t m;
        int32_t d;
    };
    std::vector<Ymd> test_cases = {
        {2024, 1, 1},    // 冬
        {2024, 3, 15},   // 春
        {2024, 6, 21},   // 夏
        {2024, 9, 15},   // 秋
        {2024, 11, 5},   // 秋末
        {2024, 12, 31},  // 冬
    };
    for (const auto& tc : test_cases) {
        DateTime sys{Date{tc.y, tc.m, tc.d}, Time{10, 0, 0}};
        auto local = DateTime::system_to_local(sys);
        auto sys2 = DateTime::local_to_system(local);
        EXPECT_EQ(sys.nanosecs_since_epoch(), sys2.nanosecs_since_epoch())
            << "Round-trip failed for " << tc.y << "-" << tc.m << "-" << tc.d;
    }
}

TEST_F(DateTimeTest, ActionDayToTradingDayMonToThuBefore18) {
    for (int32_t wd = static_cast<int32_t>(Weekday::Monday);
         wd <= static_cast<int32_t>(Weekday::Thursday); ++wd) {
        Date d{2024, 1, wd};
        DateTime dt{d, Time{10, 0, 0}};
        auto trading = DateTime::action_day_to_trading_day(dt);
        EXPECT_EQ(trading.date(), dt.date()) << "weekday=" << wd << " before 18";
    }
}

TEST_F(DateTimeTest, ActionDayToTradingDayMonToThuAfter18) {
    for (int32_t wd = static_cast<int32_t>(Weekday::Monday);
         wd <= static_cast<int32_t>(Weekday::Thursday); ++wd) {
        Date d{2024, 1, wd};
        DateTime dt{d, Time{20, 0, 0}};
        auto trading = DateTime::action_day_to_trading_day(dt);
        EXPECT_EQ(trading.days_to(dt), -1) << "weekday=" << wd << " after 18";
    }
}

TEST_F(DateTimeTest, ActionDayToTradingDayFridayBefore18) {
    Date d{2024, 1, 5};
    DateTime dt{d, Time{10, 0, 0}};
    auto trading = DateTime::action_day_to_trading_day(dt);
    EXPECT_EQ(trading.date(), dt.date());
}

TEST_F(DateTimeTest, ActionDayToTradingDayFridayAfter18) {
    Date d{2024, 1, 5};
    DateTime dt{d, Time{20, 0, 0}};
    auto trading = DateTime::action_day_to_trading_day(dt);
    EXPECT_EQ(trading.weekday(), Weekday::Monday);
}

TEST_F(DateTimeTest, ActionDayToTradingDaySaturday) {
    Date d{2024, 1, 6};
    DateTime dt{d, Time{10, 0, 0}};
    auto trading = DateTime::action_day_to_trading_day(dt);
    EXPECT_EQ(trading.weekday(), Weekday::Monday);
}

TEST_F(DateTimeTest, ActionDayToTradingDaySunday) {
    Date d{2024, 1, 7};
    DateTime dt{d, Time{10, 0, 0}};
    auto trading = DateTime::action_day_to_trading_day(dt);
    EXPECT_EQ(trading.weekday(), Weekday::Monday);
}

TEST_F(DateTimeTest, TradingDayToActionDayMondayBefore6) {
    Date d{2024, 1, 8};
    DateTime dt{d, Time{3, 0, 0}};
    auto action = DateTime::trading_day_to_action_day(dt);
    EXPECT_EQ(action.weekday(), Weekday::Saturday);
}

TEST_F(DateTimeTest, TradingDayToActionDayMonday6To18) {
    Date d{2024, 1, 8};
    DateTime dt{d, Time{10, 0, 0}};
    auto action = DateTime::trading_day_to_action_day(dt);
    EXPECT_EQ(action.date(), dt.date());
}

TEST_F(DateTimeTest, TradingDayToActionDayMondayAfter18) {
    Date d{2024, 1, 8};
    DateTime dt{d, Time{20, 0, 0}};
    auto action = DateTime::trading_day_to_action_day(dt);
    EXPECT_EQ(action.weekday(), Weekday::Friday);
}

TEST_F(DateTimeTest, TradingDayToActionDayTueToFriBefore18) {
    for (int32_t wd = static_cast<int32_t>(Weekday::Tuesday);
         wd <= static_cast<int32_t>(Weekday::Friday); ++wd) {
        Date d{2024, 1, wd};
        DateTime dt{d, Time{10, 0, 0}};
        auto action = DateTime::trading_day_to_action_day(dt);
        EXPECT_EQ(action.date(), dt.date()) << "weekday=" << wd << " before 18";
    }
}

TEST_F(DateTimeTest, TradingDayToActionDayTueToFriAfter18) {
    for (int32_t wd = static_cast<int32_t>(Weekday::Tuesday);
         wd <= static_cast<int32_t>(Weekday::Friday); ++wd) {
        Date d{2024, 1, wd};
        DateTime dt{d, Time{20, 0, 0}};
        auto action = DateTime::trading_day_to_action_day(dt);
        EXPECT_EQ(action.weekday(), static_cast<Weekday>(wd - 1))
            << "weekday=" << wd << " after 18";
    }
}

TEST_F(DateTimeTest, TradingDayToActionDayWeekendThrows) {
    Date sat{2024, 1, 6};
    DateTime dt_sat{sat, Time{10, 0, 0}};
    EXPECT_THROW(DateTime::trading_day_to_action_day(dt_sat), std::invalid_argument);
    Date sun{2024, 1, 7};
    DateTime dt_sun{sun, Time{10, 0, 0}};
    EXPECT_THROW(DateTime::trading_day_to_action_day(dt_sun), std::invalid_argument);
}

TEST_F(DateTimeTest, ActionTradingRoundTrip) {
    for (int32_t wd = static_cast<int32_t>(Weekday::Monday);
         wd <= static_cast<int32_t>(Weekday::Friday); ++wd) {
        for (int32_t h = 0; h < 24; ++h) {
            if (wd == static_cast<int32_t>(Weekday::Monday) && h < 6) {
                // 周一 0–6 点不是有效 action 时段（属于上周六夜盘），
                // 其映射由 ActionTradingMondayBefore6MapsToSaturday 显式钉住。
                continue;
            }
            Date d{2024, 1, wd};
            DateTime action{d, Time{h, 0, 0}};
            auto trading = DateTime::action_day_to_trading_day(action);
            auto action2 = DateTime::trading_day_to_action_day(trading);
            EXPECT_EQ(action.date(), action2.date())
                << "Round-trip failed: wd=" << wd << " h=" << h;
        }
    }
}

// 周一 0–6 点属于上周六夜盘（业务日/交易日映射），不是有效 action 时段：
//   action 周一 03:00 → trading 周一 03:00（action_day_to_trading_day 恒等）
//   trading 周一 03:00 → action 上周六 03:00（trading_day_to_action_day 回退 2 天）
// 因此该区间 action 往返不恒等；与已启用的 TradingDayToActionDayMondayBefore6 一致。
TEST_F(DateTimeTest, ActionTradingMondayBefore6MapsToSaturday) {
    for (int32_t h = 0; h < 6; ++h) {
        const Date monday{2024, 1, 8};
        const DateTime action{monday, Time{h, 0, 0}};

        const auto trading = DateTime::action_day_to_trading_day(action);
        EXPECT_EQ(trading.date(), monday) << "h=" << h;

        const auto action_back = DateTime::trading_day_to_action_day(trading);
        EXPECT_EQ(action_back, (DateTime{Date{2024, 1, 6}, Time{h, 0, 0}})) << "h=" << h;
    }
}

TEST_F(DateTimeTest, ActionDayToTradingDayDateVersion) {
    Date action_date{2024, 1, 5};
    Time action_time{20, 0, 0};
    auto trading = DateTime::action_day_to_trading_day(action_date, action_time);
    EXPECT_EQ(trading.weekday(), Weekday::Monday);
}

TEST_F(DateTimeTest, TradingDayToActionDayDateVersion) {
    Date trading_date{2024, 1, 8};
    Time action_time{3, 0, 0};
    auto action = DateTime::trading_day_to_action_day(trading_date, action_time);
    EXPECT_EQ(action.weekday(), Weekday::Saturday);
}

TEST_F(DateTimeTest, OperatorStream) {
    DateTime dt{Date{2024, 3, 15}, Time{14, 30, 45}};
    std::ostringstream oss;
    oss << dt;
    EXPECT_EQ(oss.str(), "2024-03-15 14:30:45");
}

// 抽样版留在 fast 门禁；全量穷举见 date_time_exhaustive_test.cpp（slow 标签）。
TEST_F(DateTimeTest, YearRangeSampled) {
    check_date_time_year_range(range_start_1980(), range_end_2100(), std::chrono::days{29});
}

TEST_F(DateTimeTest, NightSessionSampled) {
    check_night_session(range_start_1980(), range_end_2100(), std::chrono::days{31});
}

TEST_F(DateTimeTest, MarketDataSimulationSampled) { check_market_data_simulation(50000); }

TEST_F(DateTimeTest, TimezoneRoundTripSampled) { check_timezone_round_trip(1980, 2100, 10); }
