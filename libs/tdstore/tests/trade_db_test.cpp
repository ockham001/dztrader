#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <dztrader/tdstore/schema_catalog.h>
#include <dztrader/tdstore/trade_db.h>

#include "test_temp_dir.h"

namespace dztrader::tdstore {
namespace {

class TradeDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = dztrader::test::unique_temp_path("dz_trade_db_init", "db");
        std::filesystem::remove(path_);
    }
    void TearDown() override {
        std::filesystem::remove(path_);
        std::filesystem::remove_all(path_.string() + "-wal");
        std::filesystem::remove_all(path_.string() + "-shm");
    }

    int user_version() const {
        SQLite::Database db(path_.string(), SQLite::OPEN_READONLY);
        return db.execAndGet("PRAGMA user_version").getInt();
    }

    int table_count() const {
        SQLite::Database db(path_.string(), SQLite::OPEN_READONLY);
        return db
            .execAndGet(
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
                "('orders','trades','positions','trading_accounts','instruments')")
            .getInt();
    }

    std::filesystem::path path_;
};

TEST_F(TradeDbTest, CreatesSchemaV1OnFirstStart) {
    const TradeDbInitReport report = ensure_trade_db(path_);
    EXPECT_EQ(report.status, TradeDbInitReport::Status::Created);
    EXPECT_TRUE(report.backup_path.empty());
    EXPECT_EQ(user_version(), kTdSchemaVersion);
    EXPECT_EQ(table_count(), 5);
}

TEST_F(TradeDbTest, RepeatedStartIsIdempotent) {
    (void)ensure_trade_db(path_);
    const TradeDbInitReport second = ensure_trade_db(path_);
    EXPECT_EQ(second.status, TradeDbInitReport::Status::Existing);
    EXPECT_EQ(second.previous_version, kTdSchemaVersion);
    EXPECT_TRUE(second.backup_path.empty());
    EXPECT_EQ(user_version(), kTdSchemaVersion);
}

TEST_F(TradeDbTest, OldSchemaVersionIsRenamedAndRebuilt) {
    {
        SQLite::Database db(path_.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec("CREATE TABLE orders (id INTEGER PRIMARY KEY, trading_day TEXT)");
        db.exec("PRAGMA user_version=5");
    }
    const TradeDbInitReport report = ensure_trade_db(path_);
    EXPECT_EQ(report.status, TradeDbInitReport::Status::Rebuilt);
    EXPECT_EQ(report.previous_version, 5);
    EXPECT_EQ(report.rebuild_reason, "schema_version_mismatch");
    ASSERT_FALSE(report.backup_path.empty());
    // 旧库改名保留（不删除）：备份文件仍是旧结构（orders 只有 2 列、版本 5）
    ASSERT_TRUE(std::filesystem::exists(report.backup_path));
    SQLite::Database backup(report.backup_path, SQLite::OPEN_READONLY);
    EXPECT_EQ(backup.execAndGet("PRAGMA user_version").getInt(), 5);
    EXPECT_EQ(backup.execAndGet("SELECT COUNT(*) FROM pragma_table_info('orders')").getInt(), 2);
    // 新库为 schema v1
    EXPECT_EQ(user_version(), kTdSchemaVersion);
    EXPECT_EQ(table_count(), 5);
}

TEST_F(TradeDbTest, CorruptFileIsRenamedAndRebuilt) {
    const std::string garbage = "not a sqlite database";
    {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }
    const TradeDbInitReport report = ensure_trade_db(path_);
    EXPECT_EQ(report.status, TradeDbInitReport::Status::Rebuilt);
    EXPECT_EQ(report.previous_version, -1);
    EXPECT_EQ(report.rebuild_reason, "corrupt_or_unreadable");
    ASSERT_FALSE(report.backup_path.empty());
    ASSERT_TRUE(std::filesystem::exists(report.backup_path));
    // 坏库原样保留（不删除、不覆盖）
    std::ifstream in(report.backup_path, std::ios::binary);
    const std::string preserved((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    EXPECT_EQ(preserved, garbage);
    EXPECT_EQ(user_version(), kTdSchemaVersion);
    EXPECT_EQ(table_count(), 5);
}

TEST_F(TradeDbTest, MissingTablesInVersionOneDbAreSelfHealed) {
    {
        SQLite::Database db(path_.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec("PRAGMA user_version=1");
    }
    const TradeDbInitReport report = ensure_trade_db(path_);
    EXPECT_EQ(report.status, TradeDbInitReport::Status::Existing);
    EXPECT_EQ(table_count(), 5);
}

TEST_F(TradeDbTest, ReadMaxOrderIdEmptyIsZero) {
    (void)ensure_trade_db(path_);
    EXPECT_EQ(read_max_order_id(path_), 0);
}

TEST_F(TradeDbTest, ReadMaxOrderIdUsesOrdersTable) {
    (void)ensure_trade_db(path_);
    {
        SQLite::Database db(path_.string(), SQLite::OPEN_READWRITE);
        SQLite::Statement insert(
            db,
            "INSERT INTO orders (account_id, date, order_id, instrument_id, exchange_id, seq, "
            "gateway, ext) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        for (const int64_t order_id : {11, 42, 7}) {
            insert.bind(1, std::string("acc1"));
            insert.bind(2, std::string("20261104"));
            insert.bind(3, order_id);
            insert.bind(4, std::string("rb2601"));
            insert.bind(5, std::string("SHFE"));
            insert.bind(6, order_id);
            insert.bind(7, std::string("dztd_ctp"));
            insert.bind(8, std::string("{}"));
            insert.exec();
            insert.reset();
        }
    }
    EXPECT_EQ(read_max_order_id(path_), 42);
}

}  // namespace
}  // namespace dztrader::tdstore
