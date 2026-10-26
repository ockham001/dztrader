#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>
#include <dztrader/tdstore/schema_catalog.h>

#include "td/td_persist_records.h"
#include "td/td_prescan.h"
#include "test_temp_dir.h"

namespace dztrader::ctp {
namespace {

class TdPrescanTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = dztrader::test::unique_temp_dir("dz_td_prescan_test");
        std::filesystem::create_directories(tmp_dir_);
        db_path_ = (tmp_dir_ / "test.db").string();
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_ + "-journal");
        auto database = dztrader::db::Database::open(
            dztrader::db::Config{.backend = "sqlite", .options = {{"path", db_path_}}},
            dztrader::tdstore::schemas());
        database->migrate();
        database.reset();  // 释放句柄, TearDown 才能删临时文件; prescan 仍走独立 raw 连接
    }
    void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

    /// 已建 schema 的读写连接, 用于灌测试数据.
    SQLite::Database open_rw() {
        return SQLite::Database(db_path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    }

    /// 独立只读连接 (模拟 TdApi 预扫 / 运行期兜底的独立连接; busy_timeout 与
    /// TdApi::ensure_prescan_db 的只读连接一致, 防提交窗口内误报 database is locked).
    SQLite::Database open_ro() {
        SQLite::Database db(db_path_, SQLite::OPEN_READONLY);
        db.exec("PRAGMA busy_timeout=5000");
        return db;
    }

    std::filesystem::path tmp_dir_;
    std::string db_path_;
};

/// 插入 orders 行. direction/status 按写端语义存为 INTEGER (bind 侧 static_cast<int>).
void insert_order(SQLite::Database& db,
                  const std::string& acct,
                  int64_t order_id,
                  const char* order_ref,
                  int64_t seq,
                  int8_t status,
                  int32_t vol_traded,
                  int32_t vol_canceled,
                  int64_t update_time,
                  int8_t direction) {
    SQLite::Statement s(
        db,
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

void insert_trade(SQLite::Database& db,
                  const std::string& acct,
                  const char* trade_id,
                  int64_t seq,
                  int64_t order_id) {
    SQLite::Statement s(
        db,
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

void insert_position(SQLite::Database& db,
                     const std::string& acct,
                     const char* instrument,
                     int64_t seq,
                     int8_t direction) {
    SQLite::Statement s(
        db,
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
    SQLite::Statement s(
        db,
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
// query_max_seq: 四表取大 — 每张表各自成为最大值时都必须被计入.
// 拆四例 (而非只让 positions 当最大): 漏掉任一表时, 对应用例必然红 (§1.3).
// ============================================================================

TEST_F(TdPrescanTest, QueryMaxSeqOrdersIsMax) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", 1, "000000000001", 20, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "T001", 9, 1);
        insert_position(db, "acc1", "IF2506", 12, DZ_DIRECTION_LONG);
        insert_taccount(db, "acc1", 3);
    }
    auto ro = open_ro();
    EXPECT_EQ(query_max_seq(ro, "acc1"), 20u);
    EXPECT_EQ(query_max_seq(ro, "other"), 0u);  // 空账户 -> 0
}

TEST_F(TdPrescanTest, QueryMaxSeqTradesIsMax) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", 1, "000000000001", 5, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "T001", 20, 1);
        insert_position(db, "acc1", "IF2506", 12, DZ_DIRECTION_LONG);
        insert_taccount(db, "acc1", 3);
    }
    auto ro = open_ro();
    EXPECT_EQ(query_max_seq(ro, "acc1"), 20u);
}

TEST_F(TdPrescanTest, QueryMaxSeqPositionsIsMax) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", 1, "000000000001", 5, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "T001", 9, 1);
        insert_position(db, "acc1", "IF2506", 20, DZ_DIRECTION_LONG);
        insert_taccount(db, "acc1", 3);
    }
    auto ro = open_ro();
    EXPECT_EQ(query_max_seq(ro, "acc1"), 20u);
}

TEST_F(TdPrescanTest, QueryMaxSeqTradingAccountsIsMax) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", 1, "000000000001", 5, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "T001", 9, 1);
        insert_position(db, "acc1", "IF2506", 12, DZ_DIRECTION_LONG);
        insert_taccount(db, "acc1", 20);
    }
    auto ro = open_ro();
    EXPECT_EQ(query_max_seq(ro, "acc1"), 20u);
}

// ============================================================================
// 边界: 缺表 / 损坏库 / 并发写锁窗口 (§1.4)
// ============================================================================

// 缺表: 老库或半迁移库 (缺 trading_accounts) → 预扫抛异常, 由调用方降级
// (TdApi::prescan_all_accounts 清缓存 / load_boot_data 回落空基准), 不得读出错值.
TEST_F(TdPrescanTest, QueryMaxSeqThrowsOnMissingTable) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", 1, "000000000001", 5, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        db.exec("DROP TABLE trading_accounts");
    }
    auto ro = open_ro();
    EXPECT_THROW((void)query_max_seq(ro, "acc1"), std::exception);
}

// 损坏库: 非 SQLite 文件 → 预扫抛异常 (由调用方降级), 不得静默返回 0.
TEST_F(TdPrescanTest, QueryMaxSeqThrowsOnCorruptDatabase) {
    {
        std::ofstream garbage(db_path_, std::ios::binary | std::ios::trunc);
        garbage << "this is not a sqlite database";
    }
    EXPECT_THROW(
        {
            SQLite::Database ro(db_path_, SQLite::OPEN_READONLY);
            (void)query_max_seq(ro, "acc1");
        },
        std::exception);
}

// 并发写窗口: 预扫只读连接 (fixture open_ro 已设生产同款 busy_timeout=5000,
// 见 TdApi::ensure_prescan_db) — 另一连接持 EXCLUSIVE 锁 300ms 时预扫等待而非立即
// database is locked。回滚日志模式模拟 WAL 转换降级后的生产状态 (WAL 下写不阻塞读,
// 不构成该窗口)。锁持有方的 COMMIT 无条件释放 (无环等待, Windows 同样成立)。
TEST_F(TdPrescanTest, PrescanWaitsOutExclusiveLockWindow) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", 1, "000000000001", 7, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        db.exec("PRAGMA journal_mode=delete");
    }
    auto locker = open_rw();
    locker.exec("BEGIN EXCLUSIVE");
    bool release_ok = true;
    std::thread releaser([&locker, &release_ok] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        try {
            locker.exec("COMMIT");
        } catch (const std::exception&) {
            release_ok = false;
        }
    });

    auto ro = open_ro();
    uint64_t max_seq = 0;
    EXPECT_NO_THROW(max_seq = query_max_seq(ro, "acc1"));
    EXPECT_EQ(max_seq, 7u);

    releaser.join();
    EXPECT_TRUE(release_ok);
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

    // 字段完整性: order_id / order_ref / seq / direction(整数) / status / volume_canceled /
    // update_time / trading_day
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
    auto orders = load_orders_since(ro, "acc1", 1);  // seq > 1
    auto trades = load_trades_since(ro, "acc1", 2);  // seq > 2
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
    EXPECT_EQ(boot.at("acc1").start_seq, 6u);  // max(orders=5, trades=6)
    EXPECT_EQ(boot.at("acc1").orders.size(), 1u);
    EXPECT_EQ(boot.at("acc1").trades.size(), 1u);
    EXPECT_EQ(boot.at("acc2").start_seq, 3u);
    EXPECT_EQ(boot.at("acc3").start_seq, 0u);  // 无历史 → 空基准
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
