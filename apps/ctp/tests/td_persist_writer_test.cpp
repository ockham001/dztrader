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
#include <dztrader/tdstore/records.h>

#include "td/td_persist_records.h"
#include "td/td_persist_writer.h"

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

/// 原生 SQL 写入 (测试播种/外部破坏用; 独立连接, 不经过 PersistWriter).
void raw_exec(const std::string& path, const std::string& sql) {
    SQLite::Database db(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec(sql);
}

/// 原生 SQL 标量读取 (独立只读连接; 整数经 sqlite3_column_text 转文本).
std::string raw_scalar(const std::string& path, const std::string& sql) {
    SQLite::Database db(path, SQLite::OPEN_READONLY);
    SQLite::Statement q(db, sql);
    return q.executeStep() ? q.getColumn(0).getString() : "";
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
        // 验证表存在 (独立只读连接, 不依赖 PersistWriter 句柄)
        EXPECT_EQ(std::stoi(raw_scalar(db_path_,
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
            "name IN ('schema_version','orders','trades','instruments',"
            "'positions','trading_accounts')")),
            6);
    }
}

// WAL 收敛语义: WAL 转换需独占锁且 busy_timeout 对其无效 — 别连接持锁时瞬时抛
// database is locked。open() 内迁移连接在锁窗口内有界重试失败仅告警续跑 (冷启动不中止),
// 锁释放后的 writer session 连接完成转换 — 最终收敛 journal_mode=wal, open 全程不抛。
TEST_F(TdPersistWriterTest, OpenConvergesToWalAfterLockClears) {
    // 第二连接持写事务锁住库 (RESERVED); 800ms 后释放, 让 open() 内迁移的
    // busy_timeout 能等到锁, 而 WAL 转换的 3 次有界重试 (0/100/200ms) 全部落在锁窗口内。
    SQLite::Database locker(db_path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    locker.exec("BEGIN IMMEDIATE");
    std::thread releaser([&locker] {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        try {
            locker.exec("COMMIT");
        } catch (const std::exception&) {
            // 释放失败由下方断言暴露 (open 迁移等待超时会抛)
        }
    });

    PersistWriter w(db_path_);
    bool open_threw = false;
    try {
        w.open();
    } catch (const std::exception&) {
        open_threw = true;
    }
    releaser.join();
    EXPECT_FALSE(open_threw);  // 冷启动不得因 WAL 转换失败而中止
    // 迁移连接在锁窗口内转换失败仅告警并续跑; writer session 连接在锁释放后完成
    // 转换 (仍持锁则维持当前模式继续, 不阻断 open).
    EXPECT_EQ("wal", raw_scalar(db_path_, "PRAGMA journal_mode"));
}

// 无锁: WAL 转换成功, 最终 journal_mode = wal (多网关共写 + 多读者前提)。
TEST_F(TdPersistWriterTest, OpenConvertsToWalWhenUnlocked) {
    PersistWriter w(db_path_);
    w.open();
    EXPECT_EQ("wal", raw_scalar(db_path_, "PRAGMA journal_mode"));
}

TEST_F(TdPersistWriterTest, MaxOrderIdThrowsAfterStartWriter) {
    PersistWriter w(db_path_);
    w.open();
    w.start_writer();
    EXPECT_THROW((void)w.max_order_id(), std::runtime_error);
    w.stop();
}

TEST_F(TdPersistWriterTest, MaxOrderIdThrowsBeforeOpen) {
    PersistWriter w(db_path_);
    EXPECT_THROW((void)w.max_order_id(), std::runtime_error);
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
    // orders 两笔 + trades 一笔, max = 200 (来自 trades); 原生连接播种
    raw_exec(db_path_,
             "INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
             "instrument_id, exchange_id) "
             "VALUES ('acc1', '20260814', 42, '000000000042', 'IF2506', 'CFFEX')");
    raw_exec(db_path_,
             "INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
             "instrument_id, exchange_id) "
             "VALUES ('acc1', '20260814', 100, '000000000100', 'IF2506', 'CFFEX')");
    raw_exec(db_path_,
             "INSERT INTO trades (account_id, trading_day, trade_id, order_id, "
             "instrument_id, exchange_id, price, volume) "
             "VALUES ('acc1', '20260814', 'T1', 200, 'IF2506', 'CFFEX', 100.5, 1)");
    EXPECT_EQ(w.max_order_id(), 200);
}

TEST_F(TdPersistWriterTest, MaxOrderIdOrdersOnly) {
    PersistWriter w(db_path_);
    w.open();
    raw_exec(db_path_,
             "INSERT INTO orders (account_id, trading_day, order_id, order_ref, "
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

        tdstore::InstrumentRecord r{};
        r.instrument_id = "IF2506";
        r.exchange_id = "CFFEX";
        r.name = "沪深300股指期货";
        r.product_class = DZ_PRODUCT_FUTURES;
        r.min_limit_order_volume = 1;
        r.delisted_date = 20600;
        r.volume_multiple = 300;
        r.price_tick = 0.2;
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Instrument, .data = r});

        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM instruments"), 1);
    // v4: product_class 列 (DZ_PRODUCT_*)
    EXPECT_EQ(scalar_int("SELECT product_class FROM instruments"), DZ_PRODUCT_FUTURES);
    EXPECT_EQ(scalar_int("SELECT min_limit_order_volume FROM instruments"), 1);
    EXPECT_EQ(scalar_int("SELECT delisted_date FROM instruments"), 20600);
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

// 评审 C1 失败路径: 批事务失败时, 已入队的 flush 哨兵不得误报成功.
// 批 = [合法 Order, FlushSignal, 触发异常的 Trade(trades 表已删)]:
// - 哨兵 token 在批内被收集, 但 commit/批执行失败 → 整批回滚 (Order 丢弃)
// - 若 set_value 早于 commit, wait_flush 会误报 true (数据实际已丢)
// 复现技巧: open() 建表后, 原生连接 DROP 该表 (start_writer 前),
// 批内 trades 写入时报 "no such table: trades" (确定性异常, 无需竞态).
TEST_F(TdPersistWriterTest, FlushReturnsFalseWhenBatchCommitFails) {
    PersistWriter w(db_path_);
    w.open();
    // 删 trades 表 (start_writer 前): upsert 时 prepare 失败, 执行时报错
    raw_exec(db_path_, "DROP TABLE trades");
    w.start_writer();

    // 合法 Order: 随批事务, commit 失败时一并回滚
    OrderRecord r{};
    r.base.order_id = 100;
    std::strcpy(r.base.account_id, "acc1");
    std::strcpy(r.trading_day, "20260901");
    std::strcpy(r.order_ref, "000001");
    std::strcpy(r.base.instrument_id, "IF2506");
    std::strcpy(r.base.exchange_id, "CFFEX");
    w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});

    auto token = w.enqueue_flush_signal();

    // 同批尾部触发异常: 表已删 -> exec 抛 SQLite::Exception, 整批回滚
    TradeRecord t{};
    std::strcpy(t.base.account_id, "acc1");
    std::strcpy(t.trading_day, "20260901");
    std::strcpy(t.base.trade_id, "T001");
    std::strcpy(t.base.instrument_id, "IF2506");
    std::strcpy(t.base.exchange_id, "CFFEX");
    w.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = t});

    // 批失败 -> flush 不 set -> 超时返回 false (不得误报成功)
    EXPECT_FALSE(w.wait_flush(token, std::chrono::milliseconds(300)));

    // 数据验证: 批事务已回滚, Order 未入库
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 0);

    w.stop();
}

// 终检发现 F: 批失败路径 token 不得慢性泄漏 — wait_flush 须立即 (非超时) 返回 false,
// 且反复失败后 pending_flushes_ 不累积 (set_exception + erase)。旧代码 catch 分支既不
// set 也不 erase: wait_flush 靠 300ms 超时兜底返回 false, map 条目永久残留 (7×24 累积)。
// 确定性: start_writer() 前全部入队 → 单批 drain (所有 FlushSignal 同批), 无批切分竞态。
TEST_F(TdPersistWriterTest, FailedBatchDoesNotLeakFlushTokens) {
    PersistWriter w(db_path_);
    w.open();
    // 删 trades 表 (start_writer 前): upsert 时 prepare 失败, 执行时报错
    raw_exec(db_path_, "DROP TABLE trades");

    // start_writer() 前全部入队: 队列累积, 启动后单批 drain (无批切分)。
    constexpr int kRounds = 20;
    std::vector<uint64_t> tokens;
    for (int i = 0; i < kRounds; ++i) {
        OrderRecord r{};
        r.base.order_id = 1000 + i;
        std::strcpy(r.base.account_id, "acc1");
        std::strcpy(r.trading_day, "20260901");
        std::strcpy(r.order_ref, "000001");
        std::strcpy(r.base.instrument_id, "IF2506");
        std::strcpy(r.base.exchange_id, "CFFEX");
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});

        auto token = w.enqueue_flush_signal();
        tokens.push_back(token);

        TradeRecord t{};
        std::strcpy(t.base.account_id, "acc1");
        std::strcpy(t.trading_day, "20260901");
        std::strcpy(t.base.trade_id, "T001");
        std::strcpy(t.base.instrument_id, "IF2506");
        std::strcpy(t.base.exchange_id, "CFFEX");
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = t});
    }

    w.start_writer();  // 单批 drain 全部任务, Trade 触发异常 → 整批回滚

    // 关键断言: 全部 token 必须立即返回 (不被 300ms 超时拖住), 证明 catch 分支终结了
    // promise。泄漏复现 (旧代码): token 既不 set 也不 erase → 每个 wait_flush 都要等满
    // 300ms 超时; 且已 erase 的 token 本应返回 true, 旧代码里 map 残留使其永远等待。
    // 修复后: set_exception/erase 均已执行, wait_flush 对已终结 token 立即返回。
    // 布尔值在此不严格约束: set_exception 已抓 future 的调用返回 false, 未抓 (已 erase)
    // 的按"已消费"返回 true — 二者都属"立即返回", 与旧代码"拖满 300ms"可区分。
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t token : tokens) {
        (void)w.wait_flush(token, std::chrono::milliseconds(300));
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    // 20 个 token 若都靠 300ms 超时兜底需 ≥6s; set_exception/erase 路径应 <1s。
    EXPECT_LT(elapsed, std::chrono::seconds(1)) << "flush tokens leaked: wait_flush blocked on timeout";

    // 后续未知 token 仍按"已消费"处理返回 true (map 已清, 不误伤正常路径)。
    EXPECT_TRUE(w.wait_flush(999999, std::chrono::milliseconds(50)));

    w.stop();
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

// PositionRebuild 单事务重灌: 清该账户全部持仓行 + upsert 本组 (spec §3.2 全量语义)
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

// 发现 2 回归 (评审 Important): PositionRebuild 需删"该账户全部行" (而非按 trading_day 排除).
// 盘中全平: 当日持仓通过 Kind::Position upsert 入 DB (trading_day=当日), CTP 全平后查询
// 响应不再含该合约 → PositionRebuild 组不含它. 若 DELETE 只按旧日排除, 当日行永驻 → 幽灵持仓.
TEST_F(TdPersistWriterTest, PositionRebuildClearsClosedCurrentDayRows) {
    constexpr int64_t kDay = 20697;  // 20260901
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        // 当日开仓: 单行 upsert (trading_day=当日, 模拟盘中已落库的持仓).
        {
            auto p = make_position(5, 10, DZ_DIRECTION_LONG);
            PersistTask t{.kind = PersistTask::Kind::Position,
                          .data = std::vector<DzPositionInfo>{p},
                          .account_id = "acc1",
                          .trading_day = kDay};
            w.enqueue(std::move(t));
        }
        {
            auto token = w.enqueue_flush_signal();
            EXPECT_TRUE(w.wait_flush(token, std::chrono::seconds(2)));
        }
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 1);

        // 全平: 下一次登录/补查查询响应不含该合约 → 空组 PositionRebuild (当日).
        {
            PersistTask t{.kind = PersistTask::Kind::PositionRebuild,
                          .data = std::vector<DzPositionInfo>{},
                          .account_id = "acc1",
                          .trading_day = kDay};
            w.enqueue(std::move(t));
        }
        {
            auto token = w.enqueue_flush_signal();
            EXPECT_TRUE(w.wait_flush(token, std::chrono::seconds(2)));
        }

        // 当日行也被删除 → 幽灵持仓清除.
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 0);
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE trading_day='20260901'"), 0);

        w.stop();
    }
}

// FIFO 原位语义 (load-bearing): 批内任务按原始序生效 — 早于 Rebuild 的 Position 行被
// Rebuild 清除, 晚于 Rebuild 的 Position 行覆盖 Rebuild 结果. 若实现把所有 delete
// 提前到所有 upsert 之前, 早于 Rebuild 的行会意外存活 (本用例可区分).
TEST_F(TdPersistWriterTest, PositionRebuildRespectsTaskOrderWithinBatch) {
    constexpr int64_t kDay = 20697;  // 20260901
    PersistWriter w(db_path_);
    w.open();

    // start_writer() 前全部入队 → 单批 drain, 强制三任务同批处理.
    {
        auto p = make_position(5, 10, DZ_DIRECTION_LONG);  // IF2506, 早于 rebuild
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Position,
                              .data = std::vector<DzPositionInfo>{p},
                              .account_id = "acc1",
                              .trading_day = kDay});
    }
    {
        auto rb = make_position(8, 11, DZ_DIRECTION_SHORT);  // rb2510, rebuild 组
        std::strcpy(rb.instrument_id, "rb2510");
        std::strcpy(rb.exchange_id, "SHFE");
        w.enqueue(PersistTask{.kind = PersistTask::Kind::PositionRebuild,
                              .data = std::vector<DzPositionInfo>{rb},
                              .account_id = "acc1",
                              .trading_day = kDay});
    }
    {
        auto p = make_position(9, 12, DZ_DIRECTION_SHORT);  // rb2510, 晚于 rebuild
        std::strcpy(p.instrument_id, "rb2510");
        std::strcpy(p.exchange_id, "SHFE");
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Position,
                              .data = std::vector<DzPositionInfo>{p},
                              .account_id = "acc1",
                              .trading_day = kDay});
    }

    w.start_writer();
    auto token = w.enqueue_flush_signal();
    EXPECT_TRUE(w.wait_flush(token, std::chrono::seconds(2)));

    // 早于 Rebuild 的 IF2506 行被清除; rb2510 为晚到 Position 的覆盖值 9.
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE instrument_id='IF2506'"), 0);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 1);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE instrument_id='rb2510'"), 9);

    w.stop();
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
