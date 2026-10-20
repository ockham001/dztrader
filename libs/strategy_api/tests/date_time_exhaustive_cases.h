#ifndef DZTRADER_STRATEGY_API_TESTS_DATE_TIME_EXHAUSTIVE_CASES_H_
#define DZTRADER_STRATEGY_API_TESTS_DATE_TIME_EXHAUSTIVE_CASES_H_

#include "date_range.h"

#include <dztrader/date_time/date_time.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <gtest/gtest.h>

namespace dztrader::strategy_api::test {

/**
 * @brief 1980–2100 DateTime 字段一致性检查。
 *
 * 断言本体与穷举版一致，由调用方通过 step 控制抽样密度。
 */
inline void check_date_time_year_range(std::chrono::sys_days start,
                                       std::chrono::sys_days end,
                                       std::chrono::days step) {
    for (auto dp = start; dp <= end; dp += step) {
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

/**
 * @brief action day ↔ trading day 夜盘 round-trip。
 *
 * 只覆盖有效 action 时段：周一到周五（周一 0–6 属于上周六夜盘，反向映射回周六），
 * 以及周六 0–6。周六 6 点后与周日不是有效 action 时段，跳过。
 */
inline void check_night_session(std::chrono::sys_days start,
                                std::chrono::sys_days end,
                                std::chrono::days step) {
    for (auto dp = start; dp <= end; dp += step) {
        Date d{std::chrono::year_month_day{dp}};
        auto wd = d.weekday();
        for (int32_t h = 0; h < 24; ++h) {
            if (wd == Weekday::Sunday) continue;
            if (wd == Weekday::Saturday && h >= 6) continue;
            if (wd == Weekday::Monday && h < 6) continue;

            DateTime action{d, Time{h, 0, 0}};
            auto trading = DateTime::action_day_to_trading_day(action);
            auto action2 = DateTime::trading_day_to_action_day(trading);
            EXPECT_EQ(action.date(), action2.date())
                << "Round-trip failed: " << d.to_string() << " h=" << h;
        }
    }
}

/**
 * @brief 模拟随机 tick 的时间戳/字符串 round-trip 与差值一致性。
 *
 * @param tick_count 迭代次数；fast 门禁用较小值，slow 档跑 1,000,000。
 */
inline void check_market_data_simulation(int64_t tick_count) {
    constexpr int64_t START_TS = 1704067200;
    constexpr int64_t END_TS = 1735689600;

    std::srand(42);
    for (int64_t i = 0; i < tick_count; ++i) {
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
            EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(diff).count(),
                      dt3.seconds_since_epoch() - dt.seconds_since_epoch());
        }
    }
}

/**
 * @brief 系统时间 ↔ 本地时间 round-trip。
 *
 * 年份可按步长抽样；fast 门禁用较大步长，slow 档逐年覆盖 1980–2100。
 */
inline void check_timezone_round_trip(int32_t year_start, int32_t year_end, int32_t year_step) {
    for (int32_t y = year_start; y <= year_end; y += year_step) {
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

}  // namespace dztrader::strategy_api::test

#endif  // DZTRADER_STRATEGY_API_TESTS_DATE_TIME_EXHAUSTIVE_CASES_H_
