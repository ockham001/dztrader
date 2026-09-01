#include "td/td_prescan.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <SQLiteCpp/Statement.h>

#include <dztrader/date_time/date.h>

namespace dztrader::ctp {

namespace {

/// 安全拷贝 std::string 到定长 char 数组 (截断保护, 保持尾 null).
template <size_t N>
void copy_to_fixed(char (&dest)[N], const std::string& src) {
    const size_t n = std::min(src.size(), N - 1);
    std::memcpy(dest, src.data(), n);
    dest[n] = '\0';
}

/// "YYYYMMDD" 文本 -> DzDate (距纪元天数). 非法回落 0.
int32_t parse_yyyymmdd_to_epoch(const std::string& text) {
    if (text.size() != 8) {
        return 0;
    }
    try {
        const int32_t y = std::stoi(text.substr(0, 4));
        const int32_t m = std::stoi(text.substr(4, 2));
        const int32_t d = std::stoi(text.substr(6, 2));
        return dztrader::Date::from_year_month_day(y, m, d).days_since_epoch();
    } catch (...) {
        return 0;
    }
}

/// 通用查询模板: 按 account_id 过滤 (可选 seq > since_seq 增量).
/// 列按写端 bind_order 语义读 (direction/status/price_type/position_effect 为 INTEGER).
template <bool Since>
std::vector<OrderRecord> load_orders_impl(SQLite::Database& db, const std::string& account_id,
                                          uint64_t since_seq) {
    std::vector<OrderRecord> out;
    std::string sql =
        "SELECT trading_day, order_id, order_ref, external_order_id, is_external,"
        " instrument_id, exchange_id, direction, position_effect, price_type, status,"
        " price, volume, volume_traded, volume_canceled, insert_time, update_time,"
        " error_id, error_msg, strategy_id, remark, seq"
        " FROM orders WHERE account_id = ?";
    if constexpr (Since) {
        sql += " AND seq > ?";
    }
    sql += " ORDER BY seq ASC";
    SQLite::Statement stmt(db, sql);
    stmt.bind(1, account_id);
    if constexpr (Since) {
        stmt.bind(2, static_cast<int64_t>(since_seq));
    }
    while (stmt.executeStep()) {
        OrderRecord r{};
        copy_to_fixed(r.base.account_id, account_id);
        std::string day = stmt.getColumn(0).getString();
        copy_to_fixed(r.trading_day, day);
        r.base.order_id = stmt.getColumn(1).getInt64();
        copy_to_fixed(r.order_ref, stmt.getColumn(2).getString());
        copy_to_fixed(r.external_order_id, stmt.getColumn(3).getString());
        r.is_external = static_cast<int8_t>(stmt.getColumn(4).getInt());
        copy_to_fixed(r.base.instrument_id, stmt.getColumn(5).getString());
        copy_to_fixed(r.base.exchange_id, stmt.getColumn(6).getString());
        r.base.direction = static_cast<int8_t>(stmt.getColumn(7).getInt());  // INTEGER 读 (Task 3 注)
        r.base.position_effect = static_cast<int8_t>(stmt.getColumn(8).getInt());
        r.base.price_type = static_cast<int8_t>(stmt.getColumn(9).getInt());
        r.base.status = static_cast<int8_t>(stmt.getColumn(10).getInt());
        r.base.price = stmt.getColumn(11).getDouble();
        r.base.volume = stmt.getColumn(12).getInt();
        r.base.volume_traded = stmt.getColumn(13).getInt();
        r.volume_canceled = stmt.getColumn(14).getInt();
        r.insert_time = stmt.getColumn(15).getInt64();
        r.update_time = stmt.getColumn(16).getInt64();
        r.error_id = stmt.getColumn(17).getInt();
        copy_to_fixed(r.error_msg, stmt.getColumn(18).getString());
        copy_to_fixed(r.base.strategy_id, stmt.getColumn(19).getString());
        copy_to_fixed(r.base.remark, stmt.getColumn(20).getString());
        r.base.seq = static_cast<uint64_t>(stmt.getColumn(21).getInt64());
        r.base.date = parse_yyyymmdd_to_epoch(day);
        out.push_back(r);
    }
    return out;
}

template <bool Since>
std::vector<TradeRecord> load_trades_impl(SQLite::Database& db, const std::string& account_id,
                                          uint64_t since_seq) {
    std::vector<TradeRecord> out;
    std::string sql =
        "SELECT trading_day, trade_id, order_id, instrument_id, exchange_id,"
        " direction, position_effect, price, volume, trade_time, trade_date,"
        " commission, strategy_id, seq"
        " FROM trades WHERE account_id = ?";
    if constexpr (Since) {
        sql += " AND seq > ?";
    }
    sql += " ORDER BY seq ASC";
    SQLite::Statement stmt(db, sql);
    stmt.bind(1, account_id);
    if constexpr (Since) {
        stmt.bind(2, static_cast<int64_t>(since_seq));
    }
    while (stmt.executeStep()) {
        TradeRecord r{};
        copy_to_fixed(r.base.account_id, account_id);
        std::string day = stmt.getColumn(0).getString();
        copy_to_fixed(r.trading_day, day);
        copy_to_fixed(r.base.trade_id, stmt.getColumn(1).getString());
        r.base.order_id = stmt.getColumn(2).getInt64();
        copy_to_fixed(r.base.instrument_id, stmt.getColumn(3).getString());
        copy_to_fixed(r.base.exchange_id, stmt.getColumn(4).getString());
        r.base.direction = static_cast<int8_t>(stmt.getColumn(5).getInt());
        r.base.position_effect = static_cast<int8_t>(stmt.getColumn(6).getInt());
        r.base.price = stmt.getColumn(7).getDouble();
        r.base.volume = stmt.getColumn(8).getInt();
        r.trade_time = stmt.getColumn(9).getInt64();
        r.trade_date = stmt.getColumn(10).getInt64();
        r.commission = stmt.getColumn(11).getDouble();
        copy_to_fixed(r.base.strategy_id, stmt.getColumn(12).getString());
        r.base.seq = static_cast<uint64_t>(stmt.getColumn(13).getInt64());
        r.base.date = parse_yyyymmdd_to_epoch(day);
        out.push_back(r);
    }
    return out;
}

}  // namespace

uint64_t query_max_seq(SQLite::Database& db, const std::string& account_id) {
    constexpr const char* kTables[] = {"orders", "trades", "positions", "trading_accounts"};
    uint64_t result = 0;
    for (const char* table : kTables) {
        SQLite::Statement stmt(
            db, std::string("SELECT COALESCE(MAX(seq), 0) FROM ") + table + " WHERE account_id = ?");
        stmt.bind(1, account_id);
        if (stmt.executeStep()) {
            const int64_t v = stmt.getColumn(0).getInt64();
            if (v > 0) {
                result = std::max(result, static_cast<uint64_t>(v));
            }
        }
    }
    return result;
}

std::vector<OrderRecord> load_orders(SQLite::Database& db, const std::string& account_id) {
    return load_orders_impl<false>(db, account_id, 0);
}

std::vector<TradeRecord> load_trades(SQLite::Database& db, const std::string& account_id) {
    return load_trades_impl<false>(db, account_id, 0);
}

std::vector<OrderRecord> load_orders_since(SQLite::Database& db, const std::string& account_id,
                                           uint64_t since_seq) {
    return load_orders_impl<true>(db, account_id, since_seq);
}

std::vector<TradeRecord> load_trades_since(SQLite::Database& db, const std::string& account_id,
                                           uint64_t since_seq) {
    return load_trades_impl<true>(db, account_id, since_seq);
}

std::map<std::string, SessionBootData> prescan_accounts(
    SQLite::Database& db, const std::vector<std::string>& accounts) {
    std::map<std::string, SessionBootData> result;
    for (const auto& acct : accounts) {
        SessionBootData boot;
        boot.start_seq = query_max_seq(db, acct);
        boot.orders = load_orders(db, acct);
        boot.trades = load_trades(db, acct);
        result.emplace(acct, std::move(boot));
    }
    return result;
}

}  // namespace dztrader::ctp
