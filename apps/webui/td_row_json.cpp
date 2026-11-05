#include "td_row_json.h"

#include "td_list_query.h"

#include <dztrader/data_type.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace dztrader::webui {

namespace {

/// 枚举 → 契约 rest §2.9.3 的 PascalCase 字符串。
/// 未知取值（DB 被手工改写等）不抛错，按零值语义输出并记 DEBUG
/// （页面不能因为一行脏数据整页失败）。
std::string direction_text(int value) {
    switch (value) {
        case DZ_DIRECTION_LONG:
            return "Long";
        case DZ_DIRECTION_NET:
            return "Net";
        case DZ_DIRECTION_SHORT:
            return "Short";
        default:
            SPDLOG_DEBUG("unknown td direction | value={}", value);
            return "Net";
    }
}

std::string position_effect_text(int value) {
    switch (value) {
        case DZ_POSITION_EFFECT_AUTO:
            return "Auto";
        case DZ_POSITION_EFFECT_OPEN:
            return "Open";
        case DZ_POSITION_EFFECT_CLOSE:
            return "Close";
        case DZ_POSITION_EFFECT_CLOSE_TODAY:
            return "CloseToday";
        case DZ_POSITION_EFFECT_CLOSE_YESTDAY:
            return "CloseYesterday";
        default:
            SPDLOG_DEBUG("unknown td position effect | value={}", value);
            return "Auto";
    }
}

std::string price_type_text(int value) {
    switch (value) {
        case DZ_PRICE_LIMIT:
            return "Limit";
        case DZ_PRICE_MARKET:
            return "Market";
        case DZ_PRICE_STOP:
            return "Stop";
        case DZ_PRICE_FAK:
            return "Fak";
        case DZ_PRICE_FOK:
            return "Fok";
        case DZ_PRICE_RFQ:
            return "Rfq";
        default:
            SPDLOG_DEBUG("unknown td price type | value={}", value);
            return "Limit";
    }
}

std::string status_text(int value) {
    switch (value) {
        case DZ_ORDER_SUBMITTING:
            return "Submitting";
        case DZ_ORDER_NOT_TRADED:
            return "NotTraded";
        case DZ_ORDER_PART_TRADED:
            return "PartTraded";
        case DZ_ORDER_ALL_TRADED:
            return "AllTraded";
        case DZ_ORDER_CANCELLED:
            return "Cancelled";
        case DZ_ORDER_REJECTED:
            return "Rejected";
        default:
            SPDLOG_DEBUG("unknown td order status | value={}", value);
            return "Submitting";
    }
}

/// 最小整数 d ∈ [0, 8] 使 `value × 10^d` 与最近整数之差 ≤ 1e-9（契约 rest §2.9.6 的
/// 浮点安全口径，不做字符串猜测）；无法确定返回 nullopt。仅用于 `price_tick`（量级小，
/// 绝对容差成立）。
std::optional<int> tick_decimals(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return std::nullopt;
    }
    double scaled = value;
    for (int decimals = 0; decimals <= 8; ++decimals) {
        const double nearest = std::round(scaled);
        if (std::abs(scaled - nearest) <= 1e-9) {
            return decimals;
        }
        scaled *= 10.0;
    }
    return std::nullopt;
}

/// 价格的最短十进制表示的小数位数（契约 rest §2.9.6 的兜底口径）。
/// 用 `std::to_chars` 的最短往返表示（如 3512.35 → "3512.35"、123456.789 → "123456.789"），
/// 而不是对 `value × 10^d` 做绝对容差比较——后者在价格量级大时会因浮点尾差误判。
std::optional<int> shortest_decimals(double value) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }
    const std::string_view text(buffer.data(), static_cast<size_t>(result.ptr - buffer.data()));
    // 计数小数点后的位数；科学计数法（如 "1e+06"）需扣除正指数。
    const auto exponent_pos = text.find_first_of("eE");
    const std::string_view mantissa = text.substr(0, exponent_pos);
    int exponent = 0;
    if (exponent_pos != std::string_view::npos) {
        const std::string_view exponent_text = text.substr(exponent_pos + 1);
        const auto parsed = std::from_chars(exponent_text.data(),
                                            exponent_text.data() + exponent_text.size(), exponent);
        if (parsed.ec != std::errc{}) {
            return std::nullopt;
        }
    }
    const auto dot = mantissa.find('.');
    int fraction_digits = 0;
    if (dot != std::string_view::npos) {
        fraction_digits = static_cast<int>(mantissa.size() - dot - 1);
    }
    const int decimals = std::max(0, fraction_digits - exponent);
    if (decimals > 8) {
        return std::nullopt;  // 契约范围 [0, 8]；超出按「仍无法确定」处理
    }
    return decimals;
}

}  // namespace

nlohmann::json td_order_row_json(const TdOrderRecord& row, int price_decimals) {
    return {
        {"account_id", row.account_id},
        {"date", row.date},
        {"order_id", row.order_id},
        {"strategy_id", row.strategy_id},
        {"instrument_id", row.instrument_id},
        {"exchange_id", row.exchange_id},
        {"direction", direction_text(row.direction)},
        {"position_effect", position_effect_text(row.position_effect)},
        {"price_type", price_type_text(row.price_type)},
        {"status", status_text(row.status)},
        {"price", row.price},
        {"volume", row.volume},
        {"volume_traded", row.volume_traded},
        {"remark", row.remark},
        {"time", row.time},
        {"avg_price",
         row.volume_traded == 0 ? nlohmann::json(nullptr) : nlohmann::json(row.avg_price)},
        {"price_decimals", price_decimals},
        {"avg_price_decimals", td_avg_price_decimals(price_decimals)},
        {"seq", row.seq},
        {"sort_key", td_order_sort_key(row.time, row.order_id)},
    };
}

nlohmann::json td_trade_row_json(const TdTradeRecord& row, int price_decimals) {
    return {
        {"account_id", row.account_id},
        {"date", row.date},
        {"trade_id", row.trade_id},
        {"order_id", row.order_id},
        {"strategy_id", row.strategy_id},
        {"instrument_id", row.instrument_id},
        {"exchange_id", row.exchange_id},
        {"direction", direction_text(row.direction)},
        {"position_effect", position_effect_text(row.position_effect)},
        {"price", row.price},
        {"volume", row.volume},
        {"time", row.time},
        {"price_decimals", price_decimals},
        {"seq", row.seq},
        {"sort_key", td_trade_sort_key(row.time, row.account_id, row.exchange_id, row.trade_id)},
    };
}

int td_price_decimals(double price_tick, double price) {
    if (const auto from_tick = tick_decimals(price_tick); from_tick.has_value()) {
        return *from_tick;
    }
    if (const auto from_price = shortest_decimals(price); from_price.has_value()) {
        return *from_price;
    }
    return 0;
}

int td_avg_price_decimals(int price_decimals) { return std::clamp(price_decimals + 2, 2, 6); }

std::string td_order_sort_key(int32_t time, int64_t order_id) {
    return std::format("{:05d}-{:020d}", td_time_anchor(time), order_id);
}

std::string td_trade_sort_key(int32_t time,
                              const std::string& account_id,
                              const std::string& exchange_id,
                              const std::string& trade_id) {
    return std::format("{:05d}-{}-{}-{}", td_time_anchor(time), td_hex_encode(account_id),
                       td_hex_encode(exchange_id), td_hex_encode(trade_id));
}

}  // namespace dztrader::webui
