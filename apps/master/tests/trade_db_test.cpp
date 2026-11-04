/**
 * @file trade_db_test.cpp
 * @brief master 启动期交易库初始化：建库/重建 + order_id 计数器播种（ADR 0041/0042）
 */

#include "trade_db.h"

#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <filesystem>
#include <optional>
#include <string>

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/shm/order_id_meta.h>
#include <dztrader/tdstore/schema_catalog.h>
#include <dztrader/tdstore/trade_db.h>

namespace dztrader::master {
namespace {

/// 测试内临时覆盖 DZTRADER_HOME, 析构时恢复原值并清空路径缓存。
class ScopedHome {
public:
    ScopedHome()
        : home_(dztrader::test::unique_temp_dir("master_trade_db_test")),
          original_(dztrader::env::get("DZTRADER_HOME")) {
        std::filesystem::create_directories(home_);
        dztrader::env::set("DZTRADER_HOME", home_.string());
        dztrader::paths::reset_for_test();
    }

    ~ScopedHome() {
        dztrader::paths::reset_for_test();
        try {
            if (original_) {
                dztrader::env::set("DZTRADER_HOME", *original_);
            } else {
                dztrader::env::unset("DZTRADER_HOME");
            }
        } catch (...) {
        }
        std::filesystem::remove_all(home_);
    }

    ScopedHome(const ScopedHome&) = delete;
    ScopedHome& operator=(const ScopedHome&) = delete;

    const std::filesystem::path& home() const { return home_; }

private:
    std::filesystem::path home_;
    std::optional<std::string> original_;
};

class TradeDbStartupTest : public ::testing::Test {
protected:
    void SetUp() override {
        home_ = std::make_unique<ScopedHome>();
        path_ = dztrader::paths::trade_db();
    }
    void TearDown() override { home_.reset(); }

    int user_version() const {
        SQLite::Database db(path_.string(), SQLite::OPEN_READONLY);
        return db.execAndGet("PRAGMA user_version").getInt();
    }

    std::unique_ptr<ScopedHome> home_;
    std::filesystem::path path_;
};

TEST_F(TradeDbStartupTest, FirstStartCreatesDbAndSeedsCounter) {
    const TradeDbStartupResult result = initialize_trade_db();
    EXPECT_EQ(result.init.status, tdstore::TradeDbInitReport::Status::Created);
    EXPECT_TRUE(std::filesystem::exists(path_));
    EXPECT_EQ(user_version(), tdstore::kTdSchemaVersion);
    EXPECT_EQ(result.db_max_order_id, 0);
    EXPECT_GE(result.counter_after, 1);

    const shm::OrderIdMeta counter =
        shm::OrderIdMeta::open_only(dztrader::CHANNEL_NAME_ORDER_ID, dztrader::paths::shm());
    EXPECT_EQ(counter.current(), result.counter_after);
}

TEST_F(TradeDbStartupTest, RepeatedStartIsIdempotent) {
    const TradeDbStartupResult first = initialize_trade_db();
    const TradeDbStartupResult second = initialize_trade_db();
    EXPECT_EQ(first.init.status, tdstore::TradeDbInitReport::Status::Created);
    EXPECT_EQ(second.init.status, tdstore::TradeDbInitReport::Status::Existing);
    EXPECT_TRUE(second.init.backup_path.empty());
    EXPECT_EQ(second.counter_after, first.counter_after);  // ensure_at_least 单调不减
}

TEST_F(TradeDbStartupTest, SeedsCounterFromMaxOrderIdInExistingDb) {
    (void)tdstore::ensure_trade_db(path_);
    {
        SQLite::Database db(path_.string(), SQLite::OPEN_READWRITE);
        SQLite::Statement insert(
            db,
            "INSERT INTO orders (account_id, date, order_id, instrument_id, exchange_id, seq, "
            "gateway, ext) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        insert.bind(1, std::string("acc1"));
        insert.bind(2, std::string("20261104"));
        insert.bind(3, static_cast<int64_t>(100));
        insert.bind(4, std::string("rb2601"));
        insert.bind(5, std::string("SHFE"));
        insert.bind(6, static_cast<int64_t>(1));
        insert.bind(7, std::string("dztd_ctp"));
        insert.bind(8, std::string("{}"));
        insert.exec();
    }
    const TradeDbStartupResult result = initialize_trade_db();
    EXPECT_EQ(result.init.status, tdstore::TradeDbInitReport::Status::Existing);
    EXPECT_EQ(result.db_max_order_id, 100);
    EXPECT_GE(result.counter_after, 101);
}

TEST_F(TradeDbStartupTest, OldVersionDbIsRenamedAndRebuilt) {
    {
        SQLite::Database db(path_.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec("CREATE TABLE orders (id INTEGER PRIMARY KEY, trading_day TEXT)");
        db.exec("PRAGMA user_version=5");
    }
    const TradeDbStartupResult result = initialize_trade_db();
    EXPECT_EQ(result.init.status, tdstore::TradeDbInitReport::Status::Rebuilt);
    EXPECT_EQ(result.init.previous_version, 5);
    ASSERT_FALSE(result.init.backup_path.empty());
    EXPECT_TRUE(std::filesystem::exists(result.init.backup_path));
    EXPECT_EQ(user_version(), tdstore::kTdSchemaVersion);
}

}  // namespace
}  // namespace dztrader::master
