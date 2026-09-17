#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>
#include <dztrader/tdstore/schema_catalog.h>

#include <filesystem>
#include <random>

namespace {

std::filesystem::path unique_db_path() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           ("dz_td_schema_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
            "_" + std::to_string(dist(gen)) + ".db");
}

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

class SchemaCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = unique_db_path();
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
    bool orders_key = false;
    for (const auto& index : dztrader::tdstore::orders_schema().indexes) {
        if (index.unique && index.fields.size() == 2 && index.fields[0] == "account_id" &&
            index.fields[1] == "order_id") {
            orders_key = true;
        }
    }
    EXPECT_TRUE(orders_key);
    bool trades_key = false;
    for (const auto& index : dztrader::tdstore::trades_schema().indexes) {
        if (index.unique && index.fields.size() == 3 && index.fields[0] == "account_id" &&
            index.fields[1] == "trading_day" && index.fields[2] == "trade_id") {
            trades_key = true;
        }
    }
    EXPECT_TRUE(trades_key);
}

TEST_F(SchemaCatalogTest, SchemasContainFiveCollections) {
    EXPECT_EQ(dztrader::tdstore::schemas().size(), 5u);
}

}  // namespace
