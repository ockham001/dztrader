#include "trading_day.h"

#include <format>

#include <dztrader/date_time/date_time.h>

namespace dztrader::strategy_api_internal {

std::string format_trading_day(DzDate days) {
    if (days <= DZ_DATE_NA) {
        return "";
    }
    try {
        const dztrader::Date date{days};
        return std::format("{:04d}{:02d}{:02d}", date.year(), date.month(), date.day());
    } catch (...) {
        return "";
    }
}

DzDate parse_trading_day(std::string_view text) noexcept {
    if (text.size() != 8) {
        return DZ_DATE_NA;
    }
    try {
        const int32_t year = std::stoi(std::string(text.substr(0, 4)));
        const int32_t month = std::stoi(std::string(text.substr(4, 2)));
        const int32_t day = std::stoi(std::string(text.substr(6, 2)));
        return dztrader::Date::from_year_month_day(year, month, day).days_since_epoch();
    } catch (...) {
        return DZ_DATE_NA;
    }
}

}  // namespace dztrader::strategy_api_internal
