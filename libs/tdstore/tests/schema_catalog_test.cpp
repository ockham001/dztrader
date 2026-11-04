#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <span>
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

std::vector<std::string> business_key_names(const TableSchema& table) {
    std::vector<std::string> names;
    for (const std::string_view column : business_key_columns(table)) {
        names.emplace_back(column);
    }
    return names;
}

std::string join_columns(const std::vector<std::string>& columns) {
    std::string joined;
    for (const std::string& column : columns) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += column;
    }
    return joined;
}

bool is_key_column(std::span<const std::string_view> key_columns, std::string_view column) {
    return std::find(key_columns.begin(), key_columns.end(), column) != key_columns.end();
}

/// 按列类型绑定一行确定性测试值：键列取固定值（两次写入同键），非键列随 generation 变化。
void bind_upsert_row(SQLite::Statement& stmt, const TableSchema& table, int64_t generation) {
    const std::span<const std::string_view> key_columns = business_key_columns(table);
    for (const ColumnSchema& column : table.columns) {
        const std::string name = ":" + std::string(column.name);
        const bool key = is_key_column(key_columns, column.name);
        switch (column.type) {
            case ColumnType::Integer:
                stmt.bind(name, key ? static_cast<int64_t>(7) : generation);
                break;
            case ColumnType::Real:
                stmt.bind(name, key ? 7.5 : static_cast<double>(generation) + 0.25);
                break;
            case ColumnType::Text:
                stmt.bind(name, key ? std::string("key_") + std::string(column.name)
                                    : std::string(column.name) + "_g" + std::to_string(generation));
                break;
        }
    }
}

void execute_upsert(SQLite::Database& db, const TableSchema& table, int64_t generation) {
    SQLite::Statement stmt(db, build_upsert_sql(table));
    bind_upsert_row(stmt, table, generation);
    stmt.exec();
}

const ColumnSchema* first_non_key_column(const TableSchema& table) {
    const std::span<const std::string_view> key_columns = business_key_columns(table);
    for (const ColumnSchema& column : table.columns) {
        if (!is_key_column(key_columns, column.name)) {
            return &column;
        }
    }
    return nullptr;
}

// ============================================================================
// upsert SQL 精确形状契约（列序 = schema 声明序；键列 = business_key_columns；
// 参数 = :列名；SET 排除键列）。任何列/键/格式漂移都会让本断言失败。
// ============================================================================

constexpr std::string_view kExpectedOrdersUpsertSql =
    R"sql(INSERT INTO "orders" ("account_id", "date", "order_id", "strategy_id", "instrument_id", "exchange_id", "direction", "position_effect", "price_type", "status", "price", "volume", "volume_traded", "remark", "time", "avg_price", "seq", "gateway", "ext") VALUES (:account_id, :date, :order_id, :strategy_id, :instrument_id, :exchange_id, :direction, :position_effect, :price_type, :status, :price, :volume, :volume_traded, :remark, :time, :avg_price, :seq, :gateway, :ext) ON CONFLICT ("account_id", "date", "order_id") DO UPDATE SET "strategy_id"=excluded."strategy_id", "instrument_id"=excluded."instrument_id", "exchange_id"=excluded."exchange_id", "direction"=excluded."direction", "position_effect"=excluded."position_effect", "price_type"=excluded."price_type", "status"=excluded."status", "price"=excluded."price", "volume"=excluded."volume", "volume_traded"=excluded."volume_traded", "remark"=excluded."remark", "time"=excluded."time", "avg_price"=excluded."avg_price", "seq"=excluded."seq", "gateway"=excluded."gateway", "ext"=excluded."ext")sql";

constexpr std::string_view kExpectedTradesUpsertSql =
    R"sql(INSERT INTO "trades" ("account_id", "date", "trade_id", "order_id", "strategy_id", "instrument_id", "exchange_id", "direction", "position_effect", "price", "volume", "time", "seq", "gateway", "ext") VALUES (:account_id, :date, :trade_id, :order_id, :strategy_id, :instrument_id, :exchange_id, :direction, :position_effect, :price, :volume, :time, :seq, :gateway, :ext) ON CONFLICT ("account_id", "date", "exchange_id", "trade_id") DO UPDATE SET "order_id"=excluded."order_id", "strategy_id"=excluded."strategy_id", "instrument_id"=excluded."instrument_id", "direction"=excluded."direction", "position_effect"=excluded."position_effect", "price"=excluded."price", "volume"=excluded."volume", "time"=excluded."time", "seq"=excluded."seq", "gateway"=excluded."gateway", "ext"=excluded."ext")sql";

constexpr std::string_view kExpectedPositionsUpsertSql =
    R"sql(INSERT INTO "positions" ("account_id", "date", "instrument_id", "exchange_id", "direction", "volume", "frozen_volume", "today_volume", "yd_volume", "price", "seq", "gateway", "ext") VALUES (:account_id, :date, :instrument_id, :exchange_id, :direction, :volume, :frozen_volume, :today_volume, :yd_volume, :price, :seq, :gateway, :ext) ON CONFLICT ("account_id", "date", "instrument_id", "direction") DO UPDATE SET "exchange_id"=excluded."exchange_id", "volume"=excluded."volume", "frozen_volume"=excluded."frozen_volume", "today_volume"=excluded."today_volume", "yd_volume"=excluded."yd_volume", "price"=excluded."price", "seq"=excluded."seq", "gateway"=excluded."gateway", "ext"=excluded."ext")sql";

constexpr std::string_view kExpectedTradingAccountsUpsertSql =
    R"sql(INSERT INTO "trading_accounts" ("account_id", "date", "balance", "available", "frozen", "commission", "margin", "withdraw_quota", "deposit", "withdraw", "seq", "gateway", "ext") VALUES (:account_id, :date, :balance, :available, :frozen, :commission, :margin, :withdraw_quota, :deposit, :withdraw, :seq, :gateway, :ext) ON CONFLICT ("account_id", "date") DO UPDATE SET "balance"=excluded."balance", "available"=excluded."available", "frozen"=excluded."frozen", "commission"=excluded."commission", "margin"=excluded."margin", "withdraw_quota"=excluded."withdraw_quota", "deposit"=excluded."deposit", "withdraw"=excluded."withdraw", "seq"=excluded."seq", "gateway"=excluded."gateway", "ext"=excluded."ext")sql";

constexpr std::string_view kExpectedInstrumentsUpsertSql = R"sql(INSERT INTO "instruments" ("instrument_id", "exchange_id", "symbol", "name", "product_class", "settle_cycle", "settlement_method", "is_inverse", "currency", "base_asset", "min_limit_order_volume", "max_limit_order_volume", "volume_multiple", "price_tick", "volume_step", "listed_date", "delisted_date", "option_type", "option_exercise_style", "underlying_id", "option_strike", "option_series", "update_day", "product_code", "min_market_order_volume", "max_market_order_volume", "underlying_multiple", "updated_at") VALUES (:instrument_id, :exchange_id, :symbol, :name, :product_class, :settle_cycle, :settlement_method, :is_inverse, :currency, :base_asset, :min_limit_order_volume, :max_limit_order_volume, :volume_multiple, :price_tick, :volume_step, :listed_date, :delisted_date, :option_type, :option_exercise_style, :underlying_id, :option_strike, :option_series, :update_day, :product_code, :min_market_order_volume, :max_market_order_volume, :underlying_multiple, :updated_at) ON CONFLICT ("instrument_id") DO UPDATE SET "exchange_id"=excluded."exchange_id", "symbol"=excluded."symbol", "name"=excluded."name", "product_class"=excluded."product_class", "settle_cycle"=excluded."settle_cycle", "settlement_method"=excluded."settlement_method", "is_inverse"=excluded."is_inverse", "currency"=excluded."currency", "base_asset"=excluded."base_asset", "min_limit_order_volume"=excluded."min_limit_order_volume", "max_limit_order_volume"=excluded."max_limit_order_volume", "volume_multiple"=excluded."volume_multiple", "price_tick"=excluded."price_tick", "volume_step"=excluded."volume_step", "listed_date"=excluded."listed_date", "delisted_date"=excluded."delisted_date", "option_type"=excluded."option_type", "option_exercise_style"=excluded."option_exercise_style", "underlying_id"=excluded."underlying_id", "option_strike"=excluded."option_strike", "option_series"=excluded."option_series", "update_day"=excluded."update_day", "product_code"=excluded."product_code", "min_market_order_volume"=excluded."min_market_order_volume", "max_market_order_volume"=excluded."max_market_order_volume", "underlying_multiple"=excluded."underlying_multiple", "updated_at"=excluded."updated_at")sql";

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
    // SQLite 的 TEXT PRIMARY KEY 默认允许 NULL（契约 §4「非空」）：DDL 必须显式 NOT NULL
    SQLite::Database db(path_.string(), SQLite::OPEN_READONLY);
    SQLite::Statement info(db, "PRAGMA table_info(instruments)");
    bool instrument_id_not_null = false;
    while (info.executeStep()) {
        if (info.getColumn(1).getString() == "instrument_id") {
            instrument_id_not_null = info.getColumn(3).getInt() != 0;
        }
    }
    EXPECT_TRUE(instrument_id_not_null);
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

TEST_F(SchemaCatalogTest, BusinessKeyColumnsMatchDeclaredKeys) {
    EXPECT_EQ(business_key_names(orders_schema()),
              (std::vector<std::string>{"account_id", "date", "order_id"}));
    EXPECT_EQ(business_key_names(trades_schema()),
              (std::vector<std::string>{"account_id", "date", "exchange_id", "trade_id"}));
    EXPECT_EQ(business_key_names(positions_schema()),
              (std::vector<std::string>{"account_id", "date", "instrument_id", "direction"}));
    EXPECT_EQ(business_key_names(trading_accounts_schema()),
              (std::vector<std::string>{"account_id", "date"}));
    EXPECT_EQ(business_key_names(instruments_schema()),
              (std::vector<std::string>{"instrument_id"}));
}

TEST_F(SchemaCatalogTest, BusinessKeyColumnsMatchDatabaseConstraints) {
    // 业务键必须与真库强制的唯一键/PK 同序（生成 SQL 的冲突目标与约束是同一份声明）
    const std::vector<std::string> orders_keys = unique_key_signatures(path_, "orders");
    ASSERT_EQ(orders_keys.size(), 1u);
    EXPECT_EQ(join_columns(business_key_names(orders_schema())), orders_keys[0]);

    const std::vector<std::string> trades_keys = unique_key_signatures(path_, "trades");
    ASSERT_EQ(trades_keys.size(), 1u);
    EXPECT_EQ(join_columns(business_key_names(trades_schema())), trades_keys[0]);

    const std::vector<std::string> positions_keys = unique_key_signatures(path_, "positions");
    ASSERT_EQ(positions_keys.size(), 1u);
    EXPECT_EQ(join_columns(business_key_names(positions_schema())), positions_keys[0]);

    EXPECT_EQ(join_columns(business_key_names(trading_accounts_schema())),
              join_columns(primary_key_columns(path_, "trading_accounts")));
    EXPECT_EQ(join_columns(business_key_names(instruments_schema())),
              join_columns(primary_key_columns(path_, "instruments")));
}

TEST_F(SchemaCatalogTest, BuildUpsertSqlHasExactShape) {
    EXPECT_EQ(build_upsert_sql(orders_schema()), kExpectedOrdersUpsertSql);
    EXPECT_EQ(build_upsert_sql(trades_schema()), kExpectedTradesUpsertSql);
    EXPECT_EQ(build_upsert_sql(positions_schema()), kExpectedPositionsUpsertSql);
    EXPECT_EQ(build_upsert_sql(trading_accounts_schema()), kExpectedTradingAccountsUpsertSql);
    EXPECT_EQ(build_upsert_sql(instruments_schema()), kExpectedInstrumentsUpsertSql);
}

TEST_F(SchemaCatalogTest, UpsertOverwritesSameKeyOnRealDatabase) {
    SQLite::Database db(path_.string(), SQLite::OPEN_READWRITE);
    const TableSchema* cases[] = {&orders_schema(), &trades_schema(), &positions_schema(),
                                  &trading_accounts_schema(), &instruments_schema()};
    for (const TableSchema* table : cases) {
        // 同键写两次（非键列值不同）：第二次必须覆盖而非新增
        execute_upsert(db, *table, 1);
        execute_upsert(db, *table, 2);

        SQLite::Statement count(db, "SELECT COUNT(*) FROM \"" + std::string(table->name) + "\"");
        ASSERT_TRUE(count.executeStep()) << "table=" << table->name;
        EXPECT_EQ(count.getColumn(0).getInt(), 1)
            << "upsert created a duplicate row: table=" << table->name;

        const ColumnSchema* updated = first_non_key_column(*table);
        ASSERT_NE(updated, nullptr) << "table=" << table->name;
        SQLite::Statement select(db, "SELECT \"" + std::string(updated->name) + "\" FROM \"" +
                                         std::string(table->name) + "\"");
        ASSERT_TRUE(select.executeStep()) << "table=" << table->name;
        switch (updated->type) {
            case ColumnType::Integer:
                EXPECT_EQ(select.getColumn(0).getInt64(), 2) << "table=" << table->name;
                break;
            case ColumnType::Real:
                EXPECT_DOUBLE_EQ(select.getColumn(0).getDouble(), 2.25) << "table=" << table->name;
                break;
            case ColumnType::Text:
                EXPECT_EQ(select.getColumn(0).getString(), std::string(updated->name) + "_g2")
                    << "table=" << table->name;
                break;
        }
    }
}

TEST(SchemaCatalogStandaloneTest, BuildUpsertSqlWithoutBusinessKeyIsPlainInsert) {
    constexpr ColumnSchema kColumns[] = {
        {"a", ColumnType::Text, false, false, ""},
        {"b", ColumnType::Integer, false, false, ""},
    };
    const TableSchema no_key_table = {
        .name = "plain_insert",
        .columns = std::span<const ColumnSchema>(kColumns),
        .indexes = {},
        .expression_indexes = {},
    };
    EXPECT_TRUE(business_key_columns(no_key_table).empty());
    EXPECT_EQ(build_upsert_sql(no_key_table),
              "INSERT INTO \"plain_insert\" (\"a\", \"b\") VALUES (:a, :b)");
}

TEST(SchemaCatalogStandaloneTest, BuildUpsertSqlWithAllColumnsAsKeyUsesDoNothing) {
    constexpr ColumnSchema kColumns[] = {
        {"k1", ColumnType::Text, false, true, ""},
        {"k2", ColumnType::Integer, false, true, ""},
    };
    const TableSchema all_key_table = {
        .name = "all_key",
        .columns = std::span<const ColumnSchema>(kColumns),
        .indexes = {},
        .expression_indexes = {},
    };
    EXPECT_EQ(build_upsert_sql(all_key_table),
              "INSERT INTO \"all_key\" (\"k1\", \"k2\") VALUES (:k1, :k2) "
              "ON CONFLICT (\"k1\", \"k2\") DO NOTHING");
}

}  // namespace
}  // namespace dztrader::tdstore
