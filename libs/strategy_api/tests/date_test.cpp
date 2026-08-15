#include <dztrader/date_time/date.h>

#include <chrono>
#include <sstream>
#include <gtest/gtest.h>

using namespace dztrader;

class DateTest : public ::testing::Test {
protected:
    Date epoch_{1970, 1, 1};
    Date leap_day_{2024, 2, 29};
    Date friday_{2024, 1, 5};
};

TEST_F(DateTest, DefaultConstructorIsEpoch) {
    Date d;
    EXPECT_EQ(d.days_since_epoch(), 0);
    EXPECT_EQ(d.year(), 1970);
    EXPECT_EQ(d.month(), 1);
    EXPECT_EQ(d.day(), 1);
}

TEST_F(DateTest, ConstructorFromDaysSinceEpoch) {
    Date d1{0};
    EXPECT_EQ(d1, epoch_);
    Date d2{1};
    EXPECT_EQ(d2.day(), 2);
    Date d3{-1};
    EXPECT_EQ(d3.year(), 1969);
    EXPECT_EQ(d3.month(), 12);
    EXPECT_EQ(d3.day(), 31);
}

TEST_F(DateTest, ConstructorFromYearMonthDay) {
    auto ymd = std::chrono::year_month_day{std::chrono::year{2024},
                                            std::chrono::month{2},
                                            std::chrono::day{29}};
    Date d(ymd);
    EXPECT_EQ(d, leap_day_);
    EXPECT_EQ(d.year(), 2024);
    EXPECT_EQ(d.month(), 2);
    EXPECT_EQ(d.day(), 29);
}

TEST_F(DateTest, ConstructorFromYearMonthWeekday) {
    auto ymwd = std::chrono::year_month_weekday{
        std::chrono::year{2024}, std::chrono::month{1},
        std::chrono::weekday_indexed{std::chrono::weekday{1u}, 1u}};
    Date d(ymwd);
    EXPECT_EQ(d.year(), 2024);
    EXPECT_EQ(d.month(), 1);
    EXPECT_EQ(d.day(), 1);
    EXPECT_EQ(d.weekday(), Weekday::Monday);
}

TEST_F(DateTest, ConstructorFromYearMonthDayLast) {
    auto ymdl = std::chrono::year_month_day_last{
        std::chrono::year{2024}, std::chrono::month_day_last{std::chrono::month{2}}};
    Date d(ymdl);
    EXPECT_EQ(d.day(), 29);
}

TEST_F(DateTest, ConstructorFromYearMonthWeekdayLast) {
    auto ymwdl = std::chrono::year_month_weekday_last{
        std::chrono::year{2024}, std::chrono::month{3},
        std::chrono::weekday_last{std::chrono::weekday{5u}}};
    Date d(ymwdl);
    EXPECT_EQ(d.month(), 3);
    EXPECT_EQ(d.weekday(), Weekday::Friday);
}

TEST_F(DateTest, ConstructorFromIntYMD) {
    Date d{2024, 6, 15};
    EXPECT_EQ(d.year(), 2024);
    EXPECT_EQ(d.month(), 6);
    EXPECT_EQ(d.day(), 15);
}

TEST_F(DateTest, ConstructorFromChronoYMD) {
    Date d{std::chrono::year{2024}, std::chrono::month{6}, std::chrono::day{15}};
    EXPECT_EQ(d.year(), 2024);
    EXPECT_EQ(d.month(), 6);
    EXPECT_EQ(d.day(), 15);
}

TEST_F(DateTest, Accessors) {
    Date d{2024, 3, 15};
    EXPECT_EQ(d.year(), 2024);
    EXPECT_EQ(d.month(), 3);
    EXPECT_EQ(d.day(), 15);
    EXPECT_EQ(d.weekday(), Weekday::Friday);
}

TEST_F(DateTest, WeekdayAllDays) {
    EXPECT_EQ((Date{2024, 1, 1}).weekday(), Weekday::Monday);
    EXPECT_EQ((Date{2024, 1, 2}).weekday(), Weekday::Tuesday);
    EXPECT_EQ((Date{2024, 1, 3}).weekday(), Weekday::Wednesday);
    EXPECT_EQ((Date{2024, 1, 4}).weekday(), Weekday::Thursday);
    EXPECT_EQ((Date{2024, 1, 5}).weekday(), Weekday::Friday);
    EXPECT_EQ((Date{2024, 1, 6}).weekday(), Weekday::Saturday);
    EXPECT_EQ((Date{2024, 1, 7}).weekday(), Weekday::Sunday);
}

TEST_F(DateTest, DaysSinceEpoch) {
    EXPECT_EQ(epoch_.days_since_epoch(), 0);
    EXPECT_EQ((Date{1970, 1, 2}).days_since_epoch(), 1);
}

TEST_F(DateTest, TimeSinceEpoch) {
    EXPECT_EQ(epoch_.time_since_epoch<std::chrono::days>().count(), 0);
    EXPECT_EQ(epoch_.time_since_epoch<std::chrono::hours>().count(), 0);
    EXPECT_EQ((Date{1970, 1, 2}).time_since_epoch<std::chrono::days>().count(), 1);
}

TEST_F(DateTest, YearMonthDayRoundTrip) {
    auto ymd = leap_day_.year_month_day();
    Date d2(ymd);
    EXPECT_EQ(d2, leap_day_);
}

TEST_F(DateTest, YearMonthWeekdayRoundTrip) {
    auto ymwd = friday_.year_month_weekday();
    Date d2(ymwd);
    EXPECT_EQ(d2, friday_);
}

TEST_F(DateTest, AddDaysPositive) {
    auto d = epoch_.add_days(10);
    EXPECT_EQ(d.day(), 11);
}

TEST_F(DateTest, AddDaysNegative) {
    auto d = epoch_.add_days(-1);
    EXPECT_EQ(d.year(), 1969);
    EXPECT_EQ(d.month(), 12);
    EXPECT_EQ(d.day(), 31);
}

TEST_F(DateTest, AddDaysZero) {
    auto d = epoch_.add_days(0);
    EXPECT_EQ(d, epoch_);
}

TEST_F(DateTest, AddMonthsPositive) {
    Date d{2024, 1, 15};
    auto r = d.add_months(1);
    EXPECT_EQ(r.month(), 2);
    EXPECT_EQ(r.day(), 15);
}

TEST_F(DateTest, AddMonthsNegative) {
    Date d{2024, 3, 15};
    auto r = d.add_months(-1);
    EXPECT_EQ(r.month(), 2);
    EXPECT_EQ(r.day(), 15);
}

TEST_F(DateTest, AddMonthsZero) {
    auto d = epoch_.add_months(0);
    EXPECT_EQ(d, epoch_);
}

TEST_F(DateTest, AddMonthsMonthEndTruncation) {
    Date d{2024, 1, 31};
    auto r = d.add_months(1);
    EXPECT_EQ(r.month(), 2);
    EXPECT_EQ(r.day(), 29);
}

TEST_F(DateTest, AddMonthsMonthEndTruncationNonLeap) {
    Date d{2023, 1, 31};
    auto r = d.add_months(1);
    EXPECT_EQ(r.month(), 2);
    EXPECT_EQ(r.day(), 28);
}

TEST_F(DateTest, AddMonthsSubtractToFeb) {
    Date d{2024, 3, 31};
    auto r = d.add_months(-1);
    EXPECT_EQ(r.month(), 2);
    EXPECT_EQ(r.day(), 29);
}

TEST_F(DateTest, AddYearsPositive) {
    auto d = epoch_.add_years(1);
    EXPECT_EQ(d.year(), 1971);
}

TEST_F(DateTest, AddYearsNegative) {
    auto d = epoch_.add_years(-1);
    EXPECT_EQ(d.year(), 1969);
}

TEST_F(DateTest, AddYearsZero) {
    auto d = epoch_.add_years(0);
    EXPECT_EQ(d, epoch_);
}

TEST_F(DateTest, AddYearsLeapToNonLeap) {
    Date d{2024, 2, 29};
    auto r = d.add_years(1);
    EXPECT_EQ(r.month(), 2);
    EXPECT_EQ(r.day(), 28);
}

TEST_F(DateTest, DaysToPositive) {
    Date a{2024, 1, 1};
    Date b{2024, 1, 11};
    EXPECT_EQ(a.days_to(b), 10);
}

TEST_F(DateTest, DaysToNegative) {
    Date a{2024, 1, 11};
    Date b{2024, 1, 1};
    EXPECT_EQ(a.days_to(b), -10);
}

TEST_F(DateTest, DaysToZero) {
    EXPECT_EQ(epoch_.days_to(epoch_), 0);
}

TEST_F(DateTest, OperatorPlusDays) {
    auto d = epoch_ + std::chrono::days{5};
    EXPECT_EQ(d.day(), 6);
}

TEST_F(DateTest, OperatorMinusDays) {
    auto d = epoch_ - std::chrono::days{1};
    EXPECT_EQ(d.year(), 1969);
}

TEST_F(DateTest, OperatorPlusMonthsZero) {
    auto d = epoch_ + std::chrono::months{0};
    EXPECT_EQ(d, epoch_);
}

TEST_F(DateTest, OperatorMinusMonthsZero) {
    auto d = epoch_ - std::chrono::months{0};
    EXPECT_EQ(d, epoch_);
}

TEST_F(DateTest, OperatorPlusYearsZero) {
    auto d = epoch_ + std::chrono::years{0};
    EXPECT_EQ(d, epoch_);
}

TEST_F(DateTest, OperatorMinusYearsZero) {
    auto d = epoch_ - std::chrono::years{0};
    EXPECT_EQ(d, epoch_);
}

// ========== C1 回归测试：operator+= / operator-= 完整覆盖 ==========

TEST_F(DateTest, OperatorPlusEqualDays) {
    Date d{2024, 1, 1};
    d += std::chrono::days{10};
    EXPECT_EQ(d, (Date{2024, 1, 11}));
    d += std::chrono::days{0};
    EXPECT_EQ(d, (Date{2024, 1, 11}));  // 加 0 天不变
    // 跨月边界
    Date eom{2024, 1, 31};
    eom += std::chrono::days{1};
    EXPECT_EQ(eom, (Date{2024, 2, 1}));
    // 跨年边界
    Date dec31{2024, 12, 31};
    dec31 += std::chrono::days{1};
    EXPECT_EQ(dec31, (Date{2025, 1, 1}));
}

TEST_F(DateTest, OperatorMinusEqualDays) {
    Date d{2024, 1, 11};
    d -= std::chrono::days{10};
    EXPECT_EQ(d, (Date{2024, 1, 1}));
    d -= std::chrono::days{0};
    EXPECT_EQ(d, (Date{2024, 1, 1}));  // 减 0 天不变
    // 跨月边界（向过去）
    Date bom{2024, 3, 1};
    bom -= std::chrono::days{1};
    EXPECT_EQ(bom, (Date{2024, 2, 29}));  // 2024 闰年
    // 跨年边界
    Date jan1{2025, 1, 1};
    jan1 -= std::chrono::days{1};
    EXPECT_EQ(jan1, (Date{2024, 12, 31}));
}

TEST_F(DateTest, OperatorPlusEqualMonths) {
    Date d{2024, 1, 15};
    d += std::chrono::months{1};
    EXPECT_EQ(d, (Date{2024, 2, 15}));
    d += std::chrono::months{0};
    EXPECT_EQ(d, (Date{2024, 2, 15}));  // 加 0 月不变
    // 月末截断：1/31 + 1 月 → 2/29（闰年）
    Date jan31{2024, 1, 31};
    jan31 += std::chrono::months{1};
    EXPECT_EQ(jan31, (Date{2024, 2, 29}));
    // 跨年
    Date dec15{2024, 12, 15};
    dec15 += std::chrono::months{1};
    EXPECT_EQ(dec15, (Date{2025, 1, 15}));
}

TEST_F(DateTest, OperatorMinusEqualMonths) {
    Date d{2024, 2, 15};
    d -= std::chrono::months{1};
    EXPECT_EQ(d, (Date{2024, 1, 15}));
    d -= std::chrono::months{0};
    EXPECT_EQ(d, (Date{2024, 1, 15}));  // 减 0 月不变
    // 月末截断：3/31 - 1 月 → 2/29（闰年）
    Date mar31{2024, 3, 31};
    mar31 -= std::chrono::months{1};
    EXPECT_EQ(mar31, (Date{2024, 2, 29}));
    // 跨年（向过去）
    Date jan15{2025, 1, 15};
    jan15 -= std::chrono::months{1};
    EXPECT_EQ(jan15, (Date{2024, 12, 15}));
}

TEST_F(DateTest, OperatorPlusEqualYears) {
    Date d{2024, 2, 29};
    d += std::chrono::years{1};
    // 闰日 + 1 年 → 非闰年 2/28
    EXPECT_EQ(d, (Date{2025, 2, 28}));
    d += std::chrono::years{0};
    EXPECT_EQ(d, (Date{2025, 2, 28}));  // 加 0 年不变
    // 闰日 + 4 年仍是闰日
    Date leap{2024, 2, 29};
    leap += std::chrono::years{4};
    EXPECT_EQ(leap, (Date{2028, 2, 29}));
    // 世纪边界：非闰年
    Date cent{1900, 2, 28};
    cent += std::chrono::years{1};
    EXPECT_EQ(cent, (Date{1901, 2, 28}));
}

TEST_F(DateTest, OperatorMinusEqualYears) {
    Date d{2025, 2, 28};
    d -= std::chrono::years{1};
    EXPECT_EQ(d, (Date{2024, 2, 28}));
    d -= std::chrono::years{0};
    EXPECT_EQ(d, (Date{2024, 2, 28}));  // 减 0 年不变
    // 闰日 - 4 年仍是闰日
    Date leap{2028, 2, 29};
    leap -= std::chrono::years{4};
    EXPECT_EQ(leap, (Date{2024, 2, 29}));
    // 跨世纪向过去
    Date cent{2001, 2, 28};
    cent -= std::chrono::years{1};
    EXPECT_EQ(cent, (Date{2000, 2, 28}));  // 2000 是闰年，但 2/28 不受影响
}

TEST_F(DateTest, OperatorCompoundAssignmentChained) {
    // 链式复合赋值：1/1 + 30天=1/31；1/31 + 1月=2/29（闰年截断）；2/29 + 1年=2/28
    Date d{2024, 1, 1};
    d += std::chrono::days{30};
    d += std::chrono::months{1};
    d += std::chrono::years{1};
    EXPECT_EQ(d, (Date{2025, 2, 28}));
}

TEST_F(DateTest, OperatorDateDifference) {
    Date a{2024, 1, 1};
    Date b{2024, 1, 11};
    EXPECT_EQ((b - a).count(), 10);
    EXPECT_EQ((a - b).count(), -10);
}

TEST_F(DateTest, ComparisonOperators) {
    Date a{2024, 1, 1};
    Date b{2024, 1, 2};
    EXPECT_EQ(a, a);
    EXPECT_NE(a, b);
    EXPECT_LT(a, b);
    EXPECT_LE(a, b);
    EXPECT_LE(a, a);
    EXPECT_GT(b, a);
    EXPECT_GE(b, a);
    EXPECT_GE(a, a);
}

TEST_F(DateTest, ToStringDefaultFormat) {
    Date d{2024, 3, 15};
    EXPECT_EQ(d.to_string(), "2024-03-15");
}

TEST_F(DateTest, ToStringCustomFormat) {
    Date d{2024, 3, 15};
    EXPECT_EQ(d.to_string("%F"), "2024-03-15");
    EXPECT_EQ(d.to_string("%D"), "03/15/24");
}

TEST_F(DateTest, ToStringStringTypeTemplate) {
    Date d{2024, 3, 15};
    auto s = d.to_string<std::string>();
    EXPECT_EQ(s, "2024-03-15");
}

TEST_F(DateTest, FromStringNormal) {
    auto d = Date::from_string("2024-03-15");
    EXPECT_EQ(d.year(), 2024);
    EXPECT_EQ(d.month(), 3);
    EXPECT_EQ(d.day(), 15);
}

TEST_F(DateTest, FromStringCustomFormat) {
    auto d = Date::from_string("03/15/24", "%D");
    EXPECT_EQ(d.year(), 2024);
    EXPECT_EQ(d.month(), 3);
    EXPECT_EQ(d.day(), 15);
}

TEST_F(DateTest, FromStringMissingComponents) {
    EXPECT_THROW(Date::from_string("2024-03", "%Y-%m"), std::invalid_argument);
    EXPECT_THROW(Date::from_string("2024", "%Y"), std::invalid_argument);
}

TEST_F(DateTest, FromStringInvalidDate) {
    EXPECT_THROW(Date::from_string("2024-02-30"), std::invalid_argument);
    EXPECT_THROW(Date::from_string("2024-13-01"), std::invalid_argument);
}

TEST_F(DateTest, FromYearMonthDayValid) {
    auto d = Date::from_year_month_day(std::chrono::year{2024},
                                        std::chrono::month{2},
                                        std::chrono::day{29});
    EXPECT_EQ(d.day(), 29);
}

TEST_F(DateTest, FromYearMonthDayInvalid) {
    EXPECT_THROW(Date::from_year_month_day(std::chrono::year{2024},
                                            std::chrono::month{2},
                                            std::chrono::day{30}),
                 std::invalid_argument);
}

TEST_F(DateTest, FromYearMonthDayInvalidParams) {
    EXPECT_THROW(Date::from_year_month_day(std::chrono::year{2024},
                                            std::chrono::month{0},
                                            std::chrono::day{1}),
                 std::invalid_argument);
    EXPECT_THROW(Date::from_year_month_day(std::chrono::year{2024},
                                            std::chrono::month{1},
                                            std::chrono::day{0}),
                 std::invalid_argument);
}

TEST_F(DateTest, FromYearMonthDayInt) {
    auto d = Date::from_year_month_day(2024, 6, 15);
    EXPECT_EQ(d.year(), 2024);
    EXPECT_EQ(d.month(), 6);
    EXPECT_EQ(d.day(), 15);
}

TEST_F(DateTest, FromWeekdayIndexed) {
    auto d = Date::from_weekday_indexed(2024, 1, 1, 1);
    EXPECT_EQ(d.year(), 2024);
    EXPECT_EQ(d.month(), 1);
    EXPECT_EQ(d.day(), 1);
    EXPECT_EQ(d.weekday(), Weekday::Monday);
}

TEST_F(DateTest, FromWeekdayIndexedInvalid) {
    EXPECT_THROW(Date::from_weekday_indexed(2024, 1, 9, 1), std::invalid_argument);
}

TEST_F(DateTest, FromYearMonthDayLast) {
    auto d = Date::from_year_month_day_last(2024, 2);
    EXPECT_EQ(d.day(), 29);
}

TEST_F(DateTest, FromYearMonthDayLastNonLeap) {
    auto d = Date::from_year_month_day_last(2023, 2);
    EXPECT_EQ(d.day(), 28);
}

TEST_F(DateTest, FromYearMonthDayLastInvalid) {
    EXPECT_THROW(Date::from_year_month_day_last(std::chrono::year{2024},
                                                  std::chrono::month{0}),
                 std::invalid_argument);
}

TEST_F(DateTest, FromYearMonthWeekdayLast) {
    auto d = Date::from_year_month_weekday_last(2024, 3, 5);
    EXPECT_EQ(d.month(), 3);
    EXPECT_EQ(d.weekday(), Weekday::Friday);
}

TEST_F(DateTest, FromYearMonthWeekdayLastInvalid) {
    EXPECT_THROW(Date::from_year_month_weekday_last(2024, 0, 5), std::invalid_argument);
}

TEST_F(DateTest, OperatorStream) {
    Date d{2024, 3, 15};
    std::ostringstream oss;
    oss << d;
    EXPECT_EQ(oss.str(), "2024-03-15");
}

TEST_F(DateTest, DISABLED_YearRange1980To2100) {
    auto start = std::chrono::sys_days{std::chrono::year_month_day{
        std::chrono::year{1980}, std::chrono::month{1}, std::chrono::day{1}}};
    auto end = std::chrono::sys_days{std::chrono::year_month_day{
        std::chrono::year{2100}, std::chrono::month{12}, std::chrono::day{31}}};

    for (auto dp = start; dp <= end; dp += std::chrono::days{1}) {
        Date d{std::chrono::year_month_day{dp}};
        auto ymd = d.year_month_day();
        Date d2(ymd);
        EXPECT_EQ(d, d2) << "Round-trip failed for " << d.to_string();

        auto chrono_wd = std::chrono::weekday{dp};
        EXPECT_EQ(d.weekday(), static_cast<Weekday>(chrono_wd.iso_encoding()))
            << "Weekday mismatch for " << d.to_string();

        Date next = d.add_days(1);
        EXPECT_EQ(d.days_to(next), 1) << "days_to(next) != 1 for " << d.to_string();
    }
}

TEST_F(DateTest, DISABLED_MonthEndTruncationExhaustive) {
    for (int32_t y = 1980; y <= 2100; ++y) {
        for (int32_t m = 1; m <= 12; ++m) {
            for (int32_t day = 28; day <= 31; ++day) {
                auto ymd = std::chrono::year_month_day{
                    std::chrono::year{y}, std::chrono::month{static_cast<uint32_t>(m)},
                    std::chrono::day{static_cast<uint32_t>(day)}};
                if (!ymd.ok()) continue;

                Date d{ymd};
                for (int32_t delta = -24; delta <= 24; ++delta) {
                    auto result = d.add_months(delta);
                    auto result_ymd = result.year_month_day();
                    EXPECT_TRUE(result_ymd.ok())
                        << "Invalid date from " << y << "-" << m << "-" << day
                        << " + " << delta << " months = " << result.to_string();

                    auto last_day = Date::from_year_month_day_last(
                        result.year(), result.month());
                    EXPECT_LE(result.day(), last_day.day())
                        << "Day exceeds month end from " << y << "-" << m << "-" << day
                        << " + " << delta << " months";
                }
            }
        }
    }
}
