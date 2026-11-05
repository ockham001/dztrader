#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <filesystem>
#include <span>
#include <string>

#include <dztrader/db/database.h>
#include <dztrader/db/types.h>

#include "test_temp_dir.h"

namespace dztrader::db {
namespace {

/// 复合主键 + NOT NULL 的声明（tdstore trading_accounts 的通用端口形态）。
const ResourceSchema& composite_primary_key_schema() {
    static const ResourceSchema kSchema = {
        .name = "composite_pk",
        .fields =
            {
                {.name = "account_id",
                 .type = ValueType::String,
                 .nullable = false,
                 .primary_key = true,
                 .auto_increment = false},
                {.name = "date",
                 .type = ValueType::String,
                 .nullable = false,
                 .primary_key = true,
                 .auto_increment = false},
                {.name = "balance",
                 .type = ValueType::Float64,
                 .nullable = true,
                 .primary_key = false,
                 .auto_increment = false},
            },
        .indexes = {},
    };
    return kSchema;
}

/// 单列 TEXT 主键 + NOT NULL（instruments 形态；TEXT PK 在 SQLite 默认允许 NULL，
/// 生成器必须显式补 NOT NULL）。
const ResourceSchema& single_text_primary_key_schema() {
    static const ResourceSchema kSchema = {
        .name = "single_pk",
        .fields =
            {
                {.name = "instrument_id",
                 .type = ValueType::String,
                 .nullable = false,
                 .primary_key = true,
                 .auto_increment = false},
                {.name = "price_tick",
                 .type = ValueType::Float64,
                 .nullable = false,
                 .primary_key = false,
                 .auto_increment = false},
            },
        .indexes = {},
    };
    return kSchema;
}

class SqliteSchemaTest : public ::testing::Test {
protected:
    std::filesystem::path path_;

    void SetUp() override { path_ = dztrader::test::unique_temp_path("dz_db_schema_test", "db"); }

    void TearDown() override { std::filesystem::remove(path_); }

    void migrate(const ResourceSchema& schema) {
        auto database =
            Database::open(Config{.backend = "sqlite", .options = {{"path", path_.string()}}},
                           std::span<const ResourceSchema>(&schema, 1));
        database->migrate();
    }
};

TEST_F(SqliteSchemaTest, CompositePrimaryKeyIsCreatedWithNotNullColumns) {
    migrate(composite_primary_key_schema());

    SQLite::Database db(path_.string(), SQLite::OPEN_READONLY);
    SQLite::Statement info(db, "PRAGMA table_info(composite_pk)");
    int primary_key_columns = 0;
    while (info.executeStep()) {
        const bool is_primary_key = info.getColumn(5).getInt() > 0;
        if (is_primary_key) {
            ++primary_key_columns;
            EXPECT_EQ(info.getColumn(3).getInt(), 1) << "primary key column must be NOT NULL";
        }
    }
    EXPECT_EQ(primary_key_columns, 2);
    // 非键列保持可空
    SQLite::Statement nullable(db, "PRAGMA table_info(composite_pk)");
    while (nullable.executeStep()) {
        if (nullable.getColumn(1).getString() == "balance") {
            EXPECT_EQ(nullable.getColumn(3).getInt(), 0);
        }
    }

    // 复合主键真实生效：同键二次插入被拒
    SQLite::Database write_db(path_.string(), SQLite::OPEN_READWRITE);
    write_db.exec(
        "INSERT INTO composite_pk (account_id, date, balance) VALUES ('acc', '20260901', 1.0)");
    EXPECT_THROW(
        write_db.exec("INSERT INTO composite_pk (account_id, date, balance) VALUES ('acc', "
                      "'20260901', 2.0)"),
        std::exception);
    EXPECT_NO_THROW(write_db.exec(
        "INSERT INTO composite_pk (account_id, date, balance) VALUES ('acc', '20260902', 2.0)"));
}

TEST_F(SqliteSchemaTest, SingleTextPrimaryKeyGetsNotNull) {
    migrate(single_text_primary_key_schema());

    SQLite::Database db(path_.string(), SQLite::OPEN_READWRITE);
    SQLite::Statement info(db, "PRAGMA table_info(single_pk)");
    bool instrument_not_null = false;
    while (info.executeStep()) {
        if (info.getColumn(1).getString() == "instrument_id") {
            instrument_not_null = info.getColumn(3).getInt() != 0;
        }
    }
    EXPECT_TRUE(instrument_not_null);
    EXPECT_THROW(db.exec("INSERT INTO single_pk (instrument_id, price_tick) VALUES (NULL, 1.0)"),
                 std::exception);
}

}  // namespace
}  // namespace dztrader::db
