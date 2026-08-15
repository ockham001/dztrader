#include "td/td_schema.h"

#include <SQLiteCpp/Database.h>

namespace dztrader::ctp {

// ============================================================================
// v1: 初始表结构 (设计 §13.6)
// 字段名与 strategy_api 结构体一致 (exchange_id 而非 exchange, date 而非 trading_day)
// ============================================================================

namespace {

void migration_v1(SQLite::Database& db) {
    // orders: 委托记录 (OrderRecord = DzOrderReport + SQL 扩展字段)
    db.exec(
        "CREATE TABLE IF NOT EXISTS orders ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    account_id TEXT NOT NULL,"           // base.account_id
        "    trading_day TEXT NOT NULL,"          // trading_day (SQL 扩展)
        "    order_id INTEGER NOT NULL,"          // base.order_id
        "    order_ref TEXT NOT NULL,"            // order_ref (SQL 扩展)
        "    external_order_id TEXT,"             // external_order_id (SQL 扩展)
        "    is_external INTEGER NOT NULL DEFAULT 0," // is_external (SQL 扩展)
        "    instrument_id TEXT NOT NULL,"        // base.instrument_id
        "    exchange_id TEXT NOT NULL,"          // base.exchange_id
        "    direction CHAR(1),"                  // base.direction
        "    position_effect CHAR(1),"            // base.position_effect
        "    price_type CHAR(1),"                 // base.price_type
        "    status CHAR(1),"                     // base.status
        "    price REAL,"                         // base.price
        "    volume INTEGER,"                     // base.volume (委托数量)
        "    volume_traded INTEGER,"              // base.volume_traded
        "    volume_canceled INTEGER,"            // volume_canceled (SQL 扩展)
        "    insert_time INTEGER,"                // insert_time (SQL 扩展)
        "    update_time INTEGER,"                // update_time (SQL 扩展)
        "    error_id INTEGER,"                   // error_id (SQL 扩展)
        "    error_msg TEXT,"                     // error_msg (SQL 扩展)
        "    strategy_id TEXT,"                   // base.strategy_id
        "    remark TEXT,"                        // base.remark
        "    UNIQUE(account_id, order_id)"
        ")");
    db.exec("CREATE INDEX IF NOT EXISTS idx_orders_account_day ON orders(account_id, trading_day)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_orders_day_instr ON orders(trading_day, instrument_id)");

    // trades: 成交记录 (TradeRecord = DzTradeReport + SQL 扩展字段)
    db.exec(
        "CREATE TABLE IF NOT EXISTS trades ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    account_id TEXT NOT NULL,"           // base.account_id
        "    trading_day TEXT NOT NULL,"          // trading_day (SQL 扩展)
        "    trade_id TEXT NOT NULL,"             // base.trade_id
        "    order_id INTEGER NOT NULL,"          // base.order_id
        "    instrument_id TEXT NOT NULL,"        // base.instrument_id
        "    exchange_id TEXT NOT NULL,"          // base.exchange_id
        "    direction CHAR(1),"                  // base.direction
        "    position_effect CHAR(1),"            // base.position_effect
        "    price REAL NOT NULL,"                // base.price
        "    volume INTEGER NOT NULL,"            // base.volume
        "    trade_time INTEGER,"                 // trade_time (SQL 扩展)
        "    trade_date INTEGER,"                 // trade_date (SQL 扩展)
        "    commission REAL,"                    // commission (SQL 扩展)
        "    strategy_id TEXT,"                   // base.strategy_id
        "    UNIQUE(account_id, trade_id)"
        ")");
    db.exec("CREATE INDEX IF NOT EXISTS idx_trades_day_instr ON trades(trading_day, instrument_id)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_trades_account_day ON trades(account_id, trading_day)");

    // margin_rates: 保证金率 (直接复用 DzMarginRate)
    db.exec(
        "CREATE TABLE IF NOT EXISTS margin_rates ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    account_id TEXT NOT NULL,"
        "    instrument_id TEXT NOT NULL,"
        "    product_code TEXT NOT NULL,"
        "    exchange_id TEXT NOT NULL,"
        "    hedge_flag CHAR(1),"
        "    is_relative CHAR(1),"
        "    long_margin_ratio_by_money REAL,"
        "    long_margin_ratio_by_volume REAL,"
        "    short_margin_ratio_by_money REAL,"
        "    short_margin_ratio_by_volume REAL,"
        "    date INTEGER,"                       // DzDate (距纪元天数)
        "    UNIQUE(account_id, date, product_code)"
        ")");

    // commission_rates: 手续费率 (直接复用 DzCommissionRate)
    db.exec(
        "CREATE TABLE IF NOT EXISTS commission_rates ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    account_id TEXT NOT NULL,"
        "    instrument_id TEXT NOT NULL,"
        "    product_code TEXT NOT NULL,"
        "    exchange_id TEXT NOT NULL,"
        "    open_ratio_by_money REAL,"
        "    open_ratio_by_volume REAL,"
        "    close_ratio_by_money REAL,"
        "    close_ratio_by_volume REAL,"
        "    close_today_ratio_by_money REAL,"
        "    close_today_ratio_by_volume REAL,"
        "    date INTEGER,"                       // DzDate
        "    UNIQUE(account_id, date, product_code)"
        ")");

    // instruments: 合约信息 (DzContract + update_day)
    db.exec(
        "CREATE TABLE IF NOT EXISTS instruments ("
        "    instrument_id TEXT PRIMARY KEY,"
        "    exchange_id TEXT NOT NULL,"
        "    name TEXT,"
        "    product CHAR(1),"
        "    volume_multiple INTEGER,"
        "    price_tick REAL,"
        "    min_limit_order_volume INTEGER,"
        "    max_limit_order_volume INTEGER,"
        "    option_type CHAR(1),"
        "    option_strike REAL,"
        "    option_underlying TEXT,"
        "    option_listed INTEGER,"              // DzDate
        "    option_expiry INTEGER,"              // DzDate
        "    update_day TEXT"                     // SQL 扩展
        ")");
}

}  // namespace

void apply_td_migrations(dztrader::db::MigrationManager& mgr) {
    mgr.add(kTdSchemaVersion, migration_v1);
}

}  // namespace dztrader::ctp
