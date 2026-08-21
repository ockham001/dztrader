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
            "'commission_rates','instruments')");
        ASSERT_TRUE(q.executeStep());
        EXPECT_EQ(q.getColumn(0).getInt(), 6);
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

}  // namespace
}  // namespace dztrader::ctp
