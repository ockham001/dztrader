#include <dztrader/tdstore/schema_catalog.h>

namespace dztrader::tdstore {

namespace {

using dztrader::db::FieldSchema;
using dztrader::db::IndexSchema;
using dztrader::db::ResourceSchema;
using dztrader::db::ValueType;

const ResourceSchema kOrders = {
    .name = "orders",
    .fields = {
        {"id", ValueType::Int64, false, true, true},
        {"account_id", ValueType::String, false, false, false},
        {"trading_day", ValueType::String, false, false, false},
        {"order_id", ValueType::Int64, false, false, false},
        {"order_ref", ValueType::String, false, false, false},
        {"external_order_id", ValueType::String, true, false, false},
        {"is_external", ValueType::Int64, false, false, false},
        {"instrument_id", ValueType::String, false, false, false},
        {"exchange_id", ValueType::String, false, false, false},
        {"direction", ValueType::String, true, false, false},
        {"position_effect", ValueType::String, true, false, false},
        {"price_type", ValueType::String, true, false, false},
        {"status", ValueType::String, true, false, false},
        {"price", ValueType::Float64, true, false, false},
        {"volume", ValueType::Int64, true, false, false},
        {"volume_traded", ValueType::Int64, true, false, false},
        {"volume_canceled", ValueType::Int64, true, false, false},
        {"insert_time", ValueType::Int64, true, false, false},
        {"update_time", ValueType::Int64, true, false, false},
        {"error_id", ValueType::Int64, true, false, false},
        {"error_msg", ValueType::String, true, false, false},
        {"strategy_id", ValueType::String, true, false, false},
        {"remark", ValueType::String, true, false, false},
        {"seq", ValueType::Int64, false, false, false},
    },
    .indexes = {
        {"idx_orders_account_day", {"account_id", "trading_day"}, false},
        {"idx_orders_day_instr", {"trading_day", "instrument_id"}, false},
        {"idx_orders_acct_seq", {"account_id", "seq"}, false},
        {"uq_orders_account_order", {"account_id", "order_id"}, true},
    },
};

const ResourceSchema kTrades = {
    .name = "trades",
    .fields = {
        {"id", ValueType::Int64, false, true, true},
        {"account_id", ValueType::String, false, false, false},
        {"trading_day", ValueType::String, false, false, false},
        {"trade_id", ValueType::String, false, false, false},
        {"order_id", ValueType::Int64, false, false, false},
        {"instrument_id", ValueType::String, false, false, false},
        {"exchange_id", ValueType::String, false, false, false},
        {"direction", ValueType::String, true, false, false},
        {"position_effect", ValueType::String, true, false, false},
        {"price", ValueType::Float64, false, false, false},
        {"volume", ValueType::Int64, false, false, false},
        {"trade_time", ValueType::Int64, true, false, false},
        {"trade_date", ValueType::Int64, true, false, false},
        {"commission", ValueType::Float64, true, false, false},
        {"strategy_id", ValueType::String, true, false, false},
        {"seq", ValueType::Int64, false, false, false},
    },
    .indexes = {
        {"idx_trades_day_instr", {"trading_day", "instrument_id"}, false},
        {"idx_trades_account_day", {"account_id", "trading_day"}, false},
        {"idx_trades_acct_seq", {"account_id", "seq"}, false},
        {"uq_trades_account_day_trade", {"account_id", "trading_day", "trade_id"}, true},
    },
};

const ResourceSchema kPositions = {
    .name = "positions",
    .fields = {
        {"account_id", ValueType::String, false, false, false},
        {"trading_day", ValueType::String, false, false, false},
        {"instrument_id", ValueType::String, false, false, false},
        {"exchange_id", ValueType::String, false, false, false},
        {"direction", ValueType::String, false, false, false},
        {"volume", ValueType::Int64, true, false, false},
        {"frozen_volume", ValueType::Int64, true, false, false},
        {"today_volume", ValueType::Int64, true, false, false},
        {"yd_volume", ValueType::Int64, true, false, false},
        {"price", ValueType::Float64, true, false, false},
        {"seq", ValueType::Int64, false, false, false},
    },
    .indexes = {
        {"idx_positions_acct_seq", {"account_id", "seq"}, false},
        {"uq_positions_key", {"account_id", "instrument_id", "direction"}, true},
    },
};

const ResourceSchema kTradingAccounts = {
    .name = "trading_accounts",
    .fields = {
        {"account_id", ValueType::String, false, true, false},
        {"trading_day", ValueType::String, false, false, false},
        {"balance", ValueType::Float64, true, false, false},
        {"available", ValueType::Float64, true, false, false},
        {"frozen", ValueType::Float64, true, false, false},
        {"commission", ValueType::Float64, true, false, false},
        {"margin", ValueType::Float64, true, false, false},
        {"withdraw_quota", ValueType::Float64, true, false, false},
        {"deposit", ValueType::Float64, true, false, false},
        {"withdraw", ValueType::Float64, true, false, false},
        {"seq", ValueType::Int64, false, false, false},
    },
    .indexes = {
        {"idx_taccount_acct_seq", {"account_id", "seq"}, false},
    },
};

const ResourceSchema kInstruments = {
    .name = "instruments",
    .fields = {
        {"instrument_id", ValueType::String, false, true, false},
        {"exchange_id", ValueType::String, false, false, false},
        {"symbol", ValueType::String, false, false, false},
        {"name", ValueType::String, true, false, false},
        {"product_class", ValueType::Int64, false, false, false},
        {"settle_cycle", ValueType::Int64, false, false, false},
        {"settlement_method", ValueType::Int64, false, false, false},
        {"is_inverse", ValueType::Int64, false, false, false},
        {"currency", ValueType::String, false, false, false},
        {"base_asset", ValueType::String, false, false, false},
        {"min_limit_order_volume", ValueType::Int64, false, false, false},
        {"max_limit_order_volume", ValueType::Int64, false, false, false},
        {"volume_multiple", ValueType::Float64, false, false, false},
        {"price_tick", ValueType::Float64, false, false, false},
        {"volume_step", ValueType::Float64, false, false, false},
        {"listed_date", ValueType::Int64, false, false, false},
        {"delisted_date", ValueType::Int64, false, false, false},
        {"option_type", ValueType::Int64, false, false, false},
        {"option_exercise_style", ValueType::Int64, false, false, false},
        {"underlying_id", ValueType::String, false, false, false},
        {"option_strike", ValueType::Float64, false, false, false},
        {"option_series", ValueType::String, false, false, false},
        {"update_day", ValueType::String, true, false, false},
        {"product_code", ValueType::String, false, false, false},
        {"min_market_order_volume", ValueType::Int64, false, false, false},
        {"max_market_order_volume", ValueType::Int64, false, false, false},
        {"underlying_multiple", ValueType::Float64, false, false, false},
        {"updated_at", ValueType::Int64, false, false, false},
    },
    .indexes = {},
};

}  // namespace

const dztrader::db::ResourceSchema& orders_schema() { return kOrders; }
const dztrader::db::ResourceSchema& trades_schema() { return kTrades; }
const dztrader::db::ResourceSchema& positions_schema() { return kPositions; }
const dztrader::db::ResourceSchema& trading_accounts_schema() { return kTradingAccounts; }
const dztrader::db::ResourceSchema& instruments_schema() { return kInstruments; }

std::span<const dztrader::db::ResourceSchema> schemas() {
    static const ResourceSchema kAll[] = {kOrders, kTrades, kPositions, kTradingAccounts,
                                          kInstruments};
    return kAll;
}

}  // namespace dztrader::tdstore
