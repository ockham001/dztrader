#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>

#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <random>
#include <string>

using namespace dztrader::db;

namespace {

std::filesystem::path unique_db_path(const std::string& name) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           (name + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
            "_" + std::to_string(dist(gen)) + ".db");
}

/// 删除 db 及附属文件 (Windows 下须先释放全部连接句柄再调用)
void remove_db_files(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    std::filesystem::remove(path.string() + "-journal", ec);
}

int64_t raw_scalar(const std::filesystem::path& path, const std::string& sql) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement q(db, sql);
    return q.executeStep() ? q.getColumn(0).getInt64() : 0;
}

double raw_double(const std::filesystem::path& path, const std::string& sql) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement q(db, sql);
    return q.executeStep() ? q.getColumn(0).getDouble() : 0.0;
}

std::string raw_string(const std::filesystem::path& path, const std::string& sql) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement q(db, sql);
    return q.executeStep() ? q.getColumn(0).getString() : "";
}

/// 列名 -> 声明类型 (PRAGMA table_info 的 name/type 列)
std::map<std::string, std::string> raw_columns(const std::filesystem::path& path,
                                               const std::string& table) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement q(db, "PRAGMA table_info(" + table + ")");
    std::map<std::string, std::string> columns;
    while (q.executeStep()) {
        columns.emplace(q.getColumn(1).getString(), q.getColumn(2).getString());
    }
    return columns;
}

void open_and_migrate(const std::filesystem::path& path) {
    auto database =
        Database::open(Config{.backend = "sqlite", .options = {{"path", path.string()}}});
    database->migrate();
}

/// v1 DDL: 逐字复制自 src/sqlite_migrations.cpp migration_v1 (内部符号不可见;
/// 升级测试必须构造真实历史库, 故此处置入 DDL 是有意为之 — 历史迁移不可改).
void create_v1_schema(SQLite::Database& db) {
    db.exec(
        "CREATE TABLE IF NOT EXISTS orders ("
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
        "    UNIQUE(account_id, order_id)"
        ")");
    db.exec("CREATE INDEX IF NOT EXISTS idx_orders_account_day ON orders(account_id, trading_day)");
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_orders_day_instr ON orders(trading_day, instrument_id)");

    db.exec(
        "CREATE TABLE IF NOT EXISTS trades ("
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
        "    UNIQUE(account_id, trade_id)"
        ")");
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_trades_day_instr ON trades(trading_day, instrument_id)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_trades_account_day ON trades(account_id, trading_day)");

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
        "    date INTEGER,"
        "    UNIQUE(account_id, date, product_code)"
        ")");

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
        "    date INTEGER,"
        "    UNIQUE(account_id, date, product_code)"
        ")");

    db.exec(
        "CREATE TABLE IF NOT EXISTS instruments ("
        "    instrument_id TEXT PRIMARY KEY,"
        "    exchange_id TEXT NOT NULL,"
        "    name TEXT,"
        "    product CHAR(1),"
        "    volume_multiple REAL,"
        "    price_tick REAL,"
        "    min_order_volume INTEGER,"
        "    max_order_volume INTEGER,"
        "    option_type CHAR(1),"
        "    option_strike REAL,"
        "    option_underlying TEXT,"
        "    option_listed INTEGER,"
        "    option_expiry INTEGER,"
        "    update_day TEXT"
        ")");
}

}  // namespace

// ============================================================================
// Important 1: v1 -> v5 升级路径回归.
// 被删的 tdstore/tests/schema_test.cpp 是唯一覆盖历史库升级的用例; 统一到 libs/db 后
// 仅测全新库 v5 的话, 升级会静默丢数据/丢约束. 本用例构造真实 v1 库 (含数据), 走公开
// 入口 Database::open()+migrate() 升级到 v5.
// v4 单独构造需要复制 v1..v4 全部 DDL 才"忠实"; 本用例逐版本执行 v2/v3/v4, v4 产出
// (改名+新列) 在下面逐列断言, 故不再单独构造 v4 库 — 重复 DDL 收益低且易与驱动漂移.
// ============================================================================
TEST(SqliteUpgradeTest, V1WithDataUpgradesToV5PreservingRows) {
    const auto path = unique_db_path("dz_sqlite_upgrade_v1");
    {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        create_v1_schema(db);
        // 2 单 2 成 2 合约: 覆盖 v2 重建保数据 / v3 类型搬运 / v4 改名
        // 第一单/第一成逐列填满互异值, 供 v2 位置式 INSERT 的 23/15 列逐列断言
        db.exec(
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, external_order_id, "
            "is_external, instrument_id, exchange_id, direction, position_effect, price_type, "
            "status, price, volume, volume_traded, volume_canceled, insert_time, update_time, "
            "error_id, error_msg, strategy_id, remark) "
            "VALUES ('acc1', '20260726', 1, 'REF1', 'EXT1', 1, 'IF2506', 'CFFEX', '0', '1', '2', "
            "'4', 3900.5, 2, 1, 1, 1001, 1002, 7, 'err1', 'strat1', 'note1')");
        db.exec(
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id, "
            "exchange_id, status, price, volume) "
            "VALUES ('acc1', '20260726', 2, '000002', 'IF2506', 'CFFEX', '1', 3899.0, 1)");
        db.exec(
            "INSERT INTO trades (account_id, trading_day, trade_id, order_id, instrument_id, "
            "exchange_id, direction, position_effect, price, volume, trade_time, trade_date, "
            "commission, strategy_id) "
            "VALUES ('acc1', '20260726', 'T1', 1, 'IF2506', 'CFFEX', '0', '2', 3900.25, 1, 123456, "
            "20260726, 1.5, 'strat1')");
        db.exec(
            "INSERT INTO trades (account_id, trading_day, trade_id, order_id, instrument_id, "
            "exchange_id, price, volume) "
            "VALUES ('acc1', '20260726', 'T2', 2, 'IF2506', 'CFFEX', 3901.5, 1)");
        // product: ASCII 文本 '70' (期货, 历史写入形态) / 文本枚举 '13' (SPOT)
        // option_listed/option_expiry: -1 哨兵; option_strike: 0.0 (v1 写入路径全字段 bind,
        // 不产生 NULL — bind_instrument 未做 NULL 绑定)
        db.exec(
            "INSERT INTO instruments (instrument_id, exchange_id, name, product, volume_multiple, "
            "price_tick, min_order_volume, max_order_volume, option_type, option_strike, "
            "option_listed, option_expiry, option_underlying, update_day) "
            "VALUES ('IF2506', 'CFFEX', '沪深300', '70', 300, 0.2, 1, 10, '0', 0.0, -1, -1, '', "
            "'20260726')");
        db.exec(
            "INSERT INTO instruments (instrument_id, exchange_id, name, product, volume_multiple, "
            "price_tick, min_order_volume, max_order_volume, option_type, option_strike, "
            "option_listed, option_expiry, option_underlying, update_day) "
            "VALUES ('AU9999', 'SGE', '黄金现货', '13', 1, 0.01, 1, 1000, '0', 0.0, -1, -1, '', "
            "'20260726')");
        db.exec(
            "CREATE TABLE schema_version (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)");
        db.exec(
            "INSERT INTO schema_version (version, applied_at) VALUES (1, '2026-01-01T00:00:00Z')");
    }

    open_and_migrate(path);

    // v2..v5 全部应用
    EXPECT_EQ(raw_scalar(path, "SELECT MAX(version) FROM schema_version"), 5);
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM schema_version"), 5);
    // v2 重建保数据: orders 23 列 / trades 15 列位置式 INSERT 逐列断言
    // (列错位或丢失时, 只抽验 status/volume/price 不会变红)
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM orders"), 2);
    const std::string order_where = " WHERE account_id='acc1' AND order_id=1";
    EXPECT_EQ(raw_scalar(path, "SELECT id FROM orders" + order_where), 1);
    EXPECT_EQ(raw_string(path, "SELECT account_id FROM orders" + order_where), "acc1");
    EXPECT_EQ(raw_string(path, "SELECT trading_day FROM orders" + order_where), "20260726");
    EXPECT_EQ(raw_scalar(path, "SELECT order_id FROM orders" + order_where), 1);
    EXPECT_EQ(raw_string(path, "SELECT order_ref FROM orders" + order_where), "REF1");
    EXPECT_EQ(raw_string(path, "SELECT external_order_id FROM orders" + order_where), "EXT1");
    EXPECT_EQ(raw_scalar(path, "SELECT is_external FROM orders" + order_where), 1);
    EXPECT_EQ(raw_string(path, "SELECT instrument_id FROM orders" + order_where), "IF2506");
    EXPECT_EQ(raw_string(path, "SELECT exchange_id FROM orders" + order_where), "CFFEX");
    EXPECT_EQ(raw_string(path, "SELECT direction FROM orders" + order_where), "0");
    EXPECT_EQ(raw_string(path, "SELECT position_effect FROM orders" + order_where), "1");
    EXPECT_EQ(raw_string(path, "SELECT price_type FROM orders" + order_where), "2");
    EXPECT_EQ(raw_string(path, "SELECT status FROM orders" + order_where), "4");
    EXPECT_DOUBLE_EQ(raw_double(path, "SELECT price FROM orders" + order_where), 3900.5);
    EXPECT_EQ(raw_scalar(path, "SELECT volume FROM orders" + order_where), 2);
    EXPECT_EQ(raw_scalar(path, "SELECT volume_traded FROM orders" + order_where), 1);
    EXPECT_EQ(raw_scalar(path, "SELECT volume_canceled FROM orders" + order_where), 1);
    EXPECT_EQ(raw_scalar(path, "SELECT insert_time FROM orders" + order_where), 1001);
    EXPECT_EQ(raw_scalar(path, "SELECT update_time FROM orders" + order_where), 1002);
    EXPECT_EQ(raw_scalar(path, "SELECT error_id FROM orders" + order_where), 7);
    EXPECT_EQ(raw_string(path, "SELECT error_msg FROM orders" + order_where), "err1");
    EXPECT_EQ(raw_string(path, "SELECT strategy_id FROM orders" + order_where), "strat1");
    EXPECT_EQ(raw_string(path, "SELECT remark FROM orders" + order_where), "note1");
    EXPECT_EQ(raw_scalar(path, "SELECT seq FROM orders" + order_where), 0);

    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM trades"), 2);
    const std::string trade_where = " WHERE account_id='acc1' AND trade_id='T1'";
    EXPECT_EQ(raw_scalar(path, "SELECT id FROM trades" + trade_where), 1);
    EXPECT_EQ(raw_string(path, "SELECT account_id FROM trades" + trade_where), "acc1");
    EXPECT_EQ(raw_string(path, "SELECT trading_day FROM trades" + trade_where), "20260726");
    EXPECT_EQ(raw_string(path, "SELECT trade_id FROM trades" + trade_where), "T1");
    EXPECT_EQ(raw_scalar(path, "SELECT order_id FROM trades" + trade_where), 1);
    EXPECT_EQ(raw_string(path, "SELECT instrument_id FROM trades" + trade_where), "IF2506");
    EXPECT_EQ(raw_string(path, "SELECT exchange_id FROM trades" + trade_where), "CFFEX");
    EXPECT_EQ(raw_string(path, "SELECT direction FROM trades" + trade_where), "0");
    EXPECT_EQ(raw_string(path, "SELECT position_effect FROM trades" + trade_where), "2");
    EXPECT_DOUBLE_EQ(raw_double(path, "SELECT price FROM trades" + trade_where), 3900.25);
    EXPECT_EQ(raw_scalar(path, "SELECT volume FROM trades" + trade_where), 1);
    EXPECT_EQ(raw_scalar(path, "SELECT trade_time FROM trades" + trade_where), 123456);
    EXPECT_EQ(raw_scalar(path, "SELECT trade_date FROM trades" + trade_where), 20260726);
    EXPECT_DOUBLE_EQ(raw_double(path, "SELECT commission FROM trades" + trade_where), 1.5);
    EXPECT_EQ(raw_string(path, "SELECT strategy_id FROM trades" + trade_where), "strat1");
    EXPECT_EQ(raw_scalar(path, "SELECT seq FROM trades" + trade_where), 0);
    EXPECT_DOUBLE_EQ(raw_double(path, "SELECT price FROM trades WHERE trade_id='T2'"), 3901.5);
    // v3 重建保数据 + 类型搬运 (ASCII '70' -> 1, 文本枚举 '13' -> 13)
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM instruments"), 2);
    EXPECT_EQ(
        raw_scalar(path, "SELECT product_class FROM instruments WHERE instrument_id='IF2506'"), 1);
    EXPECT_EQ(
        raw_scalar(path, "SELECT product_class FROM instruments WHERE instrument_id='AU9999'"), 13);
    EXPECT_EQ(
        raw_scalar(path,
                   "SELECT min_limit_order_volume FROM instruments WHERE instrument_id='IF2506'"),
        1);
    EXPECT_EQ(
        raw_scalar(path,
                   "SELECT max_limit_order_volume FROM instruments WHERE instrument_id='IF2506'"),
        10);
    EXPECT_EQ(raw_scalar(path, "SELECT settle_cycle FROM instruments WHERE instrument_id='IF2506'"),
              -1);
    EXPECT_EQ(raw_string(path, "SELECT currency FROM instruments WHERE instrument_id='IF2506'"),
              "CNY");
    EXPECT_EQ(raw_string(path, "SELECT update_day FROM instruments WHERE instrument_id='IF2506'"),
              "20260726");
    // v4 改名 + 哨兵归零 + 新列默认值
    EXPECT_EQ(
        raw_scalar(path, "SELECT delisted_date FROM instruments WHERE instrument_id='IF2506'"), 0);
    EXPECT_EQ(raw_string(path, "SELECT product_code FROM instruments WHERE instrument_id='IF2506'"),
              "");
    EXPECT_EQ(
        raw_scalar(path,
                   "SELECT min_market_order_volume FROM instruments WHERE instrument_id='IF2506'"),
        0);
    EXPECT_EQ(
        raw_scalar(path,
                   "SELECT max_market_order_volume FROM instruments WHERE instrument_id='IF2506'"),
        0);
    EXPECT_EQ(raw_scalar(
                  path, "SELECT underlying_multiple FROM instruments WHERE instrument_id='IF2506'"),
              0);
    EXPECT_EQ(raw_scalar(path, "SELECT updated_at FROM instruments WHERE instrument_id='IF2506'"),
              0);
    // v5: 费率两表退役
    EXPECT_EQ(raw_scalar(path,
                         "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                         "AND name IN ('margin_rates','commission_rates')"),
              0);

    remove_db_files(path);
}

// v4 产出列断言 (全新库 v5): 改名后列在、旧列名消失、声明类型正确.
TEST(SqliteUpgradeTest, V5SchemaHasV4InstrumentColumns) {
    const auto path = unique_db_path("dz_sqlite_upgrade_v4cols");
    open_and_migrate(path);

    const auto columns = raw_columns(path, "instruments");
    for (const char* name :
         {"product_class", "delisted_date", "product_code", "min_market_order_volume",
          "max_market_order_volume", "underlying_multiple", "updated_at"}) {
        EXPECT_EQ(columns.count(name), 1u) << "missing column: " << name;
    }
    for (const char* name : {"product", "min_order_volume", "max_order_volume", "expiry_date"}) {
        EXPECT_EQ(columns.count(name), 0u) << "legacy column still present: " << name;
    }
    EXPECT_EQ(columns.at("product_class"), "INTEGER");
    EXPECT_EQ(columns.at("delisted_date"), "INTEGER");
    EXPECT_EQ(columns.at("product_code"), "TEXT");
    EXPECT_EQ(columns.at("min_market_order_volume"), "INTEGER");
    EXPECT_EQ(columns.at("underlying_multiple"), "REAL");
    EXPECT_EQ(columns.at("updated_at"), "INTEGER");
    EXPECT_EQ(raw_scalar(path,
                         "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                         "AND name IN ('margin_rates','commission_rates')"),
              0);

    remove_db_files(path);
}

// ============================================================================
// Important 1.3: migrate 后 UNIQUE / PK 约束仍生效 (旧 schema_test 唯一覆盖)
// ============================================================================

TEST(SqliteUpgradeTest, OrdersUniqueConstraintRejectsDuplicate) {
    const auto path = unique_db_path("dz_sqlite_upgrade_uniq_orders");
    open_and_migrate(path);
    {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE);
        db.exec(
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id, "
            "exchange_id) VALUES ('acc1', '20260726', 1, '001', 'IF2506', 'CFFEX')");
        EXPECT_THROW(db.exec("INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
                             "instrument_id, exchange_id) "
                             "VALUES ('acc1', '20260726', 1, '002', 'IF2506', 'CFFEX')"),
                     SQLite::Exception);
        // 不同账户同 order_id 合法
        EXPECT_NO_THROW(db.exec(
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id, "
            "exchange_id) VALUES ('acc2', '20260726', 1, '001', 'IF2506', 'CFFEX')"));
        EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM orders"), 2);
    }
    remove_db_files(path);
}

TEST(SqliteUpgradeTest, TradesUniqueKeyAllowsSameTradeIdAcrossDays) {
    const auto path = unique_db_path("dz_sqlite_upgrade_uniq_trades");
    open_and_migrate(path);
    {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE);
        const char* kInsert =
            "INSERT INTO trades (account_id, trading_day, trade_id, order_id, instrument_id, "
            "exchange_id, price, volume) VALUES ('acc1', ";
        // v2 键含 trading_day: CTP TradeID 跨日重复必须共存 (v1 键会误拒)
        db.exec(std::string(kInsert) + "'20260726', 'T1', 1, 'IF2506', 'CFFEX', 3900.0, 1)");
        EXPECT_NO_THROW(
            db.exec(std::string(kInsert) + "'20260727', 'T1', 2, 'IF2506', 'CFFEX', 3901.0, 1)"));
        // 同账户+同交易日+同 trade_id -> 拒绝
        EXPECT_THROW(
            db.exec(std::string(kInsert) + "'20260726', 'T1', 3, 'IF2506', 'CFFEX', 3902.0, 1)"),
            SQLite::Exception);
        EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM trades"), 2);
    }
    remove_db_files(path);
}

TEST(SqliteUpgradeTest, InstrumentsPrimaryKeyRejectsDuplicate) {
    const auto path = unique_db_path("dz_sqlite_upgrade_pk_instr");
    open_and_migrate(path);
    {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE);
        db.exec(
            "INSERT INTO instruments (instrument_id, exchange_id, volume_multiple, price_tick) "
            "VALUES ('IF2506', 'CFFEX', 300, 0.2)");
        EXPECT_THROW(
            db.exec(
                "INSERT INTO instruments (instrument_id, exchange_id, volume_multiple, price_tick) "
                "VALUES ('IF2506', 'CFFEX', 300, 0.2)"),
            SQLite::Exception);
        EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM instruments"), 1);
    }
    remove_db_files(path);
}

// ============================================================================
// v2 失败回滚: 整轮迁移包在单事务里 (sqlite_migrations.cpp apply_td_migrations).
// 成功路径用例无法发现"半迁移": 这里预置缺 seq 列的 positions 表, 使 v2 在
// orders/trades 重建之后建 idx_positions_acct_seq 时失败, 从而验证此前 DDL/数据
// 变更随事务整体回滚.
// ============================================================================
TEST(SqliteUpgradeTest, V2FailureRollsBackWholeMigration) {
    const auto path = unique_db_path("dz_sqlite_upgrade_v2_fail");
    {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        create_v1_schema(db);
        db.exec(
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id, "
            "exchange_id, status, price, volume) "
            "VALUES ('acc1', '20260726', 1, '000001', 'IF2506', 'CFFEX', '4', 3900.0, 2)");
        db.exec(
            "INSERT INTO trades (account_id, trading_day, trade_id, order_id, instrument_id, "
            "exchange_id, price, volume) "
            "VALUES ('acc1', '20260726', 'T1', 1, 'IF2506', 'CFFEX', 3900.0, 1)");
        db.exec("CREATE TABLE positions (account_id TEXT NOT NULL)");
        db.exec(
            "CREATE TABLE schema_version (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)");
        db.exec(
            "INSERT INTO schema_version (version, applied_at) VALUES (1, '2026-01-01T00:00:00Z')");
    }

    DzErrorCode code = DZ_EC_OK;
    try {
        open_and_migrate(path);
    } catch (const dztrader::Exception& e) {
        code = e.code();
    }
    EXPECT_EQ(code, DZ_EC_DB_MIGRATE_FAILED);

    // 版本停在 v1
    EXPECT_EQ(raw_scalar(path, "SELECT MAX(version) FROM schema_version"), 1);
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM schema_version"), 1);
    // orders 仍是 v1 结构 (无 seq), 旧数据原样
    const auto order_columns = raw_columns(path, "orders");
    EXPECT_EQ(order_columns.count("seq"), 0u);
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM orders"), 1);
    EXPECT_EQ(raw_scalar(path, "SELECT status FROM orders WHERE order_id=1"), 4);
    EXPECT_EQ(raw_scalar(path, "SELECT volume FROM orders WHERE order_id=1"), 2);
    EXPECT_DOUBLE_EQ(raw_double(path, "SELECT price FROM orders WHERE order_id=1"), 3900.0);
    // trades 仍是 v1 结构 (无 seq), 旧数据原样
    const auto trade_columns = raw_columns(path, "trades");
    EXPECT_EQ(trade_columns.count("seq"), 0u);
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM trades"), 1);
    EXPECT_EQ(raw_string(path, "SELECT trade_id FROM trades"), "T1");
    // 无半迁移残留: v2 建过的中间表/后续表都不存在
    EXPECT_EQ(raw_scalar(path,
                         "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                         "AND name IN ('orders_v2','trades_v2','trading_accounts')"),
              0);
    // 预置的 positions 保持原样 (未补列)
    EXPECT_EQ(raw_columns(path, "positions").count("seq"), 0u);

    remove_db_files(path);
}

// ============================================================================
// v3 product CASE 的 20 个 WHEN 逐个分支 + ELSE 0; 另验证真实期权字段
// (option_type / v1 option_listed / v1 option_expiry, 均非 -1 哨兵) 原值保留.
// ============================================================================
TEST(SqliteUpgradeTest, V3ProductClassMapsEveryCaseBranch) {
    struct CaseRow {
        const char* product;
        int64_t expected;
    };
    const CaseRow cases[] = {
        {"70", 1}, {"79", 2}, {"83", 4},  // ASCII 'F'/'O'/'S'
        {"F", 1},  {"O", 2},  {"S", 4},   // 字符形态
        {"1", 1},  {"2", 2},  {"3", 3},   {"4", 4},   {"5", 5},   {"6", 6},   {"7", 7},
        {"8", 8},  {"9", 9},  {"10", 10}, {"11", 11}, {"12", 12}, {"13", 13}, {"14", 14},
    };
    const auto path = unique_db_path("dz_sqlite_upgrade_v3_case");
    {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        create_v1_schema(db);
        // v3 目标是 NOT NULL DEFAULT 0, 而 v1 这些列可空: 必须显式给值避免 NULL 搬运失败
        SQLite::Statement ins(
            db,
            "INSERT INTO instruments (instrument_id, exchange_id, product, option_type, "
            "option_strike, option_listed, option_expiry, option_underlying, min_order_volume, "
            "max_order_volume, volume_multiple, price_tick) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 1.0, 0.0)");
        auto insert_row = [&](const std::string& id, const std::string& product,
                              const std::string& option_type, double strike, int64_t listed,
                              int64_t expiry, const std::string& underlying) {
            ins.bind(1, id);
            ins.bind(2, std::string("CFFEX"));
            ins.bind(3, product);
            ins.bind(4, option_type);
            ins.bind(5, strike);
            ins.bind(6, listed);
            ins.bind(7, expiry);
            ins.bind(8, underlying);
            ins.exec();
            ins.reset();
        };
        for (size_t i = 0; i < std::size(cases); ++i) {
            insert_row("CASE_" + std::to_string(i), cases[i].product, "0", 0.0, -1, -1, "");
        }
        insert_row("CASE_ELSE", "99", "0", 0.0, -1, -1, "");  // ELSE 0
        // 非哨兵真实值: v3 必须原值搬运, 不得被 CASE WHEN -1 归零
        insert_row("OPT_REAL", "70", "2", 12.5, 20260101, 20261231, "IF2506");
        db.exec(
            "CREATE TABLE schema_version (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)");
        db.exec(
            "INSERT INTO schema_version (version, applied_at) VALUES (1, '2026-01-01T00:00:00Z')");
    }

    open_and_migrate(path);

    for (size_t i = 0; i < std::size(cases); ++i) {
        SCOPED_TRACE(cases[i].product);
        EXPECT_EQ(
            raw_scalar(path, "SELECT product_class FROM instruments WHERE instrument_id='CASE_" +
                                 std::to_string(i) + "'"),
            cases[i].expected);
    }
    EXPECT_EQ(
        raw_scalar(path, "SELECT product_class FROM instruments WHERE instrument_id='CASE_ELSE'"),
        0);
    EXPECT_EQ(
        raw_scalar(path, "SELECT option_type FROM instruments WHERE instrument_id='OPT_REAL'"), 2);
    // v1 的 option_listed/option_expiry 在 v3 改名为 listed_date/expiry_date,
    // expiry_date 再经 v4 改名 delisted_date; 非 -1 哨兵必须原值保留
    EXPECT_EQ(
        raw_scalar(path, "SELECT listed_date FROM instruments WHERE instrument_id='OPT_REAL'"),
        20260101);
    EXPECT_EQ(
        raw_scalar(path, "SELECT delisted_date FROM instruments WHERE instrument_id='OPT_REAL'"),
        20261231);
    EXPECT_DOUBLE_EQ(
        raw_double(path, "SELECT option_strike FROM instruments WHERE instrument_id='OPT_REAL'"),
        12.5);
    // v3 把 option_underlying 改名为 underlying_id
    EXPECT_EQ(
        raw_string(path, "SELECT underlying_id FROM instruments WHERE instrument_id='OPT_REAL'"),
        "IF2506");

    remove_db_files(path);
}
