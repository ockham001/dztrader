#include "td/td_day_load.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SQLiteCpp/Statement.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <dztrader/data_type.h>
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

/// ext JSON（schema v1 的柜台扩展列）-> OrderRecord 的 CTP 扩展字段。
/// 登录重放匹配/重放过滤所需的 order_ref、update_time、volume_canceled 等在此恢复；
/// 坏 JSON 按空 ext 处理（不阻塞装载）。
void fill_order_ext(OrderRecord& record, const std::string& ext_text) {
    if (ext_text.empty()) {
        return;
    }
    try {
        const nlohmann::json ext = nlohmann::json::parse(ext_text);
        if (!ext.is_object()) {
            return;
        }
        if (const auto it = ext.find(kOrderExtOrderRef); it != ext.end() && it->is_string()) {
            copy_to_fixed(record.order_ref, it->get<std::string>());
        }
        if (const auto it = ext.find(kOrderExtOrderSysId); it != ext.end() && it->is_string()) {
            copy_to_fixed(record.external_order_id, it->get<std::string>());
        }
        if (const auto it = ext.find(kOrderExtVolumeCanceled); it != ext.end() && it->is_number()) {
            record.volume_canceled = it->get<int32_t>();
        }
        if (const auto it = ext.find(kOrderExtUpdateTime); it != ext.end() && it->is_number()) {
            record.update_time = it->get<int64_t>();
        }
        if (const auto it = ext.find(kOrderExtErrorId); it != ext.end() && it->is_number()) {
            record.error_id = it->get<int32_t>();
        }
        if (const auto it = ext.find(kOrderExtErrorMessage); it != ext.end() && it->is_string()) {
            copy_to_fixed(record.error_msg, it->get<std::string>());
        }
    } catch (const std::exception&) {
        // 坏 ext: 视为无扩展字段
    }
}

/// 当日 orders 装载 (schema v1 列序, date 过滤, 行序 = seq 升序).
std::vector<OrderRecord> load_day_orders(SQLite::Database& db,
                                         const std::string& account_id,
                                         const std::string& day) {
    std::vector<OrderRecord> out;
    SQLite::Statement stmt(
        db,
        "SELECT date, order_id, strategy_id, instrument_id, exchange_id, direction,"
        " position_effect, price_type, status, price, volume, volume_traded, remark,"
        " time, avg_price, seq, ext FROM orders WHERE account_id = ? AND date = ?"
        " ORDER BY seq ASC");
    stmt.bind(1, account_id);
    stmt.bind(2, day);
    while (stmt.executeStep()) {
        OrderRecord r{};
        copy_to_fixed(r.base.account_id, account_id);
        const std::string row_day = stmt.getColumn(0).getString();
        copy_to_fixed(r.trading_day, row_day);
        r.base.order_id = stmt.getColumn(1).getInt64();
        copy_to_fixed(r.base.strategy_id, stmt.getColumn(2).getString());
        copy_to_fixed(r.base.instrument_id, stmt.getColumn(3).getString());
        copy_to_fixed(r.base.exchange_id, stmt.getColumn(4).getString());
        r.base.direction = static_cast<int8_t>(stmt.getColumn(5).getInt());
        r.base.position_effect = static_cast<int8_t>(stmt.getColumn(6).getInt());
        r.base.price_type = static_cast<int8_t>(stmt.getColumn(7).getInt());
        r.base.status = static_cast<int8_t>(stmt.getColumn(8).getInt());
        r.base.price = stmt.getColumn(9).getDouble();
        r.base.volume = stmt.getColumn(10).getInt();
        r.base.volume_traded = stmt.getColumn(11).getInt();
        copy_to_fixed(r.base.remark, stmt.getColumn(12).getString());
        r.base.time = stmt.getColumn(13).getInt();
        r.base.avg_price = stmt.getColumn(14).getDouble();
        r.base.seq = static_cast<uint64_t>(stmt.getColumn(15).getInt64());
        fill_order_ext(r, stmt.getColumn(16).getString());
        r.is_external = (std::string_view(r.base.strategy_id) == DZ_STRATEGY_ID_EXTERNAL) ? 1 : 0;
        r.base.date = parse_yyyymmdd_to_epoch(row_day);
        out.push_back(r);
    }
    return out;
}

/// 当日 trades 装载 (schema v1 列序, date 过滤, 行序 = seq 升序).
std::vector<TradeRecord> load_day_trades(SQLite::Database& db,
                                         const std::string& account_id,
                                         const std::string& day) {
    std::vector<TradeRecord> out;
    SQLite::Statement stmt(
        db,
        "SELECT date, trade_id, order_id, strategy_id, instrument_id, exchange_id,"
        " direction, position_effect, price, volume, time, seq FROM trades"
        " WHERE account_id = ? AND date = ? ORDER BY seq ASC");
    stmt.bind(1, account_id);
    stmt.bind(2, day);
    while (stmt.executeStep()) {
        TradeRecord r{};
        copy_to_fixed(r.base.account_id, account_id);
        const std::string row_day = stmt.getColumn(0).getString();
        copy_to_fixed(r.trading_day, row_day);
        copy_to_fixed(r.base.trade_id, stmt.getColumn(1).getString());
        r.base.order_id = stmt.getColumn(2).getInt64();
        copy_to_fixed(r.base.strategy_id, stmt.getColumn(3).getString());
        copy_to_fixed(r.base.instrument_id, stmt.getColumn(4).getString());
        copy_to_fixed(r.base.exchange_id, stmt.getColumn(5).getString());
        r.base.direction = static_cast<int8_t>(stmt.getColumn(6).getInt());
        r.base.position_effect = static_cast<int8_t>(stmt.getColumn(7).getInt());
        r.base.price = stmt.getColumn(8).getDouble();
        r.base.volume = stmt.getColumn(9).getInt();
        r.base.time = stmt.getColumn(10).getInt();
        r.base.seq = static_cast<uint64_t>(stmt.getColumn(11).getInt64());
        r.base.date = parse_yyyymmdd_to_epoch(row_day);
        out.push_back(r);
    }
    return out;
}

}  // namespace

// ============================================================================
// OrderTradeAvgTracker: 每单成交累计 (Σ价×量 / Σ量, 契约 td-data-sync §3)
// ============================================================================

void OrderTradeAvgTracker::seed(const std::vector<TradeRecord>& trades) {
    // 只在 order_id 维度聚合: 逐笔成交记录本身不保留 (登录日只在库中)。
    for (const auto& t : trades) {
        add_trade(t.base.order_id, t.base.price, t.base.volume);
    }
}

void OrderTradeAvgTracker::add_trade(int64_t order_id, double price, int32_t volume) {
    // 非有限价与 volume<=0 不入累计 (脏数据不污染均价; 无成交语义保持 nullopt)。
    if (volume <= 0 || !std::isfinite(price)) {
        return;
    }
    Acc& acc = accs_[order_id];
    acc.price_volume += price * static_cast<double>(volume);
    acc.volume += static_cast<int64_t>(volume);
}

std::optional<double> OrderTradeAvgTracker::avg_price(int64_t order_id) const {
    const auto it = accs_.find(order_id);
    if (it == accs_.end() || it->second.volume <= 0) {
        return std::nullopt;
    }
    return it->second.price_volume / static_cast<double>(it->second.volume);
}

int64_t OrderTradeAvgTracker::volume(int64_t order_id) const {
    const auto it = accs_.find(order_id);
    return it == accs_.end() ? 0 : it->second.volume;
}

OrderRecord order_record_with_avg(const OrderRecord& base, double avg_price) {
    // 只改均价派生列: update_time / volume_canceled / status 等对比字段保持基准值,
    // 否则刷新行会破坏重放去重基准 (契约 §3, 重放过滤器对比集)。
    OrderRecord out = base;
    out.base.avg_price = avg_price;
    return out;
}

std::string format_day_text(int32_t day) {
    if (day <= 0) {
        return "00000000";
    }
    try {
        const dztrader::Date d{day};
        return std::format("{:04d}{:02d}{:02d}", d.year(), d.month(), d.day());
    } catch (...) {
        return "00000000";
    }
}

uint64_t query_max_seq(SQLite::Database& db, const std::string& account_id) {
    constexpr const char* kTables[] = {"orders", "trades", "positions", "trading_accounts"};
    uint64_t result = 0;
    for (const char* table : kTables) {
        SQLite::Statement stmt(db, std::string("SELECT COALESCE(MAX(seq), 0) FROM ") + table +
                                       " WHERE account_id = ?");
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

DayLoadData load_trading_day(SQLite::Database& db,
                             const std::string& account_id,
                             const std::string& day) {
    DayLoadData data;
    data.start_seq = query_max_seq(db, account_id);
    data.orders = load_day_orders(db, account_id, day);
    data.trades = load_day_trades(db, account_id, day);
    // 均价基准重建 (契约 §3): 当日成交按 order_id 聚合, 并用聚合均价覆盖装载委托的
    // avg_price; 当日无成交的委托保留 DB 值 (不被清零, 跨日 GFD 单昨日成交段不回填)。
    data.order_avgs.seed(data.trades);
    for (auto& order : data.orders) {
        const auto avg = data.order_avgs.avg_price(order.base.order_id);
        if (avg.has_value()) {
            order.base.avg_price = *avg;
        }
    }
    return data;
}

void AccountSeqAllocator::set_db_provider(std::function<SQLite::Database*()> provider) {
    db_provider_ = std::move(provider);
}

uint64_t AccountSeqAllocator::next(const std::string& account_id) {
    // 每次分配都取 max(已分配值, DB MAX(seq)) 作为基准再递增:
    // - 已分配值只增不减 (DB 未提交的行不得让计数回退);
    // - 同时吸收「同账户会话在无会话拒单之后又推进了 seq」的进展 (DB 已提交),
    //   否则会话断线后本分配器会从旧种子续号, 与既有行撞 seq.
    uint64_t base = 0;
    const auto it = allocated_.find(account_id);
    if (it != allocated_.end()) {
        base = it->second;
    }
    if (db_provider_) {
        SQLite::Database* db = db_provider_();
        if (db != nullptr) {
            try {
                base = std::max(base, query_max_seq(*db, account_id));
            } catch (const std::exception& e) {
                SPDLOG_WARN(
                    "td seq allocator db query failed, use allocated | account={} "
                    "allocated={} error=\"{}\"",
                    account_id, base, e.what());
            }
        } else if (base == 0) {
            SPDLOG_WARN("td seq allocator db unavailable, start from 1 | account={}", account_id);
        }
    } else if (base == 0) {
        SPDLOG_WARN("td seq allocator db provider missing, start from 1 | account={}", account_id);
    }
    const uint64_t assigned = base + 1;
    allocated_[account_id] = assigned;
    return assigned;
}

uint64_t AccountSeqAllocator::current(const std::string& account_id) const {
    const auto it = allocated_.find(account_id);
    return it != allocated_.end() ? it->second : 0;
}

OrderRecord build_no_session_reject(const DzOrderReq& req,
                                    int32_t date,
                                    int32_t time,
                                    std::string_view reason) {
    OrderRecord rec{};
    rec.base.order_id = req.order_id;
    copy_to_fixed(rec.base.strategy_id, std::string(req.strategy_id));
    copy_to_fixed(rec.base.instrument_id, std::string(req.instrument_id));
    copy_to_fixed(rec.base.account_id, std::string(req.account_id));
    rec.base.direction = req.direction;
    rec.base.price_type = req.price_type;
    rec.base.position_effect = req.position_effect;
    rec.base.status = DZ_ORDER_REJECTED;
    rec.base.price = req.price;
    rec.base.volume = req.volume;
    rec.base.volume_traded = 0;
    rec.base.date = date;
    rec.base.time = time;
    copy_to_fixed(rec.base.remark, std::string(reason));
    rec.base.seq = 0;  // 由调用方分配
    rec.is_external = 0;
    rec.volume_canceled = req.volume;
    rec.error_id = -1;  // 本地拒绝, 与 CTP 错误码区分
    copy_to_fixed(rec.error_msg, std::string(reason));
    copy_to_fixed(rec.trading_day, format_day_text(date));
    return rec;
}

}  // namespace dztrader::ctp
