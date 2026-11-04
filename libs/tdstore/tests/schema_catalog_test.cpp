#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/tdstore/schema_catalog.h>
#include <dztrader/tdstore/trade_db.h>

#include "test_temp_dir.h"

namespace dztrader::tdstore {
namespace {

std::vector<std::string> table_columns(const std::filesystem::path& path,
                                       const std::string& table) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement query(db, "PRAGMA table_info(" + table + ")");
    std::vector<std::string> columns;
    while (query.executeStep()) {
        columns.push_back(query.getColumn(1).getString());
    }
    return columns;
}

/// 主键列（PRAGMA table_info 的 pk 列 > 0，按声明序）
std::vector<std::string> primary_key_columns(const std::filesystem::path& path,
                                             const std::string& table) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement query(db, "PRAGMA table_info(" + table + ")");
    std::vector<std::pair<int, std::string>> keyed;
    while (query.executeStep()) {
        const int position = query.getColumn(5).getInt();
        if (position > 0) {
            keyed.emplace_back(position, query.getColumn(1).getString());
        }
    }
    std::sort(keyed.begin(), keyed.end());
    std::vector<std::string> columns;
    columns.reserve(keyed.size());
    for (const auto& [position, name] : keyed) {
        columns.push_back(name);
    }
    return columns;
}

/// 全部唯一键签名（含 PK 自动索引；字段序敏感，排序后集合比较）
std::vector<std::string> unique_key_signatures(const std::filesystem::path& path,
                                               const std::string& table) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    std::vector<std::string> signatures;
    {
        SQLite::Statement list(db, "PRAGMA index_list(" + table + ")");
        std::vector<std::string> unique_indexes;
        while (list.executeStep()) {
            const std::string name = list.getColumn(1).getString();
            if (list.getColumn(2).getInt() == 0 || name.rfind("sqlite_autoindex", 0) == 0) {
                continue;  // 非 unique / PK 自动索引（PK 由 primary_key_columns 单独断言）
            }
            unique_indexes.push_back(name);
        }
        for (const std::string& index : unique_indexes) {
            SQLite::Statement info(db, "PRAGMA index_info(" + index + ")");
            std::vector<std::pair<int, std::string>> keyed;
            while (info.executeStep()) {
                keyed.emplace_back(info.getColumn(0).getInt(), info.getColumn(2).getString());
            }
            std::sort(keyed.begin(), keyed.end());
            std::string signature;
            for (const auto& [seq, name] : keyed) {
                if (!signature.empty()) {
                    signature += ", ";
                }
                signature += name;
            }
            signatures.push_back(signature);
        }
    }
    std::sort(signatures.begin(), signatures.end());
    return signatures;
}

std::set<std::string> index_names(const std::filesystem::path& path, const std::string& table) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement list(db, "PRAGMA index_list(" + table + ")");
    std::set<std::string> names;
    while (list.executeStep()) {
        const std::string name = list.getColumn(1).getString();
        if (name.rfind("sqlite_autoindex", 0) != 0) {
            names.insert(name);
        }
    }
    return names;
}

std::string index_sql(const std::filesystem::path& path, std::string_view index_name) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement query(db, "SELECT sql FROM sqlite_master WHERE type='index' AND name=?");
    query.bind(1, std::string(index_name));
    return query.executeStep() ? query.getColumn(0).getString() : "";
}

std::vector<std::string> declared_columns(const TableSchema& schema) {
    std::vector<std::string> columns;
    columns.reserve(schema.columns.size());
    for (const ColumnSchema& column : schema.columns) {
        columns.emplace_back(column.name);
    }
    return columns;
}

class SchemaCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = dztrader::test::unique_temp_path("dz_trade_db_schema", "db");
        const TradeDbInitReport report = ensure_trade_db(path_);
        ASSERT_EQ(report.status, TradeDbInitReport::Status::Created);
    }
    void TearDown() override { std::filesystem::remove(path_); }

    std::filesystem::path path_;
};

TEST_F(SchemaCatalogTest, DeclaredColumnsMatchCreatedTables) {
    const TableSchema* cases[] = {&orders_schema(), &trades_schema(), &positions_schema(),
                                  &trading_accounts_schema(), &instruments_schema()};
    for (const TableSchema* schema : cases) {
        EXPECT_EQ(declared_columns(*schema), table_columns(path_, std::string(schema->name)))
            << "schema drift: table=" << schema->name;
    }
}

TEST_F(SchemaCatalogTest, SchemaVersionAndTables) {
    SQLite::Database db(path_.string(), SQLite::OPEN_READONLY);
    EXPECT_EQ(db.execAndGet("PRAGMA user_version").getInt(), kTdSchemaVersion);
    SQLite::Statement query(db,
                            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
                            "('orders','trades','positions','trading_accounts','instruments')");
    ASSERT_TRUE(query.executeStep());
    EXPECT_EQ(query.getColumn(0).getInt(), 5);
}

TEST_F(SchemaCatalogTest, OrdersColumnsAndKeys) {
    const std::vector<std::string> expected = {
        "account_id",  "date",      "order_id",        "strategy_id", "instrument_id",
        "exchange_id", "direction", "position_effect", "price_type",  "status",
        "price",       "volume",    "volume_traded",   "remark",      "time",
        "avg_price",   "seq",       "gateway",         "ext"};
    EXPECT_EQ(table_columns(path_, "orders"), expected);
    EXPECT_TRUE(primary_key_columns(path_, "orders").empty());
    const std::vector<std::string> signed_keys = unique_key_signatures(path_, "orders");
    ASSERT_EQ(signed_keys.size(), 1u);
    EXPECT_EQ(signed_keys[0], "account_id, date, order_id");
}

TEST_F(SchemaCatalogTest, TradesColumnsAndKeys) {
    const std::vector<std::string> expected = {
        "account_id",    "date",        "trade_id",  "order_id",        "strategy_id",
        "instrument_id", "exchange_id", "direction", "position_effect", "price",
        "volume",        "time",        "seq",       "gateway",         "ext"};
    EXPECT_EQ(table_columns(path_, "trades"), expected);
    const std::vector<std::string> signed_keys = unique_key_signatures(path_, "trades");
    ASSERT_EQ(signed_keys.size(), 1u);
    EXPECT_EQ(signed_keys[0], "account_id, date, exchange_id, trade_id");
}

TEST_F(SchemaCatalogTest, PositionsColumnsAndKeys) {
    const std::vector<std::string> expected = {
        "account_id", "date",   "instrument_id", "exchange_id",
        "direction",  "volume", "frozen_volume", "today_volume",
        "yd_volume",  "price",  "seq",           "gateway",
        "ext"};
    EXPECT_EQ(table_columns(path_, "positions"), expected);
    const std::vector<std::string> signed_keys = unique_key_signatures(path_, "positions");
    ASSERT_EQ(signed_keys.size(), 1u);
    EXPECT_EQ(signed_keys[0], "account_id, date, instrument_id, direction");
}

TEST_F(SchemaCatalogTest, TradingAccountsColumnsAndKeys) {
    const std::vector<std::string> expected = {
        "account_id",     "date",    "balance",  "available", "frozen",  "commission", "margin",
        "withdraw_quota", "deposit", "withdraw", "seq",       "gateway", "ext"};
    EXPECT_EQ(table_columns(path_, "trading_accounts"), expected);
    EXPECT_EQ(primary_key_columns(path_, "trading_accounts"),
              (std::vector<std::string>{"account_id", "date"}));
    EXPECT_TRUE(unique_key_signatures(path_, "trading_accounts").empty());
}

TEST_F(SchemaCatalogTest, InstrumentsRetained) {
    EXPECT_EQ(table_columns(path_, "instruments").size(), 28u);
    EXPECT_EQ(primary_key_columns(path_, "instruments"),
              (std::vector<std::string>{"instrument_id"}));
    EXPECT_TRUE(index_names(path_, "instruments").empty());
}

TEST_F(SchemaCatalogTest, IndexesDeclaredPerSpec) {
    EXPECT_EQ(index_names(path_, "orders"),
              (std::set<std::string>{"uq_orders_key", "idx_orders_order_id", "idx_orders_acct_seq",
                                     "idx_orders_account_date", "idx_orders_sort"}));
    EXPECT_EQ(
        index_names(path_, "trades"),
        (std::set<std::string>{"uq_trades_key", "idx_trades_acct_seq", "idx_trades_account_date",
                               "idx_trades_date_instr", "idx_trades_sort"}));
    EXPECT_EQ(index_names(path_, "positions"),
              (std::set<std::string>{"uq_positions_key", "idx_positions_acct_seq",
                                     "idx_positions_account_date", "idx_positions_date_instr"}));
    EXPECT_EQ(index_names(path_, "trading_accounts"),
              (std::set<std::string>{"idx_taccount_acct_seq", "idx_taccount_account_date"}));
}

TEST_F(SchemaCatalogTest, SortIndexesUse18AnchorExpression) {
    const std::string orders_sort = index_sql(path_, "idx_orders_sort");
    EXPECT_NE(orders_sort.find("(time + 21600) % 86400"), std::string::npos) << orders_sort;
    EXPECT_NE(orders_sort.find("DESC"), std::string::npos) << orders_sort;
    EXPECT_NE(orders_sort.find("order_id"), std::string::npos) << orders_sort;
    const std::string trades_sort = index_sql(path_, "idx_trades_sort");
    EXPECT_NE(trades_sort.find("(time + 21600) % 86400"), std::string::npos) << trades_sort;
    EXPECT_NE(trades_sort.find("trade_id"), std::string::npos) << trades_sort;
}

TEST_F(SchemaCatalogTest, RemovedLegacyColumnsAbsent) {
    const auto orders = table_columns(path_, "orders");
    for (const char* dropped :
         {"trading_day", "id", "order_ref", "external_order_id", "is_external", "volume_canceled",
          "insert_time", "update_time", "error_id", "error_msg"}) {
        EXPECT_EQ(std::find(orders.begin(), orders.end(), dropped), orders.end())
            << "legacy column still present: orders." << dropped;
    }
    const auto trades = table_columns(path_, "trades");
    for (const char* dropped : {"trading_day", "id", "trade_date", "commission"}) {
        EXPECT_EQ(std::find(trades.begin(), trades.end(), dropped), trades.end())
            << "legacy column still present: trades." << dropped;
    }
    for (const char* required : {"date", "avg_price", "gateway", "ext"}) {
        EXPECT_NE(std::find(orders.begin(), orders.end(), required), orders.end())
            << "required column missing: orders." << required;
    }
}

TEST_F(SchemaCatalogTest, BuildSchemaDdlIsIdempotent) {
    SQLite::Database db(path_.string(), SQLite::OPEN_READWRITE);
    EXPECT_NO_THROW(db.exec(build_schema_ddl()));
    EXPECT_EQ(db.execAndGet("PRAGMA user_version").getInt(), kTdSchemaVersion);
}

}  // namespace
}  // namespace dztrader::tdstore
