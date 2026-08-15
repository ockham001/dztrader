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

class TdSchemaTest : public ::testing::Test {
protected:
    dztrader::db::Connection conn{":memory:"};
    dztrader::db::MigrationManager mgr;

    void SetUp() override {
        apply_td_migrations(mgr);
        auto applied = mgr.apply(conn.db());
        ASSERT_EQ(applied.size(), 1u);
        EXPECT_EQ(applied[0], kTdSchemaVersion);
    }
};

TEST_F(TdSchemaTest, AllTablesCreated) {
    EXPECT_TRUE(table_exists(conn, "schema_version"));
    EXPECT_TRUE(table_exists(conn, "orders"));
    EXPECT_TRUE(table_exists(conn, "trades"));
    EXPECT_TRUE(table_exists(conn, "margin_rates"));
    EXPECT_TRUE(table_exists(conn, "commission_rates"));
    EXPECT_TRUE(table_exists(conn, "instruments"));
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

}  // namespace
}  // namespace dztrader::ctp
