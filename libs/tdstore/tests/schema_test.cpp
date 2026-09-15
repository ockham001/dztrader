#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include <dztrader/db/connection.h>
#include <dztrader/db/migration.h>
#include <dztrader/tdstore/schema.h>

namespace dztrader::tdstore {
namespace {

bool table_exists(dztrader::db::Connection& conn, const std::string& name) {
    return conn.scalar<int>(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='" + name + "'") > 0;
}

int index_count(dztrader::db::Connection& conn, const std::string& table) {
    return conn.scalar<int>(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND tbl_name='" + table + "'");
}

bool index_exists(dztrader::db::Connection& conn, const std::string& name) {
    return conn.scalar<int>(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='" + name + "'") > 0;
}

std::string index_sql(dztrader::db::Connection& conn, const std::string& name) {
    return conn.scalar<std::string>(
        "SELECT sql FROM sqlite_master WHERE type='index' AND name='" + name + "'");
}

void expect_index_columns(dztrader::db::Connection& conn, const std::string& name,
                          const std::vector<std::string>& cols) {
    const auto sql = index_sql(conn, name);
    for (const auto& col : cols) {
        EXPECT_NE(sql.find(col), std::string::npos) << name << ": 缺少列 " << col;
    }
}

/// 列名 -> 声明类型 (PRAGMA table_info 的 name/type 列).
std::map<std::string, std::string> table_columns(dztrader::db::Connection& conn,
                                                 const std::string& table) {
    std::map<std::string, std::string> columns;
    SQLite::Statement q(conn.db(), "PRAGMA table_info(" + table + ")");
    while (q.executeStep()) {
        columns.emplace(q.getColumn(1).getString(), q.getColumn(2).getString());
    }
    return columns;
}

std::string column_type(const std::map<std::string, std::string>& columns,
                        const std::string& name) {
    auto it = columns.find(name);
    return it == columns.end() ? std::string("<missing>") : it->second;
}

class TdSchemaTest : public ::testing::Test {
protected:
    dztrader::db::Connection conn{":memory:"};
    dztrader::db::MigrationManager mgr;

    void SetUp() override {
        apply_td_migrations(mgr);
        auto applied = mgr.apply(conn.db());
        ASSERT_EQ(applied.size(), 4u);
        EXPECT_EQ(applied[3], kTdSchemaVersion);
    }
};

TEST_F(TdSchemaTest, AllTablesCreated) {
    EXPECT_TRUE(table_exists(conn, "schema_version"));
    EXPECT_TRUE(table_exists(conn, "orders"));
    EXPECT_TRUE(table_exists(conn, "trades"));
    EXPECT_TRUE(table_exists(conn, "margin_rates"));
    EXPECT_TRUE(table_exists(conn, "commission_rates"));
    EXPECT_TRUE(table_exists(conn, "instruments"));
    EXPECT_TRUE(table_exists(conn, "positions"));
    EXPECT_TRUE(table_exists(conn, "trading_accounts"));
}

TEST_F(TdSchemaTest, OrdersIndexesCreated) {
    EXPECT_GE(index_count(conn, "orders"), 3);
}

TEST_F(TdSchemaTest, TradesIndexesCreated) {
    EXPECT_GE(index_count(conn, "trades"), 3);
}

TEST_F(TdSchemaTest, ReApplyIsNoOp) {
    auto second = mgr.apply(conn.db());
    EXPECT_TRUE(second.empty());
}

TEST_F(TdSchemaTest, OrdersUniqueConstraintWorks) {
    conn.exec("INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
              "instrument_id, exchange_id) VALUES ('acc1', '20260726', 1, '001', 'IF2506', 'CFFEX')");
    EXPECT_THROW(
        conn.exec("INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
                  "instrument_id, exchange_id) VALUES ('acc1', '20260726', 1, '002', 'IF2506', 'CFFEX')"),
        SQLite::Exception);
    EXPECT_NO_THROW(
        conn.exec("INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
                  "instrument_id, exchange_id) VALUES ('acc2', '20260726', 1, '001', 'IF2506', 'CFFEX')"));
}

TEST_F(TdSchemaTest, InstrumentsPrimaryKeyWorks) {
    conn.exec("INSERT INTO instruments (instrument_id, exchange_id, volume_multiple, price_tick) "
              "VALUES ('IF2506', 'CFFEX', 300, 0.2)");
    EXPECT_THROW(
        conn.exec("INSERT INTO instruments (instrument_id, exchange_id, volume_multiple, price_tick) "
                  "VALUES ('IF2506', 'CFFEX', 300, 0.2)"),
        SQLite::Exception);
}

// ============================================================================
// v2: seq 列 + (account_id, seq) 索引 + trades 唯一键升级 + 新表
// ============================================================================

TEST_F(TdSchemaTest, OrdersTradesHaveSeqColumnAndIndex) {
    // (1) seq 列存在
    EXPECT_NO_THROW(conn.scalar<int>("SELECT seq FROM orders LIMIT 0"));
    EXPECT_NO_THROW(conn.scalar<int>("SELECT seq FROM trades LIMIT 0"));
    // (2) (account_id, seq) 索引存在且列内容正确 (防错建成 (seq))
    EXPECT_GE(index_count(conn, "orders"), 4);
    EXPECT_GE(index_count(conn, "trades"), 4);
    expect_index_columns(conn, "idx_orders_acct_seq", {"account_id", "seq"});
    expect_index_columns(conn, "idx_trades_acct_seq", {"account_id", "seq"});
}

TEST_F(TdSchemaTest, OrdersRebuildPreservesRowsAndIndexes) {
    // v1 建表插 2 行 -> 迁移 v2 -> 2 行仍在且 seq=0, 索引保留
    // 构造 v1 库: orders/trades 建 v1 表 (DDL 复制自 tdstore schema.cpp migration_v1) + 插数据
    dztrader::db::Connection legacy(":memory:");
    legacy.db().exec(
        "CREATE TABLE orders ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    account_id TEXT NOT NULL, trading_day TEXT NOT NULL, order_id INTEGER NOT NULL,"
        "    order_ref TEXT NOT NULL, external_order_id TEXT, is_external INTEGER NOT NULL DEFAULT 0,"
        "    instrument_id TEXT NOT NULL, exchange_id TEXT NOT NULL, direction CHAR(1),"
        "    position_effect CHAR(1), price_type CHAR(1), status CHAR(1), price REAL, volume INTEGER,"
        "    volume_traded INTEGER, volume_canceled INTEGER, insert_time INTEGER, update_time INTEGER,"
        "    error_id INTEGER, error_msg TEXT, strategy_id TEXT, remark TEXT,"
        "    UNIQUE(account_id, order_id))");
    legacy.db().exec("CREATE INDEX idx_orders_account_day ON orders(account_id, trading_day)");
    legacy.db().exec("CREATE INDEX idx_orders_day_instr ON orders(trading_day, instrument_id)");
    legacy.db().exec(
        "CREATE TABLE trades ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    account_id TEXT NOT NULL, trading_day TEXT NOT NULL, trade_id TEXT NOT NULL,"
        "    order_id INTEGER NOT NULL, instrument_id TEXT NOT NULL, exchange_id TEXT NOT NULL,"
        "    direction CHAR(1), position_effect CHAR(1), price REAL NOT NULL, volume INTEGER NOT NULL,"
        "    trade_time INTEGER, trade_date INTEGER, commission REAL, strategy_id TEXT,"
        "    UNIQUE(account_id, trade_id))");
    legacy.db().exec("CREATE INDEX idx_trades_day_instr ON trades(trading_day, instrument_id)");
    legacy.db().exec("CREATE INDEX idx_trades_account_day ON trades(account_id, trading_day)");
    // 插 2 行
    legacy.exec("INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
                "instrument_id, exchange_id) VALUES ('acc1', '20260726', 1, '001', 'IF2506', 'CFFEX')");
    legacy.exec("INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
                "instrument_id, exchange_id) VALUES ('acc1', '20260726', 2, '002', 'IF2506', 'CFFEX')");
    legacy.exec("INSERT INTO trades (account_id, trading_day, trade_id, order_id, "
                "instrument_id, exchange_id, price, volume) "
                "VALUES ('acc1', '20260726', 'T1', 1, 'IF2506', 'CFFEX', 3900.0, 1)");
    legacy.exec("INSERT INTO trades (account_id, trading_day, trade_id, order_id, "
                "instrument_id, exchange_id, price, volume) "
                "VALUES ('acc1', '20260726', 'T2', 2, 'IF2506', 'CFFEX', 3900.0, 1)");

    // 应用完整迁移: v1 (IF NOT EXISTS 对既有表 no-op) + v2 (四步重建保数据) + v3 + v4
    dztrader::db::MigrationManager mgr2;
    apply_td_migrations(mgr2);
    auto applied = mgr2.apply(legacy.db());
    ASSERT_EQ(applied.size(), 4u);

    EXPECT_EQ(legacy.scalar<int>("SELECT COUNT(*) FROM orders"), 2);
    EXPECT_EQ(legacy.scalar<int>("SELECT COALESCE(MAX(seq), 0) FROM orders"), 0);
    EXPECT_EQ(legacy.scalar<int>("SELECT COUNT(*) FROM trades"), 2);
    EXPECT_EQ(legacy.scalar<int>("SELECT COALESCE(MAX(seq), 0) FROM trades"), 0);
    EXPECT_GE(index_count(legacy, "orders"), 4);
    EXPECT_GE(index_count(legacy, "trades"), 4);
    // (account_id, seq) 索引列内容正确
    expect_index_columns(legacy, "idx_orders_acct_seq", {"account_id", "seq"});
    expect_index_columns(legacy, "idx_trades_acct_seq", {"account_id", "seq"});
    // v1 既有索引保留
    EXPECT_TRUE(index_exists(legacy, "idx_orders_account_day"));
    EXPECT_TRUE(index_exists(legacy, "idx_orders_day_instr"));
    EXPECT_TRUE(index_exists(legacy, "idx_trades_day_instr"));
    EXPECT_TRUE(index_exists(legacy, "idx_trades_account_day"));
}

TEST_F(TdSchemaTest, TradesUniqueKeyIncludesTradingDay) {
    // CTP TradeID 跨日重复必须共存 (旧 UNIQUE(account_id, trade_id) 会 REPLACE)
    // 插入同 account+trade_id 不同 trading_day 两行 -> 均成功
    conn.exec("INSERT INTO trades (account_id, trading_day, trade_id, order_id, "
              "instrument_id, exchange_id, price, volume) "
              "VALUES ('acc1', '20260726', 'T1', 1, 'IF2506', 'CFFEX', 3900.0, 1)");
    conn.exec("INSERT INTO trades (account_id, trading_day, trade_id, order_id, "
              "instrument_id, exchange_id, price, volume) "
              "VALUES ('acc1', '20260727', 'T1', 2, 'IF2506', 'CFFEX', 3900.0, 1)");
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM trades"), 2);
}

TEST_F(TdSchemaTest, NewTablesPositionsTradingAccounts) {
    // positions: UNIQUE(account_id, instrument_id, direction), 含 trading_day/seq 列
    conn.exec("INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id, "
              "direction, volume) VALUES ('acc1', '20260726', 'IF2506', 'CFFEX', 0, 1)");
    conn.exec("INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id, "
              "direction, volume) VALUES ('acc1', '20260726', 'IF2506', 'CFFEX', 1, 2)");
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM positions"), 2);
    // 同 account+instrument+direction 重复 -> 冲突 (REPLACE 不抛, 插入即冲突)
    EXPECT_THROW(
        conn.exec("INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id, "
                  "direction, volume) VALUES ('acc1', '20260726', 'IF2506', 'CFFEX', 0, 3)"),
        SQLite::Exception);
    // trading_accounts: PRIMARY KEY account_id, 含 seq 列
    conn.exec("INSERT INTO trading_accounts (account_id, trading_day, balance, available) "
              "VALUES ('acc1', '20260726', 100000.0, 50000.0)");
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM trading_accounts"), 1);
    EXPECT_EQ(conn.scalar<int>("SELECT COALESCE(MAX(seq), 0) FROM trading_accounts"), 0);
    // (account_id, seq) 索引列内容正确
    expect_index_columns(conn, "idx_positions_acct_seq", {"account_id", "seq"});
    expect_index_columns(conn, "idx_taccount_acct_seq", {"account_id", "seq"});
}

// ============================================================================
// v3: instruments 重建 (product INTEGER + v2 列) -> v4 改名/新增列
// ============================================================================

TEST_F(TdSchemaTest, InstrumentsV3MigratesAsciiProductText) {
    // v1 instruments: bind_instrument 以 static_cast<int>('F')=70 绑定 CHAR(1) 列,
    // TEXT affinity 实存文本 "70" (sqlite3 实证) — CASE 必须匹配 ASCII 文本
    dztrader::db::Connection legacy(":memory:");
    legacy.db().exec(
        "CREATE TABLE instruments ("
        "    instrument_id TEXT PRIMARY KEY, exchange_id TEXT NOT NULL, name TEXT,"
        "    product CHAR(1), volume_multiple INTEGER, price_tick REAL,"
        "    min_order_volume INTEGER, max_order_volume INTEGER, option_type CHAR(1),"
        "    option_strike REAL, option_underlying TEXT, option_listed INTEGER,"
        "    option_expiry INTEGER, update_day TEXT)");
    // 模拟 v1 写入路径的存储形态: product 文本 "70", option_type 文本 "1", 日期整数 -1
    legacy.db().exec(
        "INSERT INTO instruments VALUES ('SR509C4800','CZCE','SR509C4800','79',"
        "10,0.5,1,0,'1',4800.0,'SR509',-1,-1,'20260101')");
    legacy.db().exec(
        "INSERT INTO instruments VALUES ('rb2601','SHFE','rb','70',"
        "10,1,1,0,'0',0.0,'rb',-1,-1,'20260101')");

    dztrader::db::MigrationManager mgr2;
    apply_td_migrations(mgr2);
    auto applied = mgr2.apply(legacy.db());
    ASSERT_EQ(applied.size(), 4u);

    // product -> product_class (v4 改名): 文本 "79"(期权)->2 / "70"(期货)->1
    EXPECT_EQ(legacy.scalar<int>(
                  "SELECT product_class FROM instruments WHERE instrument_id='SR509C4800'"), 2);
    EXPECT_EQ(legacy.scalar<int>(
                  "SELECT product_class FROM instruments WHERE instrument_id='rb2601'"), 1);
    // option_type 文本 "1" 搬入 INTEGER 列后 affinity 转回整数 1 (CALL 信息不丢)
    EXPECT_EQ(legacy.scalar<int>("SELECT option_type FROM instruments WHERE instrument_id='SR509C4800'"), 1);
    // 旧日期哨兵 -1 -> 新 NA 0
    EXPECT_EQ(legacy.scalar<int>("SELECT listed_date FROM instruments WHERE instrument_id='rb2601'"), 0);
    // expiry_date -> delisted_date (v4 改名), -1 -> 0
    EXPECT_EQ(legacy.scalar<int>("SELECT delisted_date FROM instruments WHERE instrument_id='rb2601'"), 0);
    // 新列缺省语义
    EXPECT_EQ(legacy.scalar<int>("SELECT settle_cycle FROM instruments WHERE instrument_id='rb2601'"), -1);
    EXPECT_EQ(legacy.scalar<std::string>("SELECT currency FROM instruments WHERE instrument_id='rb2601'"), "CNY");
    EXPECT_EQ(legacy.scalar<std::string>("SELECT product_code FROM instruments WHERE instrument_id='rb2601'"), "");
    EXPECT_EQ(legacy.scalar<int>("SELECT min_market_order_volume FROM instruments WHERE instrument_id='rb2601'"), 0);
    EXPECT_EQ(legacy.scalar<int>("SELECT max_market_order_volume FROM instruments WHERE instrument_id='rb2601'"), 0);
    EXPECT_EQ(legacy.scalar<int>("SELECT underlying_multiple FROM instruments WHERE instrument_id='rb2601'"), 0);
    EXPECT_EQ(legacy.scalar<int>("SELECT updated_at FROM instruments WHERE instrument_id='rb2601'"), 0);
}

TEST_F(TdSchemaTest, V4ColumnNamesAndTypes) {
    const auto columns = table_columns(conn, "instruments");
    // 4 个改名后列 (声明类型随 v3 原列保留)
    EXPECT_EQ(column_type(columns, "product_class"), "INTEGER");
    EXPECT_EQ(column_type(columns, "min_limit_order_volume"), "INTEGER");
    EXPECT_EQ(column_type(columns, "max_limit_order_volume"), "INTEGER");
    EXPECT_EQ(column_type(columns, "delisted_date"), "INTEGER");
    // 5 个新增列
    EXPECT_EQ(column_type(columns, "product_code"), "TEXT");
    EXPECT_EQ(column_type(columns, "min_market_order_volume"), "INTEGER");
    EXPECT_EQ(column_type(columns, "max_market_order_volume"), "INTEGER");
    EXPECT_EQ(column_type(columns, "underlying_multiple"), "REAL");
    EXPECT_EQ(column_type(columns, "updated_at"), "INTEGER");
    // 旧列名已不存在
    EXPECT_EQ(columns.count("product"), 0u);
    EXPECT_EQ(columns.count("min_order_volume"), 0u);
    EXPECT_EQ(columns.count("max_order_volume"), 0u);
    EXPECT_EQ(columns.count("expiry_date"), 0u);
}

TEST_F(TdSchemaTest, VersionIsFour) {
    EXPECT_EQ(conn.scalar<int>("SELECT MAX(version) FROM schema_version"), 4);
    EXPECT_EQ(kTdSchemaVersion, 4);
}

}  // namespace
}  // namespace dztrader::tdstore
