#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <dztrader/tdstore/schema_catalog.h>
#include <dztrader/tdstore/trade_db.h>

#include "test_temp_dir.h"

namespace dztrader::tdstore {
namespace {

/// 文件全文（二进制）读取：验证改名保留内容用
std::string read_file(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class TradeDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = dztrader::test::unique_temp_path("dz_trade_db_init", "db");
        std::filesystem::remove_all(path_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::remove_all(path_.string() + "-wal", ec);
        std::filesystem::remove_all(path_.string() + "-shm", ec);
        // 改名重建/隔离用例产生的 `.old-*` 备份（前缀含进程唯一后缀，不会误删并发用例）
        for (const std::string& backup : quarantine_backups()) {
            std::filesystem::remove_all(backup, ec);
        }
    }

    /// `<path>.old-*` 备份文件（含 `-wal`/`-shm` 伴生文件）
    std::vector<std::string> quarantine_backups() const {
        std::vector<std::string> backups;
        const std::filesystem::path directory = path_.parent_path();
        const std::string prefix = path_.filename().string() + ".old-";
        std::error_code ec;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(directory, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix, 0) == 0) {
                backups.push_back(entry.path().string());
            }
        }
        return backups;
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

TEST_F(TradeDbTest, QuarantineRenamesMainFileAndSidecars) {
    (void)ensure_trade_db(path_);
    // 模拟 WAL 伴生文件（连接已关闭）：主文件 + -wal/-shm 必须一起改名保留，不删除
    const std::string wal_content = "wal sidecar content";
    const std::string shm_content = "shm sidecar content";
    {
        std::ofstream out(path_.string() + "-wal", std::ios::binary | std::ios::trunc);
        out.write(wal_content.data(), static_cast<std::streamsize>(wal_content.size()));
    }
    {
        std::ofstream out(path_.string() + "-shm", std::ios::binary | std::ios::trunc);
        out.write(shm_content.data(), static_cast<std::streamsize>(shm_content.size()));
    }
    const std::uintmax_t main_size = std::filesystem::file_size(path_);

    const std::string backup = quarantine_trade_db(path_);

    EXPECT_EQ(backup.rfind(path_.string() + ".old-", 0), 0u);
    EXPECT_FALSE(std::filesystem::exists(path_));
    ASSERT_TRUE(std::filesystem::exists(backup));
    EXPECT_EQ(std::filesystem::file_size(backup), main_size);
    EXPECT_FALSE(std::filesystem::exists(path_.string() + "-wal"));
    EXPECT_FALSE(std::filesystem::exists(path_.string() + "-shm"));
    EXPECT_EQ(read_file(backup + "-wal"), wal_content);
    EXPECT_EQ(read_file(backup + "-shm"), shm_content);
}

TEST_F(TradeDbTest, NonRegularFilePathIsRetryableAndNeverQuarantined) {
    // db_path 指向目录：不是可恢复交易库 → 可重试错误；目录不得改名、不得建库
    std::filesystem::create_directories(path_);
    try {
        (void)ensure_trade_db(path_);
        FAIL() << "ensure_trade_db accepted a non-regular file path";
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("not a regular file"), std::string::npos) << message;
        EXPECT_NE(message.find(path_.string()), std::string::npos) << message;
    }
    EXPECT_TRUE(std::filesystem::is_directory(path_));
    EXPECT_TRUE(quarantine_backups().empty());
}

TEST_F(TradeDbTest, BusyDatabaseIsRetryableAndNeverQuarantined) {
    (void)ensure_trade_db(path_);
    // trade.db 默认 WAL，WAL 下 BEGIN EXCLUSIVE 不阻塞读者；切回 rollback journal 后
    // 另一连接持 EXCLUSIVE 才能确定性触发 probe 的 SQLITE_BUSY。
    {
        SQLite::Database db(path_.string(), SQLite::OPEN_READWRITE);
        EXPECT_EQ(db.execAndGet("PRAGMA journal_mode=DELETE").getString(), "delete");
    }
    ASSERT_FALSE(std::filesystem::exists(path_.string() + "-wal"));

    SQLite::Database locker(path_.string(), SQLite::OPEN_READWRITE);
    locker.exec("BEGIN EXCLUSIVE");
    try {
        (void)ensure_trade_db(path_);
        FAIL() << "ensure_trade_db did not report the exclusive lock as retryable";
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        EXPECT_TRUE(message.find("busy") != std::string::npos ||
                    message.find("locked") != std::string::npos)
            << message;
        EXPECT_NE(message.find("retryable"), std::string::npos) << message;
    }
    // 健康库未被改名、未被重建
    EXPECT_TRUE(std::filesystem::is_regular_file(path_));
    EXPECT_TRUE(quarantine_backups().empty());

    locker.exec("ROLLBACK");
    // 锁释放后同一库可正常使用（未被误判损坏重建）
    const TradeDbInitReport report = ensure_trade_db(path_);
    EXPECT_EQ(report.status, TradeDbInitReport::Status::Existing);
    EXPECT_EQ(user_version(), kTdSchemaVersion);
    EXPECT_EQ(table_count(), 5);
}

}  // namespace
}  // namespace dztrader::tdstore
