#ifndef DZTRADER_STRATEGY_API_TESTS_DATE_RANGE_H_
#define DZTRADER_STRATEGY_API_TESTS_DATE_RANGE_H_

#include <chrono>

namespace dztrader::strategy_api::test {

/// @brief 穷举/抽样用例统一使用的 1980–2100 日期范围起点。
inline std::chrono::sys_days range_start_1980() {
    return std::chrono::sys_days{std::chrono::year_month_day{
        std::chrono::year{1980}, std::chrono::month{1}, std::chrono::day{1}}};
}

/// @brief 穷举/抽样用例统一使用的 1980–2100 日期范围终点。
inline std::chrono::sys_days range_end_2100() {
    return std::chrono::sys_days{std::chrono::year_month_day{
        std::chrono::year{2100}, std::chrono::month{12}, std::chrono::day{31}}};
}

}  // namespace dztrader::strategy_api::test

#endif  // DZTRADER_STRATEGY_API_TESTS_DATE_RANGE_H_
