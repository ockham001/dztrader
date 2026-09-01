#include <gtest/gtest.h>

#include <dztrader/db/connection.h>
#include <dztrader/db/migration.h>

#include "td/td_schema.h"

namespace dztrader::ctp {
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

class TdSchemaTest : public ::testing::Test {
protected:
    dztrader::db::Connection conn{":memory:"};
    dztrader::db::MigrationManager mgr;

    void SetUp() override {
        apply_td_migrations(mgr);
        auto applied = mgr.apply(conn.db());
        ASSERT_EQ(applied.size(), 2u);
        EXPECT_EQ(applied[1], kTdSchemaVersion);
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
    // (2) (account_id, seq) 索引存在
    EXPECT_GE(index_count(conn, "orders"), 4);
    EXPECT_GE(index_count(conn, "trades"), 4);
}

TEST_F(TdSchemaTest, OrdersRebuildPreservesRowsAndIndexes) {
    // v1 建表插 2 行 -> 迁移 v2 -> 2 行仍在且 seq=0, 索引保留
    // 构造 v1 库: orders/trades 建 v1 表 (DDL 复制自 td_schema.cpp migration_v1) + 插数据
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

    // 应用完整迁移: v1 (IF NOT EXISTS 对既有表 no-op) + v2 (四步重建保数据)
    dztrader::db::MigrationManager mgr2;
    dztrader::ctp::apply_td_migrations(mgr2);
    auto applied = mgr2.apply(legacy.db());
    ASSERT_EQ(applied.size(), 2u);

    EXPECT_EQ(legacy.scalar<int>("SELECT COUNT(*) FROM orders"), 2);
    EXPECT_EQ(legacy.scalar<int>("SELECT COALESCE(MAX(seq), 0) FROM orders"), 0);
    EXPECT_EQ(legacy.scalar<int>("SELECT COUNT(*) FROM trades"), 2);
    EXPECT_EQ(legacy.scalar<int>("SELECT COALESCE(MAX(seq), 0) FROM trades"), 0);
    EXPECT_GE(index_count(legacy, "orders"), 4);
    EXPECT_GE(index_count(legacy, "trades"), 4);
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
}

}  // namespace
}  // namespace dztrader::ctp
