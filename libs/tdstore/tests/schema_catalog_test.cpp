#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>
#include <dztrader/tdstore/schema_catalog.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "test_temp_dir.h"

namespace {

std::vector<std::string> table_columns(const std::filesystem::path& path,
                                       const std::string& table) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement q(db, "PRAGMA table_info(" + table + ")");
    std::vector<std::string> columns;
    while (q.executeStep()) {
        columns.push_back(q.getColumn(1).getString());
    }
    return columns;
}

std::string join_fields(const std::vector<std::string_view>& fields) {
    std::string joined;
    for (const std::string_view field : fields) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += field;
    }
    return joined;
}

/// schema 声明的 unique 索引签名（字段序敏感），排序后便于集合比较
std::vector<std::string> unique_key_signatures(const dztrader::db::ResourceSchema& schema) {
    std::vector<std::string> signatures;
    for (const dztrader::db::IndexSchema& index : schema.indexes) {
        if (index.unique) {
            signatures.push_back(join_fields(index.fields));
        }
    }
    std::sort(signatures.begin(), signatures.end());
    return signatures;
}

std::vector<std::string> primary_key_fields(const dztrader::db::ResourceSchema& schema) {
    std::vector<std::string> fields;
    for (const dztrader::db::FieldSchema& field : schema.fields) {
        if (field.primary_key) {
            fields.emplace_back(field.name);
        }
    }
    return fields;
}

class SchemaCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = dztrader::test::unique_temp_path("dz_td_store", "db");
        auto database = dztrader::db::Database::open(
            dztrader::db::Config{.backend = "sqlite", .options = {{"path", path_.string()}}},
            dztrader::tdstore::schemas());
        database->migrate();
        database.reset();
    }
    void TearDown() override { std::filesystem::remove(path_); }
    std::filesystem::path path_;
};

TEST_F(SchemaCatalogTest, FieldsMatchMigratedColumns) {
    const std::pair<const dztrader::db::ResourceSchema*, const char*> cases[] = {
        {&dztrader::tdstore::orders_schema(), "orders"},
        {&dztrader::tdstore::trades_schema(), "trades"},
        {&dztrader::tdstore::positions_schema(), "positions"},
        {&dztrader::tdstore::trading_accounts_schema(), "trading_accounts"},
        {&dztrader::tdstore::instruments_schema(), "instruments"},
    };
    for (const auto& [schema, table] : cases) {
        std::vector<std::string> declared;
        declared.reserve(schema->fields.size());
        for (const auto& field : schema->fields) {
            declared.emplace_back(field.name);
        }
        EXPECT_EQ(declared, table_columns(path_, table)) << "schema drift: table=" << table;
    }
}

TEST_F(SchemaCatalogTest, UniqueKeysDeclared) {
    // 全部 schema 的键声明（含主键）：positions upsert 幂等依赖 uq_positions_key，
    // trading_accounts 以 account_id 为主键。新增 schema 或丢键都必须让本用例变红。
    struct ExpectedSchemaKeys {
        std::string_view table;
        std::vector<std::vector<std::string_view>> unique_keys;
        std::vector<std::string_view> primary_key;
    };
    const ExpectedSchemaKeys expected[] = {
        {.table = "orders", .unique_keys = {{"account_id", "order_id"}}, .primary_key = {"id"}},
        {.table = "trades",
         .unique_keys = {{"account_id", "trading_day", "trade_id"}},
         .primary_key = {"id"}},
        {.table = "positions",
         .unique_keys = {{"account_id", "instrument_id", "direction"}},
         .primary_key = {}},
        {.table = "trading_accounts", .unique_keys = {}, .primary_key = {"account_id"}},
        {.table = "instruments", .unique_keys = {}, .primary_key = {"instrument_id"}},
    };

    const std::span<const dztrader::db::ResourceSchema> schemas = dztrader::tdstore::schemas();
    ASSERT_EQ(schemas.size(), std::size(expected));

    for (const ExpectedSchemaKeys& keys : expected) {
        const auto schema = std::find_if(schemas.begin(), schemas.end(),
                                         [&keys](const dztrader::db::ResourceSchema& candidate) {
                                             return candidate.name == keys.table;
                                         });
        ASSERT_NE(schema, schemas.end()) << "missing schema: table=" << keys.table;

        std::vector<std::string> expected_unique;
        expected_unique.reserve(keys.unique_keys.size());
        for (const std::vector<std::string_view>& fields : keys.unique_keys) {
            expected_unique.push_back(join_fields(fields));
        }
        std::sort(expected_unique.begin(), expected_unique.end());
        EXPECT_EQ(unique_key_signatures(*schema), expected_unique)
            << "unique keys mismatch: table=" << keys.table;

        std::vector<std::string> expected_primary_key;
        expected_primary_key.reserve(keys.primary_key.size());
        for (const std::string_view field : keys.primary_key) {
            expected_primary_key.emplace_back(field);
        }
        EXPECT_EQ(primary_key_fields(*schema), expected_primary_key)
            << "primary key mismatch: table=" << keys.table;
    }
}

TEST_F(SchemaCatalogTest, SchemasContainFiveCollections) {
    EXPECT_EQ(dztrader::tdstore::schemas().size(), 5u);
}

}  // namespace
