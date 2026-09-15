#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/this_process.h>
#include <dztrader/db/migration.h>
#include <dztrader/tdstore/schema.h>

#include "td/td_persist_records.h"
#include "td/td_prescan.h"

namespace dztrader::ctp {
namespace {

/// 进程唯一临时目录名 (ctest -j 并行时避免测试 exe 共用固定目录名冲突)
std::filesystem::path unique_temp_dir(const std::string& name) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           (name + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
            "_" + std::to_string(dist(gen)));
}

class TdPrescanTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = unique_temp_dir("dz_td_prescan_test");
        std::filesystem::create_directories(tmp_dir_);
        db_path_ = (tmp_dir_ / "test.db").string();
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_ + "-journal");
    }
    void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

    /// 建 v2 schema (migration) 的读写连接, 用于灌测试数据.
    SQLite::Database open_rw() {
        SQLite::Database db(db_path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        dztrader::db::MigrationManager mgr;
        dztrader::tdstore::apply_td_migrations(mgr);
        mgr.apply(db);
        return db;
    }

    /// 独立只读连接 (模拟 TdApi 预扫 / 运行期兜底的独立连接).
    SQLite::Database open_ro() { return SQLite::Database(db_path_, SQLite::OPEN_READONLY); }

    std::filesystem::path tmp_dir_;
    std::string db_path_;
};

/// 插入 orders 行. direction/status 按写端语义存为 INTEGER (bind 侧 static_cast<int>).
void insert_order(SQLite::Database& db, const std::string& acct, int64_t order_id,
                  const char* order_ref, int64_t seq, int8_t status, int32_t vol_traded,
                  int32_t vol_canceled, int64_t update_time, int8_t direction) {
    SQLite::Statement s(db,
        "INSERT OR REPLACE INTO orders (account_id, trading_day, order_id, order_ref,"
        " instrument_id, exchange_id, direction, status, volume, volume_traded,"
        " volume_canceled, update_time, seq) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, acct);
    s.bind(2, "20260901");
    s.bind(3, static_cast<int64_t>(order_id));
    s.bind(4, order_ref);
    s.bind(5, "IF2506");
    s.bind(6, "CFFEX");
    s.bind(7, static_cast<int>(direction));
    s.bind(8, static_cast<int>(status));
    s.bind(9, 5);  // volume
    s.bind(10, vol_traded);
    s.bind(11, vol_canceled);
    s.bind(12, update_time);
    s.bind(13, static_cast<int64_t>(seq));
    s.exec();
}

void insert_trade(SQLite::Database& db, const std::string& acct, const char* trade_id,
                  int64_t seq, int64_t order_id) {
    SQLite::Statement s(db,
        "INSERT OR REPLACE INTO trades (account_id, trading_day, trade_id, order_id,"
        " instrument_id, exchange_id, direction, price, volume, strategy_id, seq)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, acct);
    s.bind(2, "20260901");
    s.bind(3, trade_id);
    s.bind(4, static_cast<int64_t>(order_id));
    s.bind(5, "IF2506");
    s.bind(6, "CFFEX");
    s.bind(7, 1);  // direction LONG
    s.bind(8, 3900.0);
    s.bind(9, 1);
    s.bind(10, "strat1");
    s.bind(11, static_cast<int64_t>(seq));
    s.exec();
}

void insert_position(SQLite::Database& db, const std::string& acct, const char* instrument,
                     int64_t seq, int8_t direction) {
    SQLite::Statement s(db,
        "INSERT OR REPLACE INTO positions (account_id, trading_day, instrument_id, exchange_id,"
        " direction, volume, frozen_volume, today_volume, yd_volume, price, seq)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, acct);
    s.bind(2, "20260901");
    s.bind(3, instrument);
    s.bind(4, "CFFEX");
    s.bind(5, static_cast<int>(direction));
    s.bind(6, 5);
    s.bind(7, 0);
    s.bind(8, 5);
    s.bind(9, 0);
    s.bind(10, 3900.0);
    s.bind(11, static_cast<int64_t>(seq));
    s.exec();
}

void insert_taccount(SQLite::Database& db, const std::string& acct, int64_t seq) {
    SQLite::Statement s(db,
        "INSERT OR REPLACE INTO trading_accounts (account_id, trading_day, balance, available,"
        " frozen, commission, margin, withdraw_quota, deposit, withdraw, seq)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, acct);
    s.bind(2, "20260901");
    s.bind(3, 100000.0);
    s.bind(4, 80000.0);
    s.bind(5, 5000.0);
    s.bind(6, 100.0);
    s.bind(7, 15000.0);
    s.bind(8, 70000.0);
    s.bind(9, 0.0);
    s.bind(10, 0.0);
    s.bind(11, static_cast<int64_t>(seq));
    s.exec();
}

// ============================================================================
// query_max_seq: 四表取最大 (orders/trades/positions/trading_accounts)
// ============================================================================

TEST_F(TdPrescanTest, QueryMaxSeqAcrossFourTables) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", 1, "000000000001", 5, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "T001", 9, 1);
        insert_position(db, "acc1", "IF2506", 12, DZ_DIRECTION_LONG);
        insert_taccount(db, "acc1", 3);
    }
    auto ro = open_ro();
    EXPECT_EQ(query_max_seq(ro, "acc1"), 12u);   // positions 最大 (四表取大)
    EXPECT_EQ(query_max_seq(ro, "other"), 0u);   // 空账户 -> 0
}

// ============================================================================
// load_orders / load_trades: 全量装载 (字段完整)
// ============================================================================

TEST_F(TdPrescanTest, LoadOrdersTradesFullReload) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", 1, "000000000001", 1, DZ_ORDER_NOT_TRADED, 0, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_order(db, "acc1", 2, "000000000002", 2, DZ_ORDER_ALL_TRADED, 5, 0, 2000,
                     DZ_DIRECTION_SHORT);
        insert_trade(db, "acc1", "T001", 3, 1);
        insert_trade(db, "acc1", "T002", 4, 2);
        insert_trade(db, "acc1", "T003", 5, 2);
        // 其他账户行不得混入
        insert_order(db, "other", 9, "000000000009", 9, DZ_ORDER_ALL_TRADED, 5, 0, 9000,
                     DZ_DIRECTION_LONG);
    }
    auto ro = open_ro();
    auto orders = load_orders(ro, "acc1");
    auto trades = load_trades(ro, "acc1");
    ASSERT_EQ(orders.size(), 2u);
    ASSERT_EQ(trades.size(), 3u);

    // 字段完整性: order_id / order_ref / seq / direction(整数) / status / volume_canceled / update_time / trading_day
    EXPECT_EQ(orders[0].base.order_id, 1);
    EXPECT_STREQ(orders[0].order_ref, "000000000001");
    EXPECT_EQ(orders[0].base.seq, 1u);
    EXPECT_EQ(orders[0].base.direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(orders[1].base.direction, DZ_DIRECTION_SHORT);
    EXPECT_EQ(orders[1].base.status, DZ_ORDER_ALL_TRADED);
    EXPECT_EQ(orders[1].base.volume_traded, 5);
    EXPECT_EQ(orders[1].volume_canceled, 0);
    EXPECT_EQ(orders[1].update_time, 2000);
    EXPECT_STREQ(orders[0].trading_day, "20260901");

    EXPECT_STREQ(trades[0].base.trade_id, "T001");
    EXPECT_EQ(trades[0].base.order_id, 1);
    EXPECT_EQ(trades[0].base.seq, 3u);
    EXPECT_STREQ(trades[2].base.trade_id, "T003");
}

// ============================================================================
// 运行期兜底 (config 变更后新增账户): 走同连接现查, 不得置零
// ============================================================================

TEST_F(TdPrescanTest, RuntimeAccountFallbackReturnsRealMax) {
    {
        auto db = open_rw();
        insert_order(db, "X", 1, "000000000001", 7, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "X", "TX1", 7, 1);
    }
    auto ro = open_ro();
    // 兜底查询 = query_max_seq + load: 返回真实历史 (seq=7), 不得置零
    EXPECT_EQ(query_max_seq(ro, "X"), 7u);
    EXPECT_EQ(load_orders(ro, "X").size(), 1u);
    EXPECT_EQ(load_trades(ro, "X").size(), 1u);
}

// ============================================================================
// §4.3 重连重建: 增量装载 (seq > 上次装载水位)
// ============================================================================

TEST_F(TdPrescanTest, LoadSinceReturnsIncrementalRows) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", 1, "000000000001", 1, DZ_ORDER_NOT_TRADED, 0, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_order(db, "acc1", 2, "000000000002", 2, DZ_ORDER_ALL_TRADED, 5, 0, 2000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "T001", 1, 1);
        insert_trade(db, "acc1", "T002", 2, 2);
        insert_trade(db, "acc1", "T003", 3, 2);
    }
    auto ro = open_ro();
    auto orders = load_orders_since(ro, "acc1", 1);   // seq > 1
    auto trades = load_trades_since(ro, "acc1", 2);   // seq > 2
    ASSERT_EQ(orders.size(), 1u);
    EXPECT_EQ(orders[0].base.order_id, 2);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_STREQ(trades[0].base.trade_id, "T003");
    EXPECT_EQ(load_orders_since(ro, "acc1", 100).size(), 0u);  // 水位之上无行
}

// ============================================================================
// prescan_accounts: 多账户预扫 → SessionBootData map (start_seq = MAX+1 语义由调用方取)
// ============================================================================

TEST_F(TdPrescanTest, PrescanAccountsBuildsBootDataPerAccount) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", 1, "000000000001", 5, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "T001", 6, 1);
        insert_order(db, "acc2", 2, "000000000002", 3, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
    }
    auto ro = open_ro();
    auto boot = prescan_accounts(ro, {"acc1", "acc2", "acc3"});
    ASSERT_EQ(boot.size(), 3u);
    EXPECT_EQ(boot.at("acc1").start_seq, 6u);   // max(orders=5, trades=6)
    EXPECT_EQ(boot.at("acc1").orders.size(), 1u);
    EXPECT_EQ(boot.at("acc1").trades.size(), 1u);
    EXPECT_EQ(boot.at("acc2").start_seq, 3u);
    EXPECT_EQ(boot.at("acc3").start_seq, 0u);    // 无历史 → 空基准
}

// ============================================================================
// direction 列读写一致性 (Task 3 注): 写端 static_cast<int>(direction) 存 INTEGER,
// 预扫读端必须按整数读, 不得按 CHAR/文本 ('1'/'L') 读
// ============================================================================

TEST_F(TdPrescanTest, LoadOrdersReadsDirectionAsInt) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", 1, "000000000001", 1, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_order(db, "acc1", 2, "000000000002", 2, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_SHORT);
    }
    auto ro = open_ro();
    auto orders = load_orders(ro, "acc1");
    ASSERT_EQ(orders.size(), 2u);
    // 整数读: 1 / -1 (DzDirection 值), 而非 CHAR '1'/'L'
    EXPECT_EQ(orders[0].base.direction, 1);
    EXPECT_EQ(orders[1].base.direction, -1);
}

}  // namespace
}  // namespace dztrader::ctp
