#include "common/trading_calendar.h"

namespace dztrader::ctp {

bool is_trading_window(int weekday, std::string_view hh_mm) {
    // 允许时段: 周一 06:00 ~ 周六 05:59 (ISO 8601: 1=周一...7=周日)
    if (weekday == 1 && hh_mm >= TRADING_WINDOW_START) {
        return true;  // 周一 06:00 起 (含)
    }
    if (weekday >= 2 && weekday <= 5) {
        return true;  // 周二~周五全天
    }
    if (weekday == 6 && hh_mm < TRADING_WINDOW_END) {
        return true;  // 周六 06:00 之前 (不含 06:00)
    }
    return false;  // 周日(7)、周一 00:00-05:59、周六 06:00 及之后
}

}  // namespace dztrader::ctp
