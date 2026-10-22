#ifndef DZTRADER_STRATEGY_API_TESTS_BAR_TEST_DATA_H_
#define DZTRADER_STRATEGY_API_TESTS_BAR_TEST_DATA_H_

#include <dztrader/bar/types.h>

#include <cstdint>

namespace dztrader::strategy_api::test {

/// @brief 09:00 的交易日内秒
inline constexpr int32_t TSEC_09_00 = 54000;
/// @brief 15:00 的交易日内秒
inline constexpr int32_t TSEC_15_00 = 75600;
/// @brief 夜盘小节（21:00）的交易日内秒
inline constexpr int32_t TSEC_21_00 = 10800;
/// @brief 夜盘小节结束（次日 02:30）的交易日内秒
inline constexpr int32_t TSEC_02_30 = 30600;

/// @brief 单小节当日盘表（09:00–15:00），tick→bar 测试的公共合法表
inline constexpr dztrader::bar::TradingSessionTable day_session_table() {
    dztrader::bar::TradingSessionTable table{};
    table.session_count = 1;
    table.sessions[0] = dztrader::bar::TradingSession{TSEC_09_00, TSEC_15_00};
    return table;
}

}  // namespace dztrader::strategy_api::test

#endif  // DZTRADER_STRATEGY_API_TESTS_BAR_TEST_DATA_H_
