#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>

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
           (name + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
            "_" + std::to_string(dist(gen)) + ".db");
}

int64_t raw_scalar(const std::filesystem::path& path, const std::string& sql) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement q(db, sql);
    return q.executeStep() ? q.getColumn(0).getInt64() : 0;
}

/// 删除 db 及附属文件 (Windows 下须先释放全部连接句柄再调用)
void remove_db_files(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    std::filesystem::remove(path.string() + "-journal", ec);
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
    auto database = Database::open(
        Config{.backend = "sqlite", .options = {{"path", path.string()}}}, test_schemas());
    database->migrate();
    EXPECT_EQ(raw_scalar(path, "SELECT COUNT(*) FROM schema_version WHERE version=5"), 1);
    EXPECT_GT(
        raw_scalar(path, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='orders'"),
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
    auto database = Database::open(
        Config{.backend = "sqlite", .options = {{"path", path.string()}}}, test_schemas());
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
    // schema 声明的索引必须真实落到库中（漏建索引时上述表存在断言仍会通过）
    EXPECT_EQ(raw_scalar(path,
                         "SELECT COUNT(*) FROM sqlite_master WHERE type='index' "
                         "AND name='idx_demo_ticks_instr'"),
              1);
    // 列名与声明类型逐列对照（列丢失或类型映射错误必须变红）
    const std::map<std::string, std::string> expected = {
        {"id", "INTEGER"}, {"instrument_id", "TEXT"}, {"price", "REAL"}};
    const auto columns = raw_columns(path, "demo_ticks");
    for (const auto& [name, type] : expected) {
        SCOPED_TRACE(name);
        ASSERT_EQ(columns.count(name), 1u);
        EXPECT_EQ(columns.at(name), type);
    }
    EXPECT_EQ(columns.size(), expected.size());
    database.reset();
    std::filesystem::remove(path);
}

// 库版本超前于代码: 必须显式拒绝而非静默 no-op（与 mysql 侧一致）
TEST(SqliteMigrateTest, SchemaVersionNewerThanCodeIsRejected) {
    const auto path = unique_db_path("dz_sqlite_migrate_newer");
    {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec(
            "CREATE TABLE schema_version (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)");
        db.exec(
            "INSERT INTO schema_version (version, applied_at) VALUES (6, '2026-01-01T00:00:00Z')");
    }
    auto database = Database::open(
        Config{.backend = "sqlite", .options = {{"path", path.string()}}}, test_schemas());
    try {
        database->migrate();
        FAIL() << "expected exception for newer schema version";
    } catch (const dztrader::Exception& e) {
        EXPECT_EQ(e.code(), DZ_EC_DB_MIGRATE_FAILED);
    }
    // 拒绝时不得写入任何迁移: 版本与表结构保持原样
    EXPECT_EQ(raw_scalar(path, "SELECT MAX(version) FROM schema_version"), 6);
    EXPECT_EQ(
        raw_scalar(path, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='orders'"),
        0);
    database.reset();
    remove_db_files(path);
}

TEST(SqliteMigrateTest, UnsupportedBackendThrows) {
    try {
        (void)Database::open(Config{.backend = "nope", .options = {}});
        FAIL() << "expected exception";
    } catch (const dztrader::Exception& e) {
        EXPECT_EQ(e.code(), DZ_EC_DB_UNSUPPORTED_BACKEND);
    }
}
