#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <nlohmann/json.hpp>

#include <dztrader/core/core_struct.h>
#include <dztrader/date_time/date.h>
#include <dztrader/tdstore/trade_db.h>

#include "td/td_day_load.h"
#include "td/td_persist_records.h"
#include "test_temp_dir.h"

namespace dztrader::ctp {
namespace {

class TdDayLoadTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = dztrader::test::unique_temp_dir("dz_td_day_load_test");
        std::filesystem::create_directories(tmp_dir_);
        db_path_ = (tmp_dir_ / "test.db").string();
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_ + "-journal");
        (void)dztrader::tdstore::ensure_trade_db(db_path_);
    }
    void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

    /// 已建 schema 的读写连接, 用于灌测试数据.
    SQLite::Database open_rw() {
        return SQLite::Database(db_path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    }

    /// 独立只读连接 (模拟 TdApi 的只读连接; busy_timeout 与生产一致).
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
                  const char* day,
                  int64_t order_id,
                  const char* order_ref,
                  int64_t seq,
                  int8_t status,
                  int32_t vol_traded,
                  int32_t vol_canceled,
                  int64_t update_time,
                  int8_t direction,
                  const nlohmann::json& extra_ext = nlohmann::json::object(),
                  double avg_price = 0.0) {
    nlohmann::json ext = extra_ext;
    ext["order_ref"] = order_ref;
    if (vol_canceled != 0) {
        ext["volume_canceled"] = vol_canceled;
    }
    if (update_time != 0) {
        ext["update_time"] = update_time;
    }
    SQLite::Statement s(db,
                        "INSERT OR REPLACE INTO orders (account_id, date, order_id, instrument_id,"
                        " exchange_id, direction, status, volume, volume_traded, seq, gateway, ext,"
                        " avg_price) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, acct);
    s.bind(2, day);
    s.bind(3, static_cast<int64_t>(order_id));
    s.bind(4, "IF2506");
    s.bind(5, "CFFEX");
    s.bind(6, static_cast<int>(direction));
    s.bind(7, static_cast<int>(status));
    s.bind(8, 5);  // volume
    s.bind(9, vol_traded);
    s.bind(10, static_cast<int64_t>(seq));
    s.bind(11, "dztd_test");
    s.bind(12, ext.dump());
    s.bind(13, avg_price);
    s.exec();
}

void insert_trade(SQLite::Database& db,
                  const std::string& acct,
                  const char* day,
                  const char* trade_id,
                  int64_t seq,
                  int64_t order_id,
                  double price = 3900.0,
                  int32_t volume = 1) {
    SQLite::Statement s(
        db,
        "INSERT OR REPLACE INTO trades (account_id, date, trade_id, order_id,"
        " instrument_id, exchange_id, direction, price, volume, strategy_id, seq, gateway)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, acct);
    s.bind(2, day);
    s.bind(3, trade_id);
    s.bind(4, static_cast<int64_t>(order_id));
    s.bind(5, "IF2506");
    s.bind(6, "CFFEX");
    s.bind(7, 1);  // direction LONG
    s.bind(8, price);
    s.bind(9, static_cast<int>(volume));
    s.bind(10, "strat1");
    s.bind(11, static_cast<int64_t>(seq));
    s.bind(12, "dztd_test");
    s.exec();
}

void insert_position(SQLite::Database& db,
                     const std::string& acct,
                     const char* day,
                     const char* instrument,
                     int64_t seq,
                     int8_t direction) {
    SQLite::Statement s(
        db,
        "INSERT OR REPLACE INTO positions (account_id, date, instrument_id, exchange_id,"
        " direction, volume, frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, acct);
    s.bind(2, day);
    s.bind(3, instrument);
    s.bind(4, "CFFEX");
    s.bind(5, static_cast<int>(direction));
    s.bind(6, 5);
    s.bind(7, 0);
    s.bind(8, 5);
    s.bind(9, 0);
    s.bind(10, 3900.0);
    s.bind(11, static_cast<int64_t>(seq));
    s.bind(12, "dztd_test");
    s.exec();
}

void insert_taccount(SQLite::Database& db, const std::string& acct, const char* day, int64_t seq) {
    SQLite::Statement s(
        db,
        "INSERT OR REPLACE INTO trading_accounts (account_id, date, balance, available,"
        " frozen, commission, margin, withdraw_quota, deposit, withdraw, seq, gateway)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, acct);
    s.bind(2, day);
    s.bind(3, 100000.0);
    s.bind(4, 80000.0);
    s.bind(5, 5000.0);
    s.bind(6, 100.0);
    s.bind(7, 15000.0);
    s.bind(8, 70000.0);
    s.bind(9, 0.0);
    s.bind(10, 0.0);
    s.bind(11, static_cast<int64_t>(seq));
    s.bind(12, "dztd_test");
    s.exec();
}

// ============================================================================
// query_max_seq: 四表取大 — 每张表各自成为最大值时都必须被计入.
// 拆四例 (而非只让 positions 当最大): 漏掉任一表时, 对应用例必然红 (§1.3).
// ============================================================================

TEST_F(TdDayLoadTest, QueryMaxSeqOrdersIsMax) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 20, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "20260901", "T001", 9, 1);
        insert_position(db, "acc1", "20260901", "IF2506", 12, DZ_DIRECTION_LONG);
        insert_taccount(db, "acc1", "20260901", 3);
    }
    auto ro = open_ro();
    EXPECT_EQ(query_max_seq(ro, "acc1"), 20u);
    EXPECT_EQ(query_max_seq(ro, "other"), 0u);  // 空账户 -> 0
}

TEST_F(TdDayLoadTest, QueryMaxSeqTradesIsMax) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 5, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "20260901", "T001", 20, 1);
        insert_position(db, "acc1", "20260901", "IF2506", 12, DZ_DIRECTION_LONG);
        insert_taccount(db, "acc1", "20260901", 3);
    }
    auto ro = open_ro();
    EXPECT_EQ(query_max_seq(ro, "acc1"), 20u);
}

TEST_F(TdDayLoadTest, QueryMaxSeqPositionsIsMax) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 5, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "20260901", "T001", 9, 1);
        insert_position(db, "acc1", "20260901", "IF2506", 20, DZ_DIRECTION_LONG);
        insert_taccount(db, "acc1", "20260901", 3);
    }
    auto ro = open_ro();
    EXPECT_EQ(query_max_seq(ro, "acc1"), 20u);
}

TEST_F(TdDayLoadTest, QueryMaxSeqTradingAccountsIsMax) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 5, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "20260901", "T001", 9, 1);
        insert_position(db, "acc1", "20260901", "IF2506", 12, DZ_DIRECTION_LONG);
        insert_taccount(db, "acc1", "20260901", 20);
    }
    auto ro = open_ro();
    EXPECT_EQ(query_max_seq(ro, "acc1"), 20u);
}

// ============================================================================
// 边界: 缺表 / 损坏库 / 并发写锁窗口 (§1.4)
// ============================================================================

// 缺表: 老库或半迁移库 (缺 trading_accounts) -> 装载抛异常, 由调用方按登录失败处理.
TEST_F(TdDayLoadTest, QueryMaxSeqThrowsOnMissingTable) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 5, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        db.exec("DROP TABLE trading_accounts");
    }
    auto ro = open_ro();
    EXPECT_THROW((void)query_max_seq(ro, "acc1"), std::exception);
}

// 损坏库: 非 SQLite 文件 -> 抛异常 (由调用方按登录失败处理), 不得静默返回 0.
TEST_F(TdDayLoadTest, QueryMaxSeqThrowsOnCorruptDatabase) {
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

// 并发写窗口: 只读连接 (fixture open_ro 已设生产同款 busy_timeout=5000).
// 另一连接持 EXCLUSIVE 锁 300ms 时装载等待而非立即 database is locked.
TEST_F(TdDayLoadTest, DayLoadWaitsOutExclusiveLockWindow) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 7, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
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
// 1. 登录日装载: 只返回当日行 (历史日不返回), 行序 = seq 升序
// ============================================================================

TEST_F(TdDayLoadTest, LoadTradingDayOnlyCurrentDay) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 11, DZ_ORDER_NOT_TRADED, 0, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_order(db, "acc1", "20260901", 2, "000000000002", 13, DZ_ORDER_ALL_TRADED, 5, 0, 2000,
                     DZ_DIRECTION_SHORT);
        insert_order(db, "acc1", "20260831", 3, "000000000003", 5, DZ_ORDER_ALL_TRADED, 5, 0, 900,
                     DZ_DIRECTION_LONG);  // 历史日
        insert_trade(db, "acc1", "20260901", "T001", 12, 1);
        insert_trade(db, "acc1", "20260831", "T002", 6, 3);  // 历史日
        insert_order(db, "other", "20260901", 9, "000000000009", 99, DZ_ORDER_ALL_TRADED, 5, 0,
                     9000, DZ_DIRECTION_LONG);  // 其他账户
    }
    auto ro = open_ro();
    auto day = load_trading_day(ro, "acc1", "20260901");

    ASSERT_EQ(day.orders.size(), 2u);
    EXPECT_EQ(day.orders[0].base.order_id, 1);
    EXPECT_EQ(day.orders[1].base.order_id, 2);  // 行序 = seq 升序 (11 -> 13)
    EXPECT_EQ(day.orders[0].base.seq, 11u);
    ASSERT_EQ(day.trades.size(), 1u);
    EXPECT_STREQ(day.trades[0].base.trade_id, "T001");
    // 历史日行不装载, 行数远小于全史
    EXPECT_EQ(day.orders.size() + day.trades.size(), 3u);
}

// ============================================================================
// 2. start_seq = 四表 MAX(seq) (跨日, 含历史日)
// ============================================================================

TEST_F(TdDayLoadTest, StartSeqIsFourTableMaxAcrossDays) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 30, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_order(db, "acc1", "20260831", 2, "000000000002", 50, DZ_ORDER_ALL_TRADED, 5, 0, 900,
                     DZ_DIRECTION_LONG);  // 历史日更大 seq
        insert_trade(db, "acc1", "20260830", "T009", 55, 2);
        insert_position(db, "acc1", "20260901", "IF2506", 40, DZ_DIRECTION_LONG);
        insert_taccount(db, "acc1", "20260901", 45);
    }
    auto ro = open_ro();
    auto day = load_trading_day(ro, "acc1", "20260901");
    EXPECT_EQ(day.start_seq, 55u);  // 跨日累积水位, 不因只装当日而回落
    ASSERT_EQ(day.orders.size(), 1u);
}

// ============================================================================
// 3. ext 恢复: order_ref / order_sys_id / update_time / volume_canceled / error_*
// ============================================================================

TEST_F(TdDayLoadTest, LoadRestoresOrderExt) {
    {
        auto db = open_rw();
        nlohmann::json ext = nlohmann::json::object();
        ext["order_sys_id"] = "123456";
        ext["error_id"] = 31;
        ext["error_msg"] = "ctp rejects";
        insert_order(db, "acc1", "20260901", 1, "000000000123", 8, DZ_ORDER_REJECTED, 0, 2, 1234,
                     DZ_DIRECTION_LONG, ext);
    }
    auto ro = open_ro();
    auto day = load_trading_day(ro, "acc1", "20260901");
    ASSERT_EQ(day.orders.size(), 1u);
    const auto& r = day.orders[0];
    EXPECT_STREQ(r.order_ref, "000000000123");
    EXPECT_STREQ(r.external_order_id, "123456");
    EXPECT_EQ(r.update_time, 1234);
    EXPECT_EQ(r.volume_canceled, 2);
    EXPECT_EQ(r.error_id, 31);
    EXPECT_STREQ(r.error_msg, "ctp rejects");
    EXPECT_FALSE(r.is_external);
}

// direction 列读端必须按整数读 (写端 static_cast<int>).
TEST_F(TdDayLoadTest, DirectionReadAsInt) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 1, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_order(db, "acc1", "20260901", 2, "000000000002", 2, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_SHORT);
    }
    auto ro = open_ro();
    auto day = load_trading_day(ro, "acc1", "20260901");
    ASSERT_EQ(day.orders.size(), 2u);
    EXPECT_EQ(day.orders[0].base.direction, 1);
    EXPECT_EQ(day.orders[1].base.direction, -1);
}

// ============================================================================
// 4. AccountSeqAllocator: 首用播种 DB MAX、连续 next 单调、current 反映已分配;
//    库不可用 (provider -> nullptr) 回落到 1 且不抛.
// ============================================================================

TEST_F(TdDayLoadTest, SeqAllocatorSeedsFromDbMaxAndIsMonotonic) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 7, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
        insert_trade(db, "acc1", "20260901", "T001", 9, 1);
    }
    auto ro = open_ro();
    AccountSeqAllocator alloc;
    alloc.set_db_provider([&ro]() -> SQLite::Database* { return &ro; });

    EXPECT_EQ(alloc.current("acc1"), 0u);  // 未分配过
    EXPECT_EQ(alloc.next("acc1"), 10u);    // DB MAX(seq)=9 -> 首个分配 10
    EXPECT_EQ(alloc.current("acc1"), 10u);
    EXPECT_EQ(alloc.next("acc1"), 11u);  // 只增不减
    EXPECT_EQ(alloc.current("acc1"), 11u);
    EXPECT_EQ(alloc.current("other"), 0u);  // 未用过的账户

    // 新分配器 (模拟 td 重启): 再次从 DB 播种, 不因内存丢失而回退
    AccountSeqAllocator fresh;
    fresh.set_db_provider([&ro]() -> SQLite::Database* { return &ro; });
    EXPECT_EQ(fresh.next("acc1"), 10u);
}

TEST_F(TdDayLoadTest, SeqAllocatorFallsBackWhenDbUnavailable) {
    AccountSeqAllocator alloc;
    alloc.set_db_provider([]() -> SQLite::Database* { return nullptr; });
    EXPECT_NO_THROW(EXPECT_EQ(alloc.next("acc1"), 1u));
    EXPECT_EQ(alloc.current("acc1"), 1u);
    EXPECT_EQ(alloc.next("acc1"), 2u);
}

// #13 AC4 的另一半: **空库** (该账户在库中没有任何行, W 不存在) 时, 无会话拒单仍必须拿到
// seq > 0 —— 从 1 起分配. 与上一条的区别: 上一条是「连接拿不到」(provider 返回 nullptr),
// 这条是「连接正常、库为空」(query_max_seq 返回 0), 是不同代码路径.
TEST_F(TdDayLoadTest, SeqAllocatorStartsAtOneForEmptyAccountInLiveDb) {
    auto ro = open_ro();
    AccountSeqAllocator alloc;
    alloc.set_db_provider([&ro]() -> SQLite::Database* { return &ro; });

    EXPECT_EQ(query_max_seq(ro, "acc_new"), 0u);  // 库可读, 但该账户无任何行
    EXPECT_EQ(alloc.next("acc_new"), 1u);         // 首个分配 = 1 (不是 0)
    EXPECT_EQ(alloc.next("acc_new"), 2u);         // 单调递增
    EXPECT_EQ(alloc.current("acc_new"), 2u);
    EXPECT_EQ(alloc.current("acc_never_used"), 0u);  // 未取号账户
}

// 会话在无会话拒单之后推进了 seq (DB 已提交) -> 分配器必须吸收该进展,
// 否则会话断线后的拒单会从旧种子续号, 与既有行撞 seq.
TEST_F(TdDayLoadTest, SeqAllocatorAbsorbsSessionProgress) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 3, DZ_ORDER_ALL_TRADED, 5, 0, 1000,
                     DZ_DIRECTION_LONG);
    }
    auto ro = open_ro();
    AccountSeqAllocator alloc;
    alloc.set_db_provider([&ro]() -> SQLite::Database* { return &ro; });
    EXPECT_EQ(alloc.next("acc1"), 4u);  // DB MAX(seq)=3
    EXPECT_EQ(alloc.current("acc1"), 4u);

    // 会话随后分配并提交 seq 5..9 (同一账户, 写入同一库)
    {
        auto db = open_rw();
        for (int i = 5; i <= 9; ++i) {
            insert_order(db, "acc1", "20260901", i, "000000000005", i, DZ_ORDER_ALL_TRADED, 5, 0,
                         1000, DZ_DIRECTION_LONG);
        }
    }
    EXPECT_EQ(alloc.next("acc1"), 10u);  // 吸收 DB MAX=9, 不撞号
}

// ============================================================================
// 5. build_no_session_reject: 字段齐全、strategy_id 原样、status=REJECTED、
//    remark、date/time 按入参
// ============================================================================

TEST_F(TdDayLoadTest, BuildNoSessionRejectFields) {
    DzOrderReq req{};
    req.order_id = 12345;
    std::strcpy(req.strategy_id, "strat1");
    std::strcpy(req.account_id, "acc1");
    std::strcpy(req.instrument_id, "IF2506");
    req.direction = DZ_DIRECTION_LONG;
    req.price_type = DZ_PRICE_LIMIT;
    req.position_effect = DZ_POSITION_EFFECT_OPEN;
    req.price = 3900.5;
    req.volume = 2;

    const int32_t day = dztrader::Date::from_year_month_day(2026, 9, 1).days_since_epoch();
    auto rec = build_no_session_reject(req, day, 9 * 3600 + 30 * 60 + 5, "账户未连接");

    EXPECT_EQ(rec.base.order_id, 12345);
    EXPECT_STREQ(rec.base.strategy_id, "strat1");
    EXPECT_STREQ(rec.base.account_id, "acc1");
    EXPECT_STREQ(rec.base.instrument_id, "IF2506");
    EXPECT_EQ(rec.base.direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(rec.base.price_type, DZ_PRICE_LIMIT);
    EXPECT_EQ(rec.base.position_effect, DZ_POSITION_EFFECT_OPEN);
    EXPECT_EQ(rec.base.price, 3900.5);
    EXPECT_EQ(rec.base.volume, 2);
    EXPECT_EQ(rec.base.volume_traded, 0);
    EXPECT_EQ(rec.base.status, DZ_ORDER_REJECTED);
    EXPECT_STREQ(rec.base.remark, "账户未连接");
    EXPECT_EQ(rec.base.date, day);
    EXPECT_EQ(rec.base.time, 9 * 3600 + 30 * 60 + 5);
    EXPECT_STREQ(rec.trading_day, "20260901");
    EXPECT_EQ(rec.volume_canceled, 2);
    EXPECT_EQ(rec.error_id, -1);                // 本地拒单
    EXPECT_STREQ(rec.error_msg, "账户未连接");  // NOLINT(log-format)
    // seq 由调用方分配, 构造器留 0
    EXPECT_EQ(rec.base.seq, 0u);
}

// 成交字段: 不写 trade_time/trade_date/commission (schema v1 无列).
TEST_F(TdDayLoadTest, TradeRecordOnlySchemaV1Fields) {
    {
        auto db = open_rw();
        insert_trade(db, "acc1", "20260901", "T001", 3, 1);
    }
    auto ro = open_ro();
    auto day = load_trading_day(ro, "acc1", "20260901");
    ASSERT_EQ(day.trades.size(), 1u);
    EXPECT_STREQ(day.trades[0].base.trade_id, "T001");
    EXPECT_EQ(day.trades[0].base.seq, 3u);
    EXPECT_STREQ(day.trades[0].trading_day, "20260901");
}

// format_day_text: DzDate -> "YYYYMMDD" 文本; 0/非法回落 "00000000"
// (登录日装载的 date 过滤值, 与拒单行 trading_day 列共用同一实现).
TEST_F(TdDayLoadTest, FormatDayText) {
    EXPECT_EQ(format_day_text(dztrader::Date::from_year_month_day(2026, 9, 1).days_since_epoch()),
              "20260901");
    EXPECT_EQ(format_day_text(0), "00000000");
    EXPECT_EQ(format_day_text(-1), "00000000");
}

// ============================================================================
// 6. OrderTradeAvgTracker: 每单成交累计 Σ价×量/Σ量 (契约 td-data-sync §3)
// ============================================================================

TEST(TdDayLoadOrderTradeAvgTest, SingleTradeAvgIsPrice) {
    OrderTradeAvgTracker t;
    t.add_trade(1, 3900.5, 3);
    ASSERT_TRUE(t.avg_price(1).has_value());
    EXPECT_DOUBLE_EQ(*t.avg_price(1), 3900.5);
    EXPECT_EQ(t.volume(1), 3);
}

TEST(TdDayLoadOrderTradeAvgTest, MultiTradeWeightedAvg) {
    OrderTradeAvgTracker t;
    t.add_trade(1, 3900.0, 1);  // 3900
    t.add_trade(1, 3903.0, 3);  // 11709
    // (3900*1 + 3903*3) / 4 = 15609 / 4 = 3902.25
    ASSERT_TRUE(t.avg_price(1).has_value());
    EXPECT_DOUBLE_EQ(*t.avg_price(1), 3902.25);
    EXPECT_EQ(t.volume(1), 4);
}

TEST(TdDayLoadOrderTradeAvgTest, OrdersIsolated) {
    OrderTradeAvgTracker t;
    t.add_trade(1, 3900.0, 2);
    t.add_trade(2, 4000.0, 5);
    EXPECT_DOUBLE_EQ(*t.avg_price(1), 3900.0);
    EXPECT_DOUBLE_EQ(*t.avg_price(2), 4000.0);
    EXPECT_EQ(t.volume(1), 2);
    EXPECT_EQ(t.volume(2), 5);
    EXPECT_FALSE(t.avg_price(3).has_value());  // 无成交
}

TEST(TdDayLoadOrderTradeAvgTest, NonPositiveVolumeIgnored) {
    OrderTradeAvgTracker t;
    t.add_trade(1, 3900.0, 0);   // 忽略
    t.add_trade(1, 3900.0, -2);  // 忽略
    EXPECT_FALSE(t.avg_price(1).has_value());
    EXPECT_EQ(t.volume(1), 0);
    t.add_trade(1, 3901.0, 1);
    EXPECT_DOUBLE_EQ(*t.avg_price(1), 3901.0);
    EXPECT_EQ(t.volume(1), 1);
}

TEST(TdDayLoadOrderTradeAvgTest, NonFinitePriceIgnored) {
    OrderTradeAvgTracker t;
    t.add_trade(1, std::numeric_limits<double>::quiet_NaN(), 1);
    t.add_trade(1, std::numeric_limits<double>::infinity(), 1);
    EXPECT_FALSE(t.avg_price(1).has_value());
    EXPECT_EQ(t.volume(1), 0);
    t.add_trade(1, 3900.0, 2);  // 有限价仍正常累计
    EXPECT_DOUBLE_EQ(*t.avg_price(1), 3900.0);
    EXPECT_EQ(t.volume(1), 2);
}

TEST(TdDayLoadOrderTradeAvgTest, NoTradesIsNulloptAndZeroVolume) {
    OrderTradeAvgTracker t;
    EXPECT_FALSE(t.avg_price(42).has_value());
    EXPECT_EQ(t.volume(42), 0);
}

TEST(TdDayLoadOrderTradeAvgTest, SeedAggregatesByOrderId) {
    std::vector<TradeRecord> trades;
    for (const auto& [order_id, price, volume] : std::vector<std::tuple<int64_t, double, int32_t>>{
             {1, 3900.0, 1}, {1, 3902.0, 3}, {2, 4000.0, 2}}) {
        TradeRecord r{};
        r.base.order_id = order_id;
        r.base.price = price;
        r.base.volume = volume;
        trades.push_back(r);
    }
    OrderTradeAvgTracker t;
    t.seed(trades);
    EXPECT_DOUBLE_EQ(*t.avg_price(1), 3901.5);  // (3900*1 + 3902*3) / 4
    EXPECT_EQ(t.volume(1), 4);
    EXPECT_DOUBLE_EQ(*t.avg_price(2), 4000.0);
    EXPECT_EQ(t.volume(2), 2);
}

TEST(TdDayLoadOrderTradeAvgTest, SeedAccumulatesOnExistingAcc) {
    OrderTradeAvgTracker t;
    t.add_trade(1, 4000.0, 1);
    TradeRecord r{};
    r.base.order_id = 1;
    r.base.price = 3900.0;
    r.base.volume = 1;
    t.seed({r});
    EXPECT_DOUBLE_EQ(*t.avg_price(1), 3950.0);  // 累加种子, 不重置
    EXPECT_EQ(t.volume(1), 2);
}

// ============================================================================
// 7. order_record_with_avg: 只改 base.avg_price, seq 与对比字段保持基准值
// ============================================================================

TEST(TdDayLoadOrderRecordWithAvgTest, OnlyAvgPriceChanges) {
    OrderRecord base{};
    base.base.order_id = 7;
    base.base.seq = 42;
    base.base.volume = 5;
    base.base.volume_traded = 2;
    base.base.status = DZ_ORDER_PART_TRADED;
    base.base.avg_price = 0.0;
    base.update_time = 1000;
    base.volume_canceled = 1;
    std::strcpy(base.trading_day, "20260901");
    std::strcpy(base.order_ref, "000000000042");

    const OrderRecord out = order_record_with_avg(base, 3901.5);
    EXPECT_DOUBLE_EQ(out.base.avg_price, 3901.5);
    // 对比字段 (重放去重基准) 与 seq 一律不变 — 只有调用方分配 seq
    EXPECT_EQ(out.base.seq, 42u);
    EXPECT_EQ(out.update_time, 1000);
    EXPECT_EQ(out.volume_canceled, 1);
    EXPECT_EQ(out.base.volume_traded, 2);
    EXPECT_EQ(out.base.status, DZ_ORDER_PART_TRADED);
    EXPECT_EQ(out.base.order_id, 7);
    EXPECT_STREQ(out.trading_day, "20260901");
    EXPECT_STREQ(out.order_ref, "000000000042");
    // 输入不被修改
    EXPECT_DOUBLE_EQ(base.base.avg_price, 0.0);
    EXPECT_EQ(base.base.seq, 42u);
}

// ============================================================================
// 8. load_trading_day 重建均价: 当日成交按 order_id 聚合覆盖装载委托 avg_price;
//    当日无成交的委托保留 DB 值 (不被清零; 跨日 GFD 昨日段不回填).
// ============================================================================

TEST_F(TdDayLoadTest, DayLoadRebuildsOrderAvgFromTrades) {
    {
        auto db = open_rw();
        // order 1: DB avg_price 是旧值 (昨日的 3900), 当日两笔成交 → 应被聚合均价覆盖
        insert_order(db, "acc1", "20260901", 1, "000000000001", 11, DZ_ORDER_PART_TRADED, 4, 0,
                     1000, DZ_DIRECTION_LONG, nlohmann::json::object(), 3900.0);
        insert_trade(db, "acc1", "20260901", "T001", 12, 1, 3900.0, 1);
        insert_trade(db, "acc1", "20260901", "T002", 13, 1, 3903.0, 3);
        // order 2: 当日无成交 (跨日 GFD 单) → 保留 DB 值, 不被清零
        insert_order(db, "acc1", "20260901", 2, "000000000002", 14, DZ_ORDER_NOT_TRADED, 0, 0, 2000,
                     DZ_DIRECTION_LONG, nlohmann::json::object(), 3888.5);
    }
    auto ro = open_ro();
    auto day = load_trading_day(ro, "acc1", "20260901");
    ASSERT_EQ(day.orders.size(), 2u);
    ASSERT_EQ(day.trades.size(), 2u);

    // 跟踪器 = 当日成交按 order_id 聚合
    ASSERT_TRUE(day.order_avgs.avg_price(1).has_value());
    EXPECT_DOUBLE_EQ(*day.order_avgs.avg_price(1), 3902.25);  // (3900*1 + 3903*3) / 4
    EXPECT_EQ(day.order_avgs.volume(1), 4);
    EXPECT_FALSE(day.order_avgs.avg_price(2).has_value());

    // 装载委托的 avg_price 与聚合均价一致
    EXPECT_DOUBLE_EQ(day.orders[0].base.avg_price, 3902.25);
    // 当日无成交的委托保留 DB 值
    EXPECT_DOUBLE_EQ(day.orders[1].base.avg_price, 3888.5);
}

TEST_F(TdDayLoadTest, DayLoadIgnoresHistoryDayTradesForAvg) {
    {
        auto db = open_rw();
        insert_order(db, "acc1", "20260901", 1, "000000000001", 11, DZ_ORDER_PART_TRADED, 4, 0,
                     1000, DZ_DIRECTION_LONG, nlohmann::json::object(), 3900.0);
        insert_trade(db, "acc1", "20260831", "T900", 9, 1, 3800.0, 5);  // 历史日成交
    }
    auto ro = open_ro();
    auto day = load_trading_day(ro, "acc1", "20260901");
    ASSERT_EQ(day.orders.size(), 1u);
    EXPECT_TRUE(day.trades.empty());
    // 历史日成交不参与均价重建 (契约 §3 只装当日)
    EXPECT_FALSE(day.order_avgs.avg_price(1).has_value());
    EXPECT_DOUBLE_EQ(day.orders[0].base.avg_price, 3900.0);  // 保留 DB 值
}

TEST_F(TdDayLoadTest, DayLoadEmptyAccountHasEmptyAvgTracker) {
    auto ro = open_ro();
    auto day = load_trading_day(ro, "acc_empty", "20260901");
    EXPECT_TRUE(day.orders.empty());
    EXPECT_TRUE(day.trades.empty());
    EXPECT_FALSE(day.order_avgs.avg_price(1).has_value());
    EXPECT_EQ(day.order_avgs.volume(1), 0);
}

// ============================================================================
// 9. avg_price_unchanged: 均价「变化」判定的浮点容差 (契约 §3)
//    累计值每次全量重算, 同价多笔有 1 ULP 级漂移; 裸 == 会误判成变化 → 多推 ORDER_REPORT.
// ============================================================================

TEST(TdDayLoadAvgPriceUnchangedTest, DriftedSamePriceIsUnchanged) {
    // 3512.3 非二进制精确: 3 笔同价全量重算得 3512.3000000000006 (Δ=+4.5e-13),
    // 6/7/8 笔得 3512.2999999999997 (Δ=-4.5e-13). 都不能判成「变化」.
    constexpr double kPrice = 3512.3;
    for (int n : {1, 2, 3, 4, 5, 6, 7, 8}) {
        OrderTradeAvgTracker tracker;
        for (int i = 0; i < n; ++i) {
            tracker.add_trade(1, kPrice, 1);
        }
        ASSERT_TRUE(tracker.avg_price(1).has_value()) << "n=" << n;
        const double avg = *tracker.avg_price(1);
        EXPECT_TRUE(avg_price_unchanged(avg, kPrice)) << "n=" << n << " avg=" << avg;
        EXPECT_TRUE(avg_price_unchanged(kPrice, avg)) << "n=" << n;  // 对称
    }
}

TEST(TdDayLoadAvgPriceUnchangedTest, OtherDriftingPricesAreUnchanged) {
    // 审查复现的另外两组: 3900.1×7, 3512.35×7
    for (double price : {3900.1, 3512.35}) {
        OrderTradeAvgTracker tracker;
        for (int i = 0; i < 7; ++i) {
            tracker.add_trade(1, price, 1);
        }
        ASSERT_TRUE(tracker.avg_price(1).has_value());
        EXPECT_TRUE(avg_price_unchanged(*tracker.avg_price(1), price)) << "price=" << price;
    }
}

TEST(TdDayLoadAvgPriceUnchangedTest, RealPriceDifferenceIsChanged) {
    // 真实价差 (>= 一个 tick) 必须判成变化
    EXPECT_FALSE(avg_price_unchanged(3512.3, 3512.35));
    EXPECT_FALSE(avg_price_unchanged(3900.0, 3900.5));  // tick=0.5 的合约
    EXPECT_FALSE(avg_price_unchanged(3900.0, 3901.0));
    EXPECT_FALSE(avg_price_unchanged(3500.0, 3500.001));  // 1 分钱量级
}

TEST(TdDayLoadAvgPriceUnchangedTest, ZeroAndNegativeValues) {
    EXPECT_TRUE(avg_price_unchanged(0.0, 0.0));
    EXPECT_TRUE(avg_price_unchanged(-0.0, 0.0));  // 同号零语义上无变化
    // 近零但真实变化: 容差下界 1.0 保证不会因相对误差塌成 0
    EXPECT_FALSE(avg_price_unchanged(0.0, 1e-6));
    EXPECT_TRUE(avg_price_unchanged(-3512.3, -3512.3000000000006));  // 负价对称
}

}  // namespace
}  // namespace dztrader::ctp
