#include <dztrader/date_time/date_time.h>

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <gtest/gtest.h>

using namespace dztrader;

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
    auto ymd = std::chrono::year_month_day{std::chrono::year{2024},
                                            std::chrono::month{6},
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
    DateTime dt{Date{2024, 1, 1}, Time{0, 0, 0}};
    EXPECT_GT(dt.days_since_epoch(), 0);
    EXPECT_GT(dt.hours_since_epoch(), 0);
    EXPECT_GT(dt.minutes_since_epoch(), 0);
    EXPECT_GT(dt.seconds_since_epoch(), 0);
    EXPECT_GT(dt.millisecs_since_epoch(), 0);
    EXPECT_GT(dt.microsecs_since_epoch(), 0);
    EXPECT_GT(dt.nanosecs_since_epoch(), 0);
}

TEST_F(DateTimeTest, SinceMidnightVarious) {
    DateTime dt{Date{2024, 1, 1}, Time{1, 30, 15, 500}};
    EXPECT_EQ(dt.hours_since_midnight(), 1);
    EXPECT_EQ(dt.minutes_since_midnight(), 90);
    EXPECT_EQ(dt.seconds_since_midnight(), 5415);
    EXPECT_EQ(dt.millisecs_since_midnight(), 5415500);
    EXPECT_GT(dt.microsecs_since_midnight(), 0);
    EXPECT_GT(dt.nanosecs_since_midnight(), 0);
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

TEST_F(DateTimeTest, DaysToZero) {
    EXPECT_EQ(market_open_.days_to(market_open_), 0);
}

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
    auto ymd = std::chrono::year_month_day{std::chrono::year{2024},
                                            std::chrono::month{6},
                                            std::chrono::day{15}};
    auto dur = std::chrono::hours{14} + std::chrono::minutes{30};
    auto hms = std::chrono::hh_mm_ss{dur};
    auto dt = DateTime::from_ymd_hms(ymd, hms);
    EXPECT_EQ(dt.year(), 2024);
    EXPECT_EQ(dt.hour(), 14);
}

TEST_F(DateTimeTest, FromYmdHmsChronoInvalidDate) {
    auto ymd = std::chrono::year_month_day{std::chrono::year{2024},
                                            std::chrono::month{2},
                                            std::chrono::day{30}};
    auto hms = std::chrono::hh_mm_ss{std::chrono::hours{0}};
    EXPECT_THROW(DateTime::from_ymd_hms(ymd, hms), std::invalid_argument);
}

TEST_F(DateTimeTest, FromYmdHmsChronoInvalidTime) {
    auto ymd = std::chrono::year_month_day{std::chrono::year{2024},
                                            std::chrono::month{1},
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
    auto now = DateTime::system_now();
    EXPECT_GT(now.nanosecs_since_epoch(), 0);
}

TEST_F(DateTimeTest, LocalNow) {
    auto now = DateTime::local_now();
    EXPECT_GT(now.nanosecs_since_epoch(), 0);
}

TEST_F(DateTimeTest, SystemToLocalRoundTrip) {
    auto sys = DateTime::system_now();
    auto local = DateTime::system_to_local(sys);
    auto sys2 = DateTime::local_to_system(local);
    EXPECT_EQ(sys.nanosecs_since_epoch(), sys2.nanosecs_since_epoch());
}

// ========== C2 回归测试：时区 fallback 路径交叉验证 ==========
// 说明：mktime/localtime_s 在正常时间戳下不会失败，无法直接触发 errno!=0 分支。
// 此测试通过复现 fallback 逻辑（std::chrono::zoned_time）并与正常路径结果对比，
// 验证 fallback 算法正确性。若两条路径结果一致，则 fallback 分支逻辑可信。

TEST_F(DateTimeTest, SystemToLocalFallbackMatchesNormalPath) {
    // 用一个已知的系统时间点
    DateTime sys{Date{2024, 6, 15}, Time{12, 30, 45, 500}};
    auto local_via_normal = DateTime::system_to_local(sys);

    // 复现 fallback 逻辑：用 zoned_time 转换
    auto local_via_fallback = std::chrono::zoned_time(
        std::chrono::current_zone(),
        std::chrono::sys_time<std::chrono::nanoseconds>{
            std::chrono::nanoseconds{sys.nanosecs_since_epoch()}})
        .get_local_time();

    // 两条路径结果应一致（纳秒级）
    EXPECT_EQ(local_via_normal.nanosecs_since_epoch(),
              local_via_fallback.time_since_epoch().count());
}

TEST_F(DateTimeTest, LocalToSystemFallbackMatchesNormalPath) {
    // 用一个已知的本地时间点
    DateTime local{Date{2024, 6, 15}, Time{12, 30, 45, 500}};
    auto sys_via_normal = DateTime::local_to_system(local);

    // 复现 fallback 逻辑：用 zoned_time 转换
    auto sys_via_fallback = std::chrono::zoned_time(
        std::chrono::current_zone(),
        std::chrono::local_time(std::chrono::nanoseconds{local.nanosecs_since_epoch()}));
    auto sys_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        sys_via_fallback.get_sys_time().time_since_epoch());

    // 两条路径结果应一致（纳秒级）
    EXPECT_EQ(sys_via_normal.nanosecs_since_epoch(), sys_ns.count());
}

TEST_F(DateTimeTest, SystemToLocalRoundTripMultipleTimestamps) {
    // 多个时间点的往返一致性，覆盖不同季节（夏令时/冬令时边界附近）
    struct Ymd { int32_t y; int32_t m; int32_t d; };
    std::vector<Ymd> test_cases = {
        {2024, 1, 1},   // 冬
        {2024, 3, 15},  // 春（部分时区夏令时切换附近）
        {2024, 6, 21},  // 夏
        {2024, 9, 15},  // 秋
        {2024, 11, 5},  // 秋（部分时区冬令时切换附近）
        {2024, 12, 31}, // 冬
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

TEST_F(DateTimeTest, DISABLED_YearRange1980To2100) {
    auto start = std::chrono::sys_days{std::chrono::year_month_day{
        std::chrono::year{1980}, std::chrono::month{1}, std::chrono::day{1}}};
    auto end = std::chrono::sys_days{std::chrono::year_month_day{
        std::chrono::year{2100}, std::chrono::month{12}, std::chrono::day{31}}};

    for (auto dp = start; dp <= end; dp += std::chrono::days{1}) {
        Date d{std::chrono::year_month_day{dp}};
        Time t{9, 30, 0};
        DateTime dt{d, t};

        EXPECT_EQ(dt.date(), d) << "Date mismatch for " << d.to_string();
        EXPECT_EQ(dt.time(), t) << "Time mismatch for " << d.to_string();
        EXPECT_EQ(dt.year(), d.year());
        EXPECT_EQ(dt.month(), d.month());
        EXPECT_EQ(dt.day(), d.day());
        EXPECT_EQ(dt.weekday(), d.weekday());
    }
}

TEST_F(DateTimeTest, DISABLED_NightSessionExhaustive) {
    auto start = std::chrono::sys_days{std::chrono::year_month_day{
        std::chrono::year{1980}, std::chrono::month{1}, std::chrono::day{1}}};
    auto end = std::chrono::sys_days{std::chrono::year_month_day{
        std::chrono::year{2100}, std::chrono::month{12}, std::chrono::day{31}}};

    for (auto dp = start; dp <= end; dp += std::chrono::days{1}) {
        Date d{std::chrono::year_month_day{dp}};
        auto wd = d.weekday();
        for (int32_t h = 0; h < 24; ++h) {
            DateTime action{d, Time{h, 0, 0}};
            auto trading = DateTime::action_day_to_trading_day(action);
            if (wd == Weekday::Saturday || wd == Weekday::Sunday) {
                EXPECT_THROW(DateTime::trading_day_to_action_day(trading),
                             std::invalid_argument)
                    << "Should throw for trading_day weekday="
                    << static_cast<int>(trading.weekday());
                continue;
            }
            auto action2 = DateTime::trading_day_to_action_day(trading);
            EXPECT_EQ(action.date(), action2.date())
                << "Round-trip failed: " << d.to_string() << " h=" << h;
        }
    }
}

TEST_F(DateTimeTest, DISABLED_MarketDataSimulation) {
    constexpr int64_t TICK_COUNT = 1000000;
    constexpr int64_t START_TS = 1704067200;
    constexpr int64_t END_TS = 1735689600;

    std::srand(42);
    for (int64_t i = 0; i < TICK_COUNT; ++i) {
        int64_t ts = START_TS + (std::rand() % static_cast<int>(END_TS - START_TS));
        auto dt = DateTime::from_timestamp(ts);
        auto str = dt.to_string();
        auto dt2 = DateTime::from_string(str);
        EXPECT_EQ(dt.seconds_since_epoch(), dt2.seconds_since_epoch())
            << "Round-trip failed at tick " << i;

        if (i > 0) {
            int64_t ts2 = START_TS + (std::rand() % static_cast<int>(END_TS - START_TS));
            auto dt3 = DateTime::from_timestamp(ts2);
            auto diff = dt3 - dt;
            EXPECT_EQ(
                std::chrono::duration_cast<std::chrono::seconds>(diff).count(),
                dt3.seconds_since_epoch() - dt.seconds_since_epoch());
        }
    }
}

TEST_F(DateTimeTest, DISABLED_TimezoneRoundTrip) {
    for (int32_t y = 1980; y <= 2100; ++y) {
        for (int32_t m = 1; m <= 12; ++m) {
            for (int32_t h : {0, 6, 12, 18}) {
                auto dt = DateTime::from_ymd_hms(y, m, 1, h, 0, 0);
                auto local = DateTime::system_to_local(dt);
                auto sys = DateTime::local_to_system(local);
                EXPECT_EQ(dt.nanosecs_since_epoch(), sys.nanosecs_since_epoch())
                    << "Timezone round-trip failed: " << y << "-" << m << "-1 " << h << ":00";
            }
        }
    }
}
