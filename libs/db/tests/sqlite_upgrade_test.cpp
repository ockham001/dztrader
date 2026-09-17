#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>

#include <cstdint>
#include <filesystem>
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
           (name + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
            std::to_string(dist(gen)) + ".db");
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
    db.exec("CREATE INDEX IF NOT EXISTS idx_orders_day_instr ON orders(trading_day, instrument_id)");

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
    db.exec("CREATE INDEX IF NOT EXISTS idx_trades_day_instr ON trades(trading_day, instrument_id)");
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
        db.exec(
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id, "
            "exchange_id, status, price, volume) "
            "VALUES ('acc1', '20260726', 1, '000001', 'IF2506', 'CFFEX', '4', 3900.0, 2)");
        db.exec(
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id, "
            "exchange_id, status, price, volume) "
            "VALUES ('acc1', '20260726', 2, '000002', 'IF2506', 'CFFEX', '1', 3899.0, 1)");
        db.exec(
            "INSERT INTO trades (account_id, trading_day, trade_id, order_id, instrument_id, "
            "exchange_id, price, volume) "
            "VALUES ('acc1', '20260726', 'T1', 1, 'IF2506', 'CFFEX', 3900.0, 1)");
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
        db.exec("INSERT INTO schema_version (version, applied_at) VALUES (1, '2026-01-01T00:00:00Z')");
    }

    open_and_migrate(path);

    // v2..v5 全部应用
    EXPECT_EQ(raw_scalar(path, "SELECT MAX(version) FROM schema_version"), 5);
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM schema_version"), 5);
    // v2 重建保数据
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM orders"), 2);
    EXPECT_EQ(raw_scalar(path, "SELECT status FROM orders WHERE account_id='acc1' AND order_id=1"),
              4);
    EXPECT_EQ(raw_scalar(path, "SELECT volume FROM orders WHERE account_id='acc1' AND order_id=1"),
              2);
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM trades"), 2);
    EXPECT_DOUBLE_EQ(raw_double(path, "SELECT price FROM trades WHERE trade_id='T2'"), 3901.5);
    // v3 重建保数据 + 类型搬运 (ASCII '70' -> 1, 文本枚举 '13' -> 13)
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM instruments"), 2);
    EXPECT_EQ(raw_scalar(path, "SELECT product_class FROM instruments WHERE instrument_id='IF2506'"),
              1);
    EXPECT_EQ(raw_scalar(path, "SELECT product_class FROM instruments WHERE instrument_id='AU9999'"),
              13);
    EXPECT_EQ(
        raw_scalar(path, "SELECT min_limit_order_volume FROM instruments WHERE instrument_id='IF2506'"),
        1);
    EXPECT_EQ(raw_scalar(path, "SELECT max_limit_order_volume FROM instruments WHERE instrument_id='IF2506'"),
              10);
    EXPECT_EQ(raw_scalar(path, "SELECT settle_cycle FROM instruments WHERE instrument_id='IF2506'"),
              -1);
    EXPECT_EQ(raw_string(path, "SELECT currency FROM instruments WHERE instrument_id='IF2506'"),
              "CNY");
    EXPECT_EQ(raw_string(path, "SELECT update_day FROM instruments WHERE instrument_id='IF2506'"),
              "20260726");
    // v4 改名 + 哨兵归零 + 新列默认值
    EXPECT_EQ(raw_scalar(path, "SELECT delisted_date FROM instruments WHERE instrument_id='IF2506'"),
              0);
    EXPECT_EQ(raw_string(path, "SELECT product_code FROM instruments WHERE instrument_id='IF2506'"),
              "");
    EXPECT_EQ(raw_scalar(path,
                         "SELECT min_market_order_volume FROM instruments WHERE instrument_id='IF2506'"),
              0);
    EXPECT_EQ(raw_scalar(path,
                         "SELECT max_market_order_volume FROM instruments WHERE instrument_id='IF2506'"),
              0);
    EXPECT_EQ(raw_scalar(path, "SELECT underlying_multiple FROM instruments WHERE instrument_id='IF2506'"),
              0);
    EXPECT_EQ(raw_scalar(path, "SELECT updated_at FROM instruments WHERE instrument_id='IF2506'"), 0);
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
    for (const char* name : {"product_class", "delisted_date", "product_code",
                             "min_market_order_volume", "max_market_order_volume",
                             "underlying_multiple", "updated_at"}) {
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
        EXPECT_THROW(db.exec(
                         "INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
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
