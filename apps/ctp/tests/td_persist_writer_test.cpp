#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/this_process.h>

#include "td/td_persist_writer.h"
#include "td/td_schema.h"

namespace dztrader::ctp {
namespace {

/// 进程唯一临时目录名（PID + 随机数）：ctest -j 并行时避免多个测试 exe
/// 共用固定目录名（如 dz_td_persist_test）导致 db 文件互相占用的冲突。
std::filesystem::path unique_temp_dir(const std::string& name) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           (name + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
            "_" + std::to_string(dist(gen)));
}

/// 辅助: 创建临时 db 路径
class TdPersistWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = unique_temp_dir("dz_td_persist_test");
        std::filesystem::create_directories(tmp_dir_);
        db_path_ = (tmp_dir_ / "test.db").string();
        // 清理旧文件
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_ + "-journal");
    }
    void TearDown() override {
        std::filesystem::remove_all(tmp_dir_);
    }

    /// 重新打开数据库只读查询 (验证持久化结果)
    int scalar_int(const std::string& sql) {
        SQLite::Database db(db_path_, SQLite::OPEN_READONLY);
        SQLite::Statement q(db, sql);
        if (q.executeStep()) {
            return q.getColumn(0).getInt();
        }
        return 0;
    }

    std::filesystem::path tmp_dir_;
    std::string db_path_;
};

// ============================================================================
// open + 表结构验证
// ============================================================================

TEST_F(TdPersistWriterTest, OpenCreatesAllTables) {
    {
        PersistWriter w(db_path_);
        w.open();
        // 验证表存在 (在 start_writer 前 db() 可用)
        auto& db = w.db();
        SQLite::Statement q(db,
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
            "name IN ('schema_version','orders','trades','margin_rates',"
            "'commission_rates','instruments','positions','trading_accounts')");
        ASSERT_TRUE(q.executeStep());
        EXPECT_EQ(q.getColumn(0).getInt(), 8);
    }
}

TEST_F(TdPersistWriterTest, DbAccessorThrowsAfterStartWriter) {
    PersistWriter w(db_path_);
    w.open();
    w.start_writer();
    EXPECT_THROW(w.db(), std::runtime_error);
    w.stop();
}

TEST_F(TdPersistWriterTest, DbAccessorThrowsBeforeOpen) {
    PersistWriter w(db_path_);
    EXPECT_THROW(w.db(), std::runtime_error);
}

// ============================================================================
// order_id 启动自检 (设计 §13 step 8)
// ============================================================================

TEST_F(TdPersistWriterTest, MaxOrderIdEmptyDbIsZero) {
    PersistWriter w(db_path_);
    w.open();
    EXPECT_EQ(w.max_order_id(), 0);
}

TEST_F(TdPersistWriterTest, MaxOrderIdAcrossTables) {
    PersistWriter w(db_path_);
    w.open();
    auto& db = w.db();
    // orders 两笔 + trades 一笔, max = 200 (来自 trades)
    db.exec("INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
            "instrument_id, exchange_id) "
            "VALUES ('acc1', '20260814', 42, '000000000042', 'IF2506', 'CFFEX')");
    db.exec("INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
            "instrument_id, exchange_id) "
            "VALUES ('acc1', '20260814', 100, '000000000100', 'IF2506', 'CFFEX')");
    db.exec("INSERT INTO trades (account_id, trading_day, trade_id, order_id, "
            "instrument_id, exchange_id, price, volume) "
            "VALUES ('acc1', '20260814', 'T1', 200, 'IF2506', 'CFFEX', 100.5, 1)");
    EXPECT_EQ(w.max_order_id(), 200);
}

TEST_F(TdPersistWriterTest, MaxOrderIdOrdersOnly) {
    PersistWriter w(db_path_);
    w.open();
    auto& db = w.db();
    db.exec("INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
            "instrument_id, exchange_id) "
            "VALUES ('acc1', '20260814', 77, '000000000077', 'IF2506', 'CFFEX')");
    EXPECT_EQ(w.max_order_id(), 77);
}

// ============================================================================
// 单条 + 批量持久化
// ============================================================================

TEST_F(TdPersistWriterTest, SingleOrderPersisted) {
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        OrderRecord r{};
        r.base.order_id = 100;
        std::strcpy(r.base.account_id, "acc1");
        std::strcpy(r.trading_day, "20260726");
        std::strcpy(r.order_ref, "000001");
        std::strcpy(r.external_order_id, "EXT001");
        r.is_external = 0;
        std::strcpy(r.base.instrument_id, "IF2506");
        std::strcpy(r.base.exchange_id, "CFFEX");
        r.base.direction = 0;
        r.base.position_effect = 1;
        r.base.price_type = 0;
        r.base.status = 4;
        r.base.price = 3900.0;
        r.base.volume = 1;
        r.base.volume_traded = 1;
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});

        w.stop();  // drain
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 1);
    EXPECT_EQ(scalar_int("SELECT order_id FROM orders WHERE account_id='acc1'"), 100);
}

TEST_F(TdPersistWriterTest, BatchOrdersPersisted) {
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        for (int i = 1; i <= 50; ++i) {
            OrderRecord r{};
            r.base.order_id = i;
            std::strcpy(r.base.account_id, "acc1");
            std::strcpy(r.trading_day, "20260726");
            std::strcpy(r.order_ref, "000001");
            std::strcpy(r.base.instrument_id, "IF2506");
            std::strcpy(r.base.exchange_id, "CFFEX");
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
        }
        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 50);
}

TEST_F(TdPersistWriterTest, TradePersisted) {
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        TradeRecord r{};
        std::strcpy(r.base.account_id, "acc1");
        std::strcpy(r.trading_day, "20260726");
        std::strcpy(r.base.trade_id, "T001");
        r.base.order_id = 100;
        std::strcpy(r.base.instrument_id, "IF2506");
        std::strcpy(r.base.exchange_id, "CFFEX");
        r.base.price = 3900.0;
        r.base.volume = 1;
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = r});

        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades"), 1);
}

TEST_F(TdPersistWriterTest, InstrumentPersisted) {
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        InstrumentRecord r{};
        std::strcpy(r.base.instrument_id, "IF2506");
        std::strcpy(r.base.exchange_id, "CFFEX");
        std::strcpy(r.base.name, "沪深300股指期货");
        r.base.product = 'F';
        r.base.volume_multiple = 300;
        r.base.price_tick = 0.2;
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Instrument, .data = r});

        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM instruments"), 1);
}

TEST_F(TdPersistWriterTest, MarginRatePersisted) {
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        MarginRateRecord r{};
        std::strcpy(r.account_id, "acc1");
        std::strcpy(r.product_code, "IF");
        std::strcpy(r.instrument_id, "IF2506");
        std::strcpy(r.exchange_id, "CFFEX");
        r.hedge_flag = 'S';
        r.long_margin_ratio_by_money = 0.10;
        r.date = 19635;  // DzDate (距纪元天数)
        w.enqueue(PersistTask{.kind = PersistTask::Kind::MarginRate, .data = r});

        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM margin_rates"), 1);
}

TEST_F(TdPersistWriterTest, CommissionRatePersisted) {
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        CommissionRateRecord r{};
        std::strcpy(r.account_id, "acc1");
        std::strcpy(r.product_code, "IF");
        std::strcpy(r.instrument_id, "IF2506");
        std::strcpy(r.exchange_id, "CFFEX");
        r.open_ratio_by_money = 0.000025;
        r.date = 19635;  // DzDate
        w.enqueue(PersistTask{.kind = PersistTask::Kind::CommissionRate, .data = r});

        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM commission_rates"), 1);
}

// ============================================================================
// 退出不遗漏 (stop drain 残留)
// ============================================================================

TEST_F(TdPersistWriterTest, StopDrainsResidualTasks) {
    {
        PersistWriter w(db_path_);
        w.open();
        // 不调 start_writer, 直接入队 (Writer 未启动, 队列堆积)
        for (int i = 1; i <= 100; ++i) {
            OrderRecord r{};
            r.base.order_id = i;
            std::strcpy(r.base.account_id, "acc1");
            std::strcpy(r.trading_day, "20260726");
            std::strcpy(r.order_ref, "000001");
            std::strcpy(r.base.instrument_id, "IF2506");
            std::strcpy(r.base.exchange_id, "CFFEX");
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
        }
        // 启动 Writer 并立即 stop (drain 残留)
        w.start_writer();
        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 100);
}

// ============================================================================
// INSERT OR REPLACE 去重 (RESTART 重传覆盖)
// ============================================================================

TEST_F(TdPersistWriterTest, DuplicateOrderReplacedWithLatestState) {
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        // 第一次: status=3 (PART_TRADED), volume_traded=1
        OrderRecord r1{};
        r1.base.order_id = 100;
        std::strcpy(r1.base.account_id, "acc1");
        std::strcpy(r1.trading_day, "20260726");
        std::strcpy(r1.order_ref, "000001");
        std::strcpy(r1.base.instrument_id, "IF2506");
        std::strcpy(r1.base.exchange_id, "CFFEX");
        r1.base.status = 3;
        r1.base.volume_traded = 1;
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r1});

        // 第二次: status=4 (ALL_TRADED), volume_traded=2 (RESTART 重传, 最新状态)
        OrderRecord r2 = r1;
        r2.base.status = 4;
        r2.base.volume_traded = 2;
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r2});

        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 1);  // 去重, 1 行
    EXPECT_EQ(scalar_int("SELECT status FROM orders WHERE order_id=100"), 4);  // 最新状态
    EXPECT_EQ(scalar_int("SELECT volume_traded FROM orders WHERE order_id=100"), 2);
}

// ============================================================================
// Writer 失败不崩溃
// ============================================================================

TEST_F(TdPersistWriterTest, WriterFailureDoesNotCrash) {
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        OrderRecord r1{};
        r1.base.order_id = 1;
        std::strcpy(r1.base.account_id, "acc1");
        std::strcpy(r1.trading_day, "20260726");
        std::strcpy(r1.order_ref, "000001");
        std::strcpy(r1.base.instrument_id, "IF2506");
        std::strcpy(r1.base.exchange_id, "CFFEX");
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r1});

        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 1);
}

// ============================================================================
// 队列硬上限阻塞
// ============================================================================

TEST_F(TdPersistWriterTest, QueueLimitBlocksEnqueue) {
    // 用小上限 (2) 测试阻塞逻辑
    PersistWriter w(db_path_, 2);
    w.open();
    // 不启动 Writer, 队列堆积

    OrderRecord r{};
    r.base.order_id = 1;
    std::strcpy(r.base.account_id, "acc1");
    std::strcpy(r.trading_day, "20260726");
    std::strcpy(r.order_ref, "000001");
    std::strcpy(r.base.instrument_id, "IF2506");
    std::strcpy(r.base.exchange_id, "CFFEX");

    // 入队 2 条 (达到上限), 用不同 order_id 避免 INSERT OR REPLACE 去重
    w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
    r.base.order_id = 2;
    w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});

    // 第三条应在另一线程阻塞
    auto future = std::async(std::launch::async, [&w, &r]() {
        OrderRecord r2 = r;
        r2.base.order_id = 3;
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r2});
    });

    // 等待 200ms, 验证第三条仍在阻塞 (future 未完成)
    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);

    // 启动 Writer, drain 队列, 第三条应完成
    w.start_writer();
    EXPECT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    w.stop();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 3);
}

// ============================================================================
// flush 屏障 (FIFO 哨兵: 入队哨兵保证此前任务先提交)
// ============================================================================

TEST_F(TdPersistWriterTest, FlushWaitsForPriorTasksCommitted) {
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        OrderRecord r1{};
        r1.base.order_id = 1;
        std::strcpy(r1.base.account_id, "acc1");
        std::strcpy(r1.trading_day, "20260901");
        std::strcpy(r1.order_ref, "000001");
        std::strcpy(r1.base.instrument_id, "IF2506");
        std::strcpy(r1.base.exchange_id, "CFFEX");
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r1});

        OrderRecord r2 = r1;
        r2.base.order_id = 2;
        std::strcpy(r2.order_ref, "000002");
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r2});

        auto token = w.enqueue_flush_signal();
        EXPECT_TRUE(w.wait_flush(token, std::chrono::seconds(2)));
        // 此刻 DB 已含 2 行 (此前任务必已提交 — FIFO 哨兵语义)
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 2);

        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 2);
}

// stop() 后入队丢弃路径须立即 set: 哨兵被"消费"即视为提交完成, wait 恒 true
TEST_F(TdPersistWriterTest, FlushAfterStopSucceedsImmediately) {
    PersistWriter w(db_path_);
    w.open();
    w.start_writer();
    auto token = w.enqueue_flush_signal();
    EXPECT_TRUE(w.wait_flush(token, std::chrono::seconds(2)));
    w.stop();
    // stop() 后再入队哨兵: 丢弃分支须立即 set_value, 不得死锁/超时假负
    auto token2 = w.enqueue_flush_signal();
    EXPECT_TRUE(w.wait_flush(token2, std::chrono::milliseconds(500)));
}

// ============================================================================
// positions / trading_accounts 新 Kind
// ============================================================================

namespace {

/// 构造持仓记录 (DzPositionInfo). direction: DZ_DIRECTION_LONG=1 / SHORT=-1.
DzPositionInfo make_position(std::int64_t volume, std::int64_t seq, int8_t direction) {
    DzPositionInfo p{};
    std::strcpy(p.instrument_id, "IF2506");
    std::strcpy(p.exchange_id, "CFFEX");
    std::strcpy(p.account_id, "acc1");
    p.volume = volume;
    p.frozen_volume = 0;
    p.price = 3900.0;
    p.yd_volume = 0;
    p.today_volume = volume;
    p.direction = direction;
    p.seq = static_cast<uint64_t>(seq);
    return p;
}

/// 构造账户资金记录 (DzTradingAccount).
DzTradingAccount make_taccount(double balance, std::uint64_t seq) {
    DzTradingAccount a{};
    std::strcpy(a.account_id, "acc1");
    a.balance = balance;
    a.available = balance;
    a.frozen = 0;
    a.commission = 0;
    a.margin = 0;
    a.withdraw_quota = balance;
    a.deposit = 0;
    a.withdraw = 0;
    a.seq = seq;
    return a;
}

}  // namespace

// PositionRebuild 单事务重灌: 清该账户旧日行 + upsert 本组 (spec §3.2 原子性)
TEST_F(TdPersistWriterTest, PositionRebuildAtomicClearsStaleDays) {
    // DzDate (距纪元天数) = "YYYYMMDD" 的 days-since-epoch, 与生产 trading_day_ 语义一致.
    // 20696=20260831, 20697=20260901 (见 date_time Date{2026,8,31}.days_since_epoch()).
    constexpr int64_t kOldDay = 20696;
    constexpr int64_t kNewDay = 20697;
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        // 先灌 trading_day=20260831 的 1 行 (旧日)
        {
            auto p = make_position(5, 10, DZ_DIRECTION_LONG);
            PersistTask t{.kind = PersistTask::Kind::Position,
                          .data = std::vector<DzPositionInfo>{p},
                          .account_id = "acc1",
                          .trading_day = kOldDay};
            w.enqueue(std::move(t));
        }
        {
            auto token = w.enqueue_flush_signal();
            EXPECT_TRUE(w.wait_flush(token, std::chrono::seconds(2)));
        }
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 1);

        // enqueue PositionRebuild{day=20260901, 2 行} — 同账户两个不同 (instrument, direction)
        {
            std::vector<DzPositionInfo> group;
            group.push_back(make_position(8, 11, DZ_DIRECTION_LONG));   // IF2506 多
            auto rb = make_position(3, 12, DZ_DIRECTION_SHORT);         // rb2510 空
            std::strcpy(rb.instrument_id, "rb2510");
            std::strcpy(rb.exchange_id, "SHFE");
            group.push_back(rb);
            PersistTask t{.kind = PersistTask::Kind::PositionRebuild,
                          .data = std::move(group),
                          .account_id = "acc1",
                          .trading_day = kNewDay};
            w.enqueue(std::move(t));
        }
        {
            auto token = w.enqueue_flush_signal();
            EXPECT_TRUE(w.wait_flush(token, std::chrono::seconds(2)));
        }

        // flush 后: 旧日行已删, 新 2 行在 (同事务原子切换, 无中间态)
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 2);
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE trading_day='20260831'"), 0);
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE trading_day='20260901'"), 2);

        w.stop();
    }
}

// Kind::Position 单行 upsert (盘中有变化时走它); Kind::TradingAccount 同理
TEST_F(TdPersistWriterTest, SingleUpsertPositionAndTradingAccount) {
    constexpr int64_t kNewDay = 20697;  // DzDate: 20260901
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        {
            auto p = make_position(5, 20, DZ_DIRECTION_LONG);
            PersistTask t{.kind = PersistTask::Kind::Position,
                          .data = std::vector<DzPositionInfo>{p},
                          .account_id = "acc1",
                          .trading_day = kNewDay};
            w.enqueue(std::move(t));
        }
        {
            auto a = make_taccount(1000000.0, 21);
            PersistTask t{.kind = PersistTask::Kind::TradingAccount,
                          .data = a,
                          .account_id = "acc1",
                          .trading_day = kNewDay};
            w.enqueue(std::move(t));
        }
        {
            auto token = w.enqueue_flush_signal();
            EXPECT_TRUE(w.wait_flush(token, std::chrono::seconds(2)));
        }

        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 1);
        EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE instrument_id='IF2506'"), 5);
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE account_id='acc1'"), 1);
        // balance REAL -> int 截断断言 1000000
        EXPECT_EQ(scalar_int("SELECT balance FROM trading_accounts WHERE account_id='acc1'"),
                  1000000);

        w.stop();
    }
}

}  // namespace
}  // namespace dztrader::ctp
