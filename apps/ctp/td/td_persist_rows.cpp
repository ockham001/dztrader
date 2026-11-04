#include "td/td_persist_rows.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <SQLiteCpp/Statement.h>

#include <dztrader/data_type.h>
#include <dztrader/tdstore/records.h>

namespace dztrader::ctp {

namespace {

/// 外部单/手工单/同步单用保留策略标识表达（不再有 is_external 列；ADR 0041 §1）。
std::string order_strategy_id(const OrderRecord& r) {
    if (r.is_external != 0 && r.base.strategy_id[0] == '\0') {
        return std::string(DZ_STRATEGY_ID_EXTERNAL);
    }
    return std::string(r.base.strategy_id);
}

/// CTP 扩展字段 -> ext JSON（ADR 0041 §1：柜台差异不进列；登录日装载/重放过滤读 ext）。
std::string build_order_ext(const OrderRecord& r) {
    nlohmann::json ext = nlohmann::json::object();
    if (r.order_ref[0] != '\0') {
        ext[kOrderExtOrderRef] = std::string(r.order_ref);
    }
    if (r.external_order_id[0] != '\0') {
        ext[kOrderExtOrderSysId] = std::string(r.external_order_id);
    }
    if (r.volume_canceled != 0) {
        ext[kOrderExtVolumeCanceled] = r.volume_canceled;
    }
    if (r.update_time != 0) {
        ext[kOrderExtUpdateTime] = r.update_time;
    }
    if (r.error_id != 0) {
        ext[kOrderExtErrorId] = r.error_id;
    }
    if (r.error_msg[0] != '\0') {
        ext[kOrderExtErrorMessage] = std::string(r.error_msg);
    }
    return ext.dump();
}

}  // namespace

void bind_order(SQLite::Statement& stmt, const OrderRecord& r, std::string_view gateway) {
    stmt.bind(":account_id", std::string(r.base.account_id));
    stmt.bind(":date", std::string(r.trading_day));
    stmt.bind(":order_id", static_cast<int64_t>(r.base.order_id));
    stmt.bind(":strategy_id", order_strategy_id(r));
    stmt.bind(":instrument_id", std::string(r.base.instrument_id));
    stmt.bind(":exchange_id", std::string(r.base.exchange_id));
    stmt.bind(":direction", static_cast<int64_t>(r.base.direction));
    stmt.bind(":position_effect", static_cast<int64_t>(r.base.position_effect));
    stmt.bind(":price_type", static_cast<int64_t>(r.base.price_type));
    stmt.bind(":status", static_cast<int64_t>(r.base.status));
    stmt.bind(":price", r.base.price);
    stmt.bind(":volume", static_cast<int64_t>(r.base.volume));
    stmt.bind(":volume_traded", static_cast<int64_t>(r.base.volume_traded));
    stmt.bind(":remark", std::string(r.base.remark));
    stmt.bind(":time", static_cast<int64_t>(r.base.time));
    stmt.bind(":avg_price", r.base.avg_price);
    stmt.bind(":seq", static_cast<int64_t>(r.base.seq));
    stmt.bind(":gateway", std::string(gateway));
    stmt.bind(":ext", build_order_ext(r));
}

void bind_trade(SQLite::Statement& stmt, const TradeRecord& r, std::string_view gateway) {
    stmt.bind(":account_id", std::string(r.base.account_id));
    stmt.bind(":date", std::string(r.trading_day));
    stmt.bind(":trade_id", std::string(r.base.trade_id));
    stmt.bind(":order_id", static_cast<int64_t>(r.base.order_id));
    stmt.bind(":strategy_id", std::string(r.base.strategy_id));
    stmt.bind(":instrument_id", std::string(r.base.instrument_id));
    stmt.bind(":exchange_id", std::string(r.base.exchange_id));
    stmt.bind(":direction", static_cast<int64_t>(r.base.direction));
    stmt.bind(":position_effect", static_cast<int64_t>(r.base.position_effect));
    stmt.bind(":price", r.base.price);
    stmt.bind(":volume", static_cast<int64_t>(r.base.volume));
    stmt.bind(":time", static_cast<int64_t>(r.base.time));
    stmt.bind(":seq", static_cast<int64_t>(r.base.seq));
    stmt.bind(":gateway", std::string(gateway));
    stmt.bind(":ext", std::string("{}"));  // ext: CTP 无逐笔扩展
}

void bind_position(SQLite::Statement& stmt,
                   const DzPositionInfo& r,
                   std::string_view date,
                   std::string_view gateway) {
    stmt.bind(":account_id", std::string(r.account_id));
    stmt.bind(":date", std::string(date));
    stmt.bind(":instrument_id", std::string(r.instrument_id));
    stmt.bind(":exchange_id", std::string(r.exchange_id));
    stmt.bind(":direction", static_cast<int64_t>(r.direction));
    stmt.bind(":volume", r.volume);
    stmt.bind(":frozen_volume", r.frozen_volume);
    stmt.bind(":today_volume", r.today_volume);
    stmt.bind(":yd_volume", r.yd_volume);
    stmt.bind(":price", r.price);
    stmt.bind(":seq", static_cast<int64_t>(r.seq));
    stmt.bind(":gateway", std::string(gateway));
    stmt.bind(":ext", std::string("{}"));  // ext: 暂无柜台扩展字段
}

void bind_trading_account(SQLite::Statement& stmt,
                          const DzTradingAccount& r,
                          std::string_view date,
                          std::string_view gateway) {
    stmt.bind(":account_id", std::string(r.account_id));
    stmt.bind(":date", std::string(date));
    stmt.bind(":balance", r.balance);
    stmt.bind(":available", r.available);
    stmt.bind(":frozen", r.frozen);
    stmt.bind(":commission", r.commission);
    stmt.bind(":margin", r.margin);
    stmt.bind(":withdraw_quota", r.withdraw_quota);
    stmt.bind(":deposit", r.deposit);
    stmt.bind(":withdraw", r.withdraw);
    stmt.bind(":seq", static_cast<int64_t>(r.seq));
    stmt.bind(":gateway", std::string(gateway));
    stmt.bind(":ext", std::string("{}"));  // ext: 暂无柜台扩展字段
}

void bind_instrument(SQLite::Statement& stmt, const tdstore::InstrumentRecord& r) {
    stmt.bind(":instrument_id", r.instrument_id);
    stmt.bind(":exchange_id", r.exchange_id);
    stmt.bind(":symbol", r.symbol);
    stmt.bind(":name", r.name);
    stmt.bind(":product_class", static_cast<int64_t>(r.product_class));
    stmt.bind(":settle_cycle", static_cast<int64_t>(r.settle_cycle));
    stmt.bind(":settlement_method", int64_t{0});  // 历史保留列默认值
    stmt.bind(":is_inverse", static_cast<int64_t>(r.is_inverse));
    stmt.bind(":currency", r.currency);
    stmt.bind(":base_asset", r.base_asset);
    stmt.bind(":min_limit_order_volume", r.min_limit_order_volume);
    stmt.bind(":max_limit_order_volume", r.max_limit_order_volume);
    stmt.bind(":volume_multiple", r.volume_multiple);
    stmt.bind(":price_tick", r.price_tick);
    stmt.bind(":volume_step", r.volume_step);
    stmt.bind(":listed_date", static_cast<int64_t>(r.listed_date));
    stmt.bind(":delisted_date", static_cast<int64_t>(r.delisted_date));
    stmt.bind(":option_type", static_cast<int64_t>(r.option_type));
    stmt.bind(":option_exercise_style", int64_t{0});  // 历史保留列默认值
    stmt.bind(":underlying_id", r.underlying_id);
    stmt.bind(":option_strike", r.option_strike);
    stmt.bind(":option_series", std::string(""));  // 历史保留列默认值
    stmt.bind(":update_day", r.update_day);
    stmt.bind(":product_code", r.product_code);
    stmt.bind(":min_market_order_volume", r.min_market_order_volume);
    stmt.bind(":max_market_order_volume", r.max_market_order_volume);
    stmt.bind(":underlying_multiple", r.underlying_multiple);
    stmt.bind(":updated_at", r.updated_at);
}

}  // namespace dztrader::ctp
