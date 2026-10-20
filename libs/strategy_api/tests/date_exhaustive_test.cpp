#include "date_exhaustive_cases.h"

#include <chrono>

#include <gtest/gtest.h>

using namespace dztrader;
using namespace dztrader::strategy_api::test;

// slow 档：ctest -L slow（fast 门禁的抽样版见 date_test.cpp）
TEST(DateExhaustiveTest, YearRange1980To2100) {
    check_date_year_range(range_start_1980(), range_end_2100(), std::chrono::days{1});
}

TEST(DateExhaustiveTest, MonthEndTruncationExhaustive) {
    check_date_month_end_truncation(1980, 2100, 1, -24, 24, 1);
}
