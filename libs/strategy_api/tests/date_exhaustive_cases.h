#ifndef DZTRADER_STRATEGY_API_TESTS_DATE_EXHAUSTIVE_CASES_H_
#define DZTRADER_STRATEGY_API_TESTS_DATE_EXHAUSTIVE_CASES_H_

#include "date_range.h"

#include <dztrader/date_time/date.h>

#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

namespace dztrader::strategy_api::test {

/**
 * @brief 1980–2100 日期 round-trip / 星期 / add_days 检查。
 *
 * 断言本体与穷举版一致，由调用方通过 step 控制抽样密度：fast 门禁用较大步长，
 * slow 档用 days{1} 跑全量。
 */
inline void check_date_year_range(std::chrono::sys_days start,
                                  std::chrono::sys_days end,
                                  std::chrono::days step) {
    for (auto dp = start; dp <= end; dp += step) {
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

/**
 * @brief 月末截断穷举。
 *
 * 年份与月份偏移都可抽样；fast 门禁用较大步长，slow 档两者步长均为 1。
 */
inline void check_date_month_end_truncation(int32_t year_start,
                                            int32_t year_end,
                                            int32_t year_step,
                                            int32_t delta_start,
                                            int32_t delta_end,
                                            int32_t delta_step) {
    for (int32_t y = year_start; y <= year_end; y += year_step) {
        for (int32_t m = 1; m <= 12; ++m) {
            for (int32_t day = 28; day <= 31; ++day) {
                auto ymd = std::chrono::year_month_day{
                    std::chrono::year{y}, std::chrono::month{static_cast<uint32_t>(m)},
                    std::chrono::day{static_cast<uint32_t>(day)}};
                if (!ymd.ok()) continue;

                Date d{ymd};
                for (int32_t delta = delta_start; delta <= delta_end; delta += delta_step) {
                    auto result = d.add_months(delta);
                    auto result_ymd = result.year_month_day();
                    EXPECT_TRUE(result_ymd.ok())
                        << "Invalid date from " << y << "-" << m << "-" << day << " + " << delta
                        << " months = " << result.to_string();

                    auto last_day = Date::from_year_month_day_last(result.year(), result.month());
                    EXPECT_LE(result.day(), last_day.day())
                        << "Day exceeds month end from " << y << "-" << m << "-" << day << " + "
                        << delta << " months";
                }
            }
        }
    }
}

}  // namespace dztrader::strategy_api::test

#endif  // DZTRADER_STRATEGY_API_TESTS_DATE_EXHAUSTIVE_CASES_H_
