#include "date_time_exhaustive_cases.h"

#include <chrono>

#include <gtest/gtest.h>

using namespace dztrader;
using namespace dztrader::strategy_api::test;

// slow 档：ctest -L slow（fast 门禁的抽样版见 date_time_test.cpp）
TEST(DateTimeExhaustiveTest, YearRange1980To2100) {
    check_date_time_year_range(range_start_1980(), range_end_2100(), std::chrono::days{1});
}

TEST(DateTimeExhaustiveTest, NightSessionExhaustive) {
    check_night_session(range_start_1980(), range_end_2100(), std::chrono::days{1});
}

TEST(DateTimeExhaustiveTest, MarketDataSimulation) { check_market_data_simulation(1000000); }

TEST(DateTimeExhaustiveTest, TimezoneRoundTrip) { check_timezone_round_trip(1980, 2100, 1); }
