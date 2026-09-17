#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>

#include <filesystem>
#include <random>

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

int64_t raw_scalar(const std::filesystem::path& path, const std::string& sql) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement q(db, sql);
    return q.executeStep() ? q.getColumn(0).getInt64() : 0;
}

const std::vector<ResourceSchema>& test_schemas() {
    static const std::vector<ResourceSchema> kSchemas = {
        ResourceSchema{.name = "instruments",
                       .fields = {{"instrument_id", ValueType::String, false, true, false},
                                  {"exchange_id", ValueType::String, false, false, false},
                                  {"updated_at", ValueType::Int64, false, false, false}},
                       .indexes = {}}};
    return kSchemas;
}

}  // namespace

TEST(SqliteMigrateTest, TdMigrationsCreateV5Schema) {
    const auto path = unique_db_path("dz_sqlite_migrate");
    auto database =
        Database::open(Config{.backend = "sqlite", .options = {{"path", path.string()}}},
                       test_schemas());
    database->migrate();
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM schema_version WHERE version=5"), 1);
    EXPECT_GT(raw_scalar(path,
                         "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='orders'"),
              0);
    EXPECT_GT(raw_scalar(path,
                         "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                         "AND name='trading_accounts'"),
              0);
    EXPECT_EQ(raw_scalar(path,
                         "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                         "AND name='margin_rates'"),
              0);
    database.reset();
    std::filesystem::remove(path);
}

TEST(SqliteMigrateTest, MigrateIsIdempotent) {
    const auto path = unique_db_path("dz_sqlite_migrate_idem");
    auto database =
        Database::open(Config{.backend = "sqlite", .options = {{"path", path.string()}}},
                       test_schemas());
    database->migrate();
    database->migrate();
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM schema_version"), 5);
    database.reset();
    std::filesystem::remove(path);
}

TEST(SqliteMigrateTest, AutoCreatesCollectionFromSchema) {
    const auto path = unique_db_path("dz_sqlite_autocreate");
    const std::vector<ResourceSchema> extra = {
        ResourceSchema{.name = "demo_ticks",
                       .fields = {{"id", ValueType::Int64, false, true, true},
                                  {"instrument_id", ValueType::String, false, false, false},
                                  {"price", ValueType::Float64, true, false, false}},
                       .indexes = {{"idx_demo_ticks_instr", {"instrument_id"}, false}}}};
    auto database =
        Database::open(Config{.backend = "sqlite", .options = {{"path", path.string()}}}, extra);
    database->migrate();
    EXPECT_GT(raw_scalar(path,
                         "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                         "AND name='demo_ticks'"),
              0);
    database.reset();
    std::filesystem::remove(path);
}

TEST(SqliteMigrateTest, UnsupportedBackendThrows) {
    try {
        (void)Database::open(Config{.backend = "nope", .options = {}});
        FAIL() << "expected exception";
    } catch (const dztrader::Exception& e) {
        EXPECT_EQ(e.code(), DZ_EC_DB_UNSUPPORTED_BACKEND);
    }
}
