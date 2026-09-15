#include <dztrader/tdstore/schema.h>

#include <SQLiteCpp/Database.h>

namespace dztrader::tdstore {

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

    // instruments: 合约信息 (历史 DzInstrumentInfo; 现 tdstore::InstrumentRecord + update_day)
    db.exec(
        "CREATE TABLE IF NOT EXISTS instruments ("
        "    instrument_id TEXT PRIMARY KEY,"
        "    exchange_id TEXT NOT NULL,"
        "    name TEXT,"
        "    product CHAR(1),"
        "    volume_multiple REAL,"               // v3: 字段已变 double (v1 曾为 INTEGER)
        "    price_tick REAL,"
        "    min_order_volume INTEGER,"
        "    max_order_volume INTEGER,"
        "    option_type CHAR(1),"
        "    option_strike REAL,"
        "    option_underlying TEXT,"
        "    option_listed INTEGER,"              // DzDate
        "    option_expiry INTEGER,"              // DzDate
        "    update_day TEXT"                     // SQL 扩展
        ")");
}

void migration_v2(SQLite::Database& db) {
    // ---- orders 重建: 加 seq 列 (UNIQUE 不变) ----
    // 四步法 (SQLite 无法改 UNIQUE/加列到中间): 建新表 -> 拷贝 -> 删旧 -> 改名
    // 旧数据 seq 填 0 (语义: 历史行, W 不跳过它们)
    db.exec(
        "CREATE TABLE orders_v2 ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    account_id TEXT NOT NULL,"
        "    trading_day TEXT NOT NULL,"
        "    order_id INTEGER NOT NULL,"
        "    order_ref TEXT NOT NULL,"
        "    external_order_id TEXT,"
        "    is_external INTEGER NOT NULL DEFAULT 0,"
        "    instrument_id TEXT NOT NULL,"
        "    exchange_id TEXT NOT NULL,"
        "    direction CHAR(1),"
        "    position_effect CHAR(1),"
        "    price_type CHAR(1),"
        "    status CHAR(1),"
        "    price REAL,"
        "    volume INTEGER,"
        "    volume_traded INTEGER,"
        "    volume_canceled INTEGER,"
        "    insert_time INTEGER,"
        "    update_time INTEGER,"
        "    error_id INTEGER,"
        "    error_msg TEXT,"
        "    strategy_id TEXT,"
        "    remark TEXT,"
        "    seq INTEGER NOT NULL DEFAULT 0,"
        "    UNIQUE(account_id, order_id))");
    db.exec(
        "INSERT INTO orders_v2 SELECT id,account_id,trading_day,order_id,order_ref,"
        "external_order_id,is_external,instrument_id,exchange_id,direction,"
        "position_effect,price_type,status,price,volume,volume_traded,volume_canceled,"
        "insert_time,update_time,error_id,error_msg,strategy_id,remark,0 FROM orders");
    db.exec("DROP TABLE orders");
    db.exec("ALTER TABLE orders_v2 RENAME TO orders");
    // v1 既有索引 (DROP TABLE 会一并删除, 必须重建)
    db.exec("CREATE INDEX idx_orders_account_day ON orders(account_id, trading_day)");
    db.exec("CREATE INDEX idx_orders_day_instr ON orders(trading_day, instrument_id)");
    db.exec("CREATE INDEX idx_orders_acct_seq ON orders(account_id, seq)");

    // ---- trades 重建: 加 seq 列 + 唯一键升级 (spec §3.2 修复跨日 REPLACE 隐患) ----
    // 旧表已有 UNIQUE(account_id,trade_id) 保证无重复行, 拷贝不会违反新键
    db.exec(
        "CREATE TABLE trades_v2 ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    account_id TEXT NOT NULL,"
        "    trading_day TEXT NOT NULL,"
        "    trade_id TEXT NOT NULL,"
        "    order_id INTEGER NOT NULL,"
        "    instrument_id TEXT NOT NULL,"
        "    exchange_id TEXT NOT NULL,"
        "    direction CHAR(1),"
        "    position_effect CHAR(1),"
        "    price REAL NOT NULL,"
        "    volume INTEGER NOT NULL,"
        "    trade_time INTEGER,"
        "    trade_date INTEGER,"
        "    commission REAL,"
        "    strategy_id TEXT,"
        "    seq INTEGER NOT NULL DEFAULT 0,"
        "    UNIQUE(account_id, trading_day, trade_id))");
    db.exec(
        "INSERT INTO trades_v2 SELECT id,account_id,trading_day,trade_id,order_id,"
        "instrument_id,exchange_id,direction,position_effect,price,volume,"
        "trade_time,trade_date,commission,strategy_id,0 FROM trades");
    db.exec("DROP TABLE trades");
    db.exec("ALTER TABLE trades_v2 RENAME TO trades");
    // v1 既有索引 (DROP TABLE 会一并删除, 必须重建)
    db.exec("CREATE INDEX idx_trades_day_instr ON trades(trading_day, instrument_id)");
    db.exec("CREATE INDEX idx_trades_account_day ON trades(account_id, trading_day)");
    db.exec("CREATE INDEX idx_trades_acct_seq ON trades(account_id, seq)");

    // ---- 新表: positions (spec §3.2) ----
    db.exec(
        "CREATE TABLE IF NOT EXISTS positions ("
        "    account_id TEXT NOT NULL,"
        "    trading_day TEXT NOT NULL,"
        "    instrument_id TEXT NOT NULL,"
        "    exchange_id TEXT NOT NULL,"
        "    direction CHAR(1) NOT NULL,"
        "    volume INTEGER,"
        "    frozen_volume INTEGER,"
        "    today_volume INTEGER,"
        "    yd_volume INTEGER,"
        "    price REAL,"
        "    seq INTEGER NOT NULL DEFAULT 0,"
        "    UNIQUE(account_id, instrument_id, direction))");
    db.exec("CREATE INDEX idx_positions_acct_seq ON positions(account_id, seq)");

    // ---- 新表: trading_accounts ----
    db.exec(
        "CREATE TABLE IF NOT EXISTS trading_accounts ("
        "    account_id TEXT NOT NULL PRIMARY KEY,"
        "    trading_day TEXT NOT NULL,"
        "    balance REAL, available REAL, frozen REAL,"
        "    commission REAL, margin REAL, withdraw_quota REAL,"
        "    deposit REAL, withdraw REAL,"
        "    seq INTEGER NOT NULL DEFAULT 0)");
    db.exec("CREATE INDEX idx_taccount_acct_seq ON trading_accounts(account_id, seq)");
}

void migration_v3(SQLite::Database& db) {
    // instruments 重建: 旧合约结构 v2 (product CHAR(1)->INTEGER, 新增 v2 列)
    db.exec(
        "CREATE TABLE instruments_v3 ("
        "    instrument_id TEXT PRIMARY KEY,"
        "    exchange_id TEXT NOT NULL,"
        "    symbol TEXT NOT NULL DEFAULT '',"
        "    name TEXT,"
        "    product INTEGER NOT NULL DEFAULT 0,"
        "    settle_cycle INTEGER NOT NULL DEFAULT -1,"
        "    settlement_method INTEGER NOT NULL DEFAULT 0,"
        "    is_inverse INTEGER NOT NULL DEFAULT 0,"
        "    currency TEXT NOT NULL DEFAULT '',"
        "    base_asset TEXT NOT NULL DEFAULT '',"
        "    min_order_volume INTEGER NOT NULL DEFAULT 0,"
        "    max_order_volume INTEGER NOT NULL DEFAULT 0,"
        "    volume_multiple REAL NOT NULL DEFAULT 0,"
        "    price_tick REAL NOT NULL DEFAULT 0,"
        "    volume_step REAL NOT NULL DEFAULT 1,"
        "    listed_date INTEGER NOT NULL DEFAULT 0,"
        "    expiry_date INTEGER NOT NULL DEFAULT 0,"
        "    option_type INTEGER NOT NULL DEFAULT 0,"
        "    option_exercise_style INTEGER NOT NULL DEFAULT 0,"
        "    underlying_id TEXT NOT NULL DEFAULT '',"
        "    option_strike REAL NOT NULL DEFAULT 0,"
        "    option_series TEXT NOT NULL DEFAULT '',"
        "    update_day TEXT"
        ")");
    // v2 -> v3 搬运 (类型事实已用 sqlite3 实证验证):
    //   product 列: v1 bind_instrument 以 static_cast<int>(r.base.product) 绑定
    //     (td_persist_writer.cpp:641), CHAR(1) 列 TEXT affinity 下存的是**文本
    //     "70"/"79"/"83"** (ASCII 'F'/'O'/'S'), typeof()=text — CASE 必须用文本
    //     ASCII 码, 写 WHEN 'F' 永不匹配 (实证: 迁移后全变 0/UNKNOWN);
    //     双写 ASCII 码 + 字符分支 (后者的兼容性: 若历史库曾以文本方式写入过 'F');
    //   option_type 列: 同理存文本 "1"/"-1"/"0" — 直接搬运即可, INTEGER affinity
    //     的 option_type 列自动把 TEXT "1" 转回 INTEGER 1 (实证 typeof()=integer);
    //   option_listed/option_expiry 列: DzDate 绑定本就是整数, -1 旧哨兵 -> 0 新 NA
    //     (实证 typeof()=integer, CASE WHEN -1 正常命中);
    //   symbol 置空 (CTP 网关次日登录会全量重灌, 审计兼容即可)
    db.exec(
        "INSERT INTO instruments_v3 (instrument_id, exchange_id, symbol, name, product,"
        "    settle_cycle, settlement_method, is_inverse, currency, base_asset,"
        "    min_order_volume, max_order_volume, volume_multiple, price_tick, volume_step,"
        "    listed_date, expiry_date, option_type, option_exercise_style, underlying_id,"
        "    option_strike, option_series, update_day) "
        "SELECT instrument_id, exchange_id, '', name,"
        "    CASE product WHEN '70' THEN 1 WHEN '79' THEN 2 WHEN '83' THEN 4"
        "         WHEN 'F' THEN 1 WHEN 'O' THEN 2 WHEN 'S' THEN 4 ELSE 0 END,"
        "    -1, 0, 0, 'CNY', '',"
        "    min_order_volume, max_order_volume, volume_multiple, price_tick, 1.0,"
        "    CASE option_listed WHEN -1 THEN 0 ELSE option_listed END,"
        "    CASE option_expiry WHEN -1 THEN 0 ELSE option_expiry END,"
        "    option_type,"
        "    0, option_underlying, option_strike, '', update_day "
        "FROM instruments");
    db.exec("DROP TABLE instruments");
    db.exec("ALTER TABLE instruments_v3 RENAME TO instruments");
}

void migration_v4(SQLite::Database& db) {
    // v4: 列改名 + 新列 (SQLite RENAME COLUMN 3.25+)
    // 注: underlying_multiple 在 v3 中不存在, 必须在本迁移新增 (自检发现)
    db.exec("ALTER TABLE instruments RENAME COLUMN product TO product_class");
    db.exec("ALTER TABLE instruments RENAME COLUMN min_order_volume TO min_limit_order_volume");
    db.exec("ALTER TABLE instruments RENAME COLUMN max_order_volume TO max_limit_order_volume");
    db.exec("ALTER TABLE instruments RENAME COLUMN expiry_date TO delisted_date");
    db.exec("ALTER TABLE instruments ADD COLUMN product_code TEXT NOT NULL DEFAULT ''");
    db.exec("ALTER TABLE instruments ADD COLUMN min_market_order_volume INTEGER NOT NULL DEFAULT 0");
    db.exec("ALTER TABLE instruments ADD COLUMN max_market_order_volume INTEGER NOT NULL DEFAULT 0");
    db.exec("ALTER TABLE instruments ADD COLUMN underlying_multiple REAL NOT NULL DEFAULT 0");
    db.exec("ALTER TABLE instruments ADD COLUMN updated_at INTEGER NOT NULL DEFAULT 0");
}

}  // namespace

void apply_td_migrations(dztrader::db::MigrationManager& mgr) {
    mgr.add(1, migration_v1);
    mgr.add(2, migration_v2);
    mgr.add(3, migration_v3);
    mgr.add(4, migration_v4);
}

}  // namespace dztrader::tdstore
