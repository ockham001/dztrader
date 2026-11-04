#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/tdstore/trade_db.h>
#include <dztrader/trading/td_ingest.h>

#include "td/td_day_load.h"
#include "td/td_persist_records.h"
#include "td/td_persist_writer.h"
#include "td/td_report_filter.h"
#include "test_temp_dir.h"

namespace dztrader::ctp {
namespace {

/// 进程内组装 td 侧组件: 临时库 + PersistWriter 真线程 + ReportFilter + TdIngestGate.
/// 测试按场景手工编排生产者/消费者时序, 模拟 spec §6 场景矩阵.
class TdDataSyncIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = dztrader::test::unique_temp_dir("dz_td_data_sync_it");
        std::filesystem::create_directories(tmp_dir_);
        db_path_ = (tmp_dir_ / "test.db").string();
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_ + "-journal");
    }
    void TearDown() override {
        std::filesystem::remove_all(tmp_dir_);
        for (const auto& dir : extra_dirs_) {
            std::filesystem::remove_all(dir);
        }
    }

    /// 只读连接 (与生产 PersistWriter 的连接级 PRAGMA 同理设置 busy_timeout,
    /// 防写入提交期间同进程另一连接报 database is locked).
    static SQLite::Database open_readonly(const std::string& db_path) {
        SQLite::Database db(db_path, SQLite::OPEN_READONLY);
        db.exec("PRAGMA busy_timeout=5000");
        return db;
    }

    /// 只读标量查询 (验证 DB 已提交内容).
    int64_t scalar_int(const std::string& sql) {
        SQLite::Database db = open_readonly(db_path_);
        SQLite::Statement q(db, sql);
        if (q.executeStep()) {
            return q.getColumn(0).getInt64();
        }
        return 0;
    }

    /// 只读文本标量查询.
    std::string scalar_text(const std::string& sql) {
        SQLite::Database db = open_readonly(db_path_);
        SQLite::Statement q(db, sql);
        if (q.executeStep()) {
            return q.getColumn(0).getString();
        }
        return {};
    }

    /// 只读实型标量查询.
    double scalar_double(const std::string& sql) {
        SQLite::Database db = open_readonly(db_path_);
        SQLite::Statement q(db, sql);
        if (q.executeStep()) {
            return q.getColumn(0).getDouble();
        }
        return 0.0;
    }

    /// 登录日装载 (用局部只读连接, 避免把临时对象绑定到非 const 引用).
    /// 空库场景下先按生产门面建库 (master 职责), 否则只读连接打不开文件.
    DayLoadData load_day(const char* day = "20260901") {
        if (!std::filesystem::exists(db_path_)) {
            (void)dztrader::tdstore::ensure_trade_db(db_path_);
        }
        SQLite::Database db = open_readonly(db_path_);
        return load_trading_day(db, "acc1", day);
    }

    std::filesystem::path tmp_dir_;
    std::string db_path_;
    /// 用例内另起的临时库目录 (失败路径也必须清理: TearDown 统一删除).
    std::vector<std::filesystem::path> extra_dirs_;
};

/// 构造委托记录. seq 为唯一区分点, 其余字段同基准可复现.
OrderRecord make_order(int64_t order_id, uint64_t seq, const char* day = "20260901") {
    OrderRecord r{};
    r.base.order_id = order_id;
    r.base.seq = seq;
    std::strcpy(r.base.account_id, "acc1");
    std::strcpy(r.trading_day, day);
    std::strcpy(r.order_ref, "000001");
    std::strcpy(r.base.instrument_id, "IF2506");
    std::strcpy(r.base.exchange_id, "CFFEX");
    r.base.direction = DZ_DIRECTION_LONG;
    r.base.status = DZ_ORDER_ALL_TRADED;
    r.base.volume = 5;
    r.base.volume_traded = 5;
    r.update_time = 1000;
    return r;
}

/// 构造成交记录. (trading_day, trade_id, exchange_id) 为唯一键区分点.
TradeRecord make_trade(const char* trade_id,
                       uint64_t seq,
                       const char* day = "20260901",
                       const char* exchange_id = "CFFEX",
                       int64_t order_id = 1,
                       double price = 3900.0,
                       int32_t volume = 1) {
    TradeRecord r{};
    r.base.seq = seq;
    std::strcpy(r.base.account_id, "acc1");
    std::strcpy(r.trading_day, day);
    std::strcpy(r.base.trade_id, trade_id);
    r.base.order_id = order_id;
    std::strcpy(r.base.instrument_id, "IF2506");
    std::strcpy(r.base.exchange_id, exchange_id);
    r.base.direction = DZ_DIRECTION_LONG;
    r.base.price = price;
    r.base.volume = volume;
    return r;
}

// ============================================================================
// 场景 1: 重放风暴去重端到端 (spec §4.1 + §6 重登风暴)
// 灌 50 条订单入 DB → 装载过滤器基准 → 重放同 50 条全 kSkip (全吞, 不落新行)
// → 其中 1 条 volume_traded 变化 → kForward → 更新基准 → 落库 → W 前移
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, ReplayStormDedupEndToEnd) {
    constexpr int kCount = 50;
    std::vector<OrderRecord> seed;
    seed.reserve(kCount);
    for (int i = 1; i <= kCount; ++i) {
        seed.push_back(make_order(i, static_cast<uint64_t>(i)));
    }

    // ---- 生产者: 真 PersistWriter 线程落 50 条入库, flush 屏障保证提交 ----
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        for (const auto& r : seed) {
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
        }
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE account_id='acc1'"), kCount);

    // ---- 消费者启动: 独立只读连接查 W + 登录日装载过滤器基准 ----
    SQLite::Database ro(db_path_, SQLite::OPEN_READONLY);
    const uint64_t W = query_max_seq(ro, "acc1");
    EXPECT_EQ(W, static_cast<uint64_t>(kCount));  // 四表取大 = orders 最大 seq

    auto day = load_trading_day(ro, "acc1", "20260901");
    EXPECT_EQ(day.start_seq, W);
    auto filter = ReportFilter::load(day.orders, day.trades);

    // ---- 重放风暴: CTP 重放同 50 条 (同 order_id/同字段) → 全 kSkip ----
    for (const auto& rec : seed) {
        bool warn = false;
        EXPECT_EQ(ReportFilter::Verdict::kSkip, filter.check_order(rec, &warn))
            << rec.base.order_id;
        EXPECT_FALSE(warn);
    }

    // ---- 其中 1 条状态变化 (volume_traded 5→4, update_time 前移) → kForward ----
    OrderRecord changed = seed[7];
    changed.base.volume_traded = 4;
    changed.update_time = 2000;
    bool warn = false;
    EXPECT_EQ(ReportFilter::Verdict::kForward, filter.check_order(changed, &warn));
    EXPECT_FALSE(warn);

    // 消费者端: 转发 + 分配 seq + 更新基准 + 落库 (单条)
    const uint64_t new_seq = W + 1;
    changed.base.seq = new_seq;
    filter.accept_order(changed);
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = changed});
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE account_id='acc1'"),
              kCount);  // 覆盖非新增
    EXPECT_EQ(scalar_int("SELECT volume_traded FROM orders WHERE order_id=8"), 4);
    EXPECT_EQ(scalar_int("SELECT seq FROM orders WHERE order_id=8"), new_seq);

    // ---- 重放该变化后的行 → 已入基准 → kSkip; W 前移到新 seq ----
    {
        bool w2 = false;
        EXPECT_EQ(ReportFilter::Verdict::kSkip, filter.check_order(changed, &w2));
    }
    SQLite::Database ro2(db_path_, SQLite::OPEN_READONLY);
    EXPECT_EQ(query_max_seq(ro2, "acc1"), new_seq);
}

// ============================================================================
// 场景 2: 消费者竞态生产者 (spec §5.2 在途窗口)
// 变体 A: 帧 DZ_FRAME_NOTIFY_UI-110 写入 (seq 已分配) + persist 入队未提交 → 消费者 W=100
//         → 首帧 DZ_FRAME_NOTIFY_UI 无断档正常应用 (DB 提交在后, W 读自快照, 不误报 gap)
// 变体 B: 帧 DZ_FRAME_NOTIFY_UI-105 在 reader 开启前完成 (帧先于快照), DB 提交在后
//         → 首帧 106 → gap(DZ_FRAME_NOTIFY_UI,105) → 回补查 DB (提交后) → 5 行返回
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, ConsumerRacesProducerInFlightWindow) {
    constexpr uint64_t kW = 100;

    // 先提交旧快照 seq 1..100 (W=100 的来源), 再在内存产生在途帧 DZ_FRAME_NOTIFY_UI-105.
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        for (int i = 1; i <= 100; ++i) {
            auto r = make_order(i, static_cast<uint64_t>(i));
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
        }
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE account_id='acc1'"), 100);

    // ===================== 变体 A: 首帧 DZ_FRAME_NOTIFY_UI 无断档 =====================
    {
        // 生产者: 帧已写 (内存已见 seq DZ_FRAME_NOTIFY_UI-105), persist 入队但 Writer 未启动
        // (未提交) 队列堆积, 无 Writer 线程持锁 → 消费者可无锁快照.
        PersistWriter w(db_path_, "dztd_test");
        w.open();  // 不 start_writer: 任务入队未提交, 不占写锁
        std::vector<OrderRecord> inflight;
        for (uint64_t s = 101; s <= 105; ++s) {
            inflight.push_back(make_order(static_cast<int64_t>(s), s));
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = inflight.back()});
        }

        // 消费者: 独立只读连接查 W (在途帧未提交 → 仍为旧快照 100)
        SQLite::Database ro(db_path_, SQLite::OPEN_READONLY);
        const uint64_t w_read = query_max_seq(ro, "acc1");
        EXPECT_EQ(w_read, kW);

        // gate 首帧 DZ_FRAME_NOTIFY_UI == W+1 → 无 gap, 正常应用
        dztrader::TdIngestGate gate;
        gate.set_watermark("acc1", w_read);
        EXPECT_EQ(dztrader::TdIngestGate::Verdict::kApply, gate.admit("acc1", 101));
        EXPECT_FALSE(gate.take_pending_gap().has_value());  // 顺序首帧, 无断档

        // 在途帧随 Writer drain 提交
        w.start_writer();
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE account_id='acc1'"), 105);
    }

    // ===================== 变体 B: 首帧 106 → gap(DZ_FRAME_NOTIFY_UI,105) → 回补
    // ===================== 独立临时库: 先提交 1..100 (W=100 快照), 帧 DZ_FRAME_NOTIFY_UI-105 在
    // reader 开启前完成写入, DB 提交在后 (在途窗口).
    std::filesystem::path tmp2 = dztrader::test::unique_temp_dir("dz_td_data_sync_it_b");
    std::filesystem::create_directories(tmp2);
    extra_dirs_.push_back(tmp2);  // 断言提前返回时也由 TearDown 清理
    const std::string db2 = (tmp2 / "b.db").string();
    std::filesystem::remove(db2);
    std::filesystem::remove(db2 + "-journal");
    {      // 变体 B 作用域: 内层 w/ro/ro2 三个 SQLite 句柄全部析构后才允许删库
        {  // 首个 PersistWriter 独立作用域
            PersistWriter w(db2, "dztd_test");
            w.open();
            w.start_writer();
            for (int i = 1; i <= 100; ++i) {
                auto r = make_order(i, static_cast<uint64_t>(i));
                w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
            }
            auto token = w.enqueue_flush_signal();
            ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
            w.stop();
        }

        // 帧 DZ_FRAME_NOTIFY_UI-105 在 reader 开启前完成写入 (内存), DB 提交在后.
        // open() 即建 schema; 不 start_writer → 队列堆积未提交, 读者无锁快照.
        PersistWriter w(db2, "dztd_test");
        w.open();
        for (uint64_t s = 101; s <= 105; ++s) {
            auto r = make_order(static_cast<int64_t>(s), s);
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
        }

        // 消费者: reader 在 DZ_FRAME_NOTIFY_UI-105 写完后开启 → 首个可见帧 = 106. W = 100 (旧快照).
        SQLite::Database ro(db2, SQLite::OPEN_READONLY);
        const uint64_t w_read = query_max_seq(ro, "acc1");
        EXPECT_EQ(w_read, 100u);

        // 首帧 106 → gate 标记 gap(DZ_FRAME_NOTIFY_UI,105)
        dztrader::TdIngestGate gate;
        gate.set_watermark("acc1", w_read);
        EXPECT_EQ(dztrader::TdIngestGate::Verdict::kApply, gate.admit("acc1", 106));
        auto gap = gate.take_pending_gap();
        ASSERT_TRUE(gap.has_value());
        EXPECT_EQ(gap->from, 101u);
        EXPECT_EQ(gap->to, 105u);

        // 回补: Writer drain (DB 提交在途窗口) 后新开只读连接再查 seq>100 → 5 行.
        // 不用 drain 前已建的 ro (其 SQL 快照在提交前建立, 复用会读到"提交前"可见性,
        // 且与其余场景"drain 后新开连接"不一致).
        w.start_writer();
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
        SQLite::Database ro2 = open_readonly(db2);
        auto backfilled = load_trading_day(ro2, "acc1", "20260901").orders;
        ASSERT_EQ(backfilled.size(), 105u);  // 当日全部行, 断档回补只看 seq > 100 的部分
        std::vector<uint64_t> above;
        for (const auto& rec : backfilled) {
            if (rec.base.seq > 100u) {
                above.push_back(rec.base.seq);
            }
        }
        ASSERT_EQ(above.size(), 5u);
        EXPECT_EQ(above.front(), 101u);
        EXPECT_EQ(above.back(), 105u);
    }  // PersistWriter w / ro / ro2 在此析构, 释放 db2 的 SQLite 句柄

    // tmp2 由 TearDown 统一删除 (断言提前返回的失败路径也覆盖); 此时块内 SQLite 句柄
    // 均已析构 — Windows 不允许删除仍被占用的文件, 顺序由作用域保证。
}

// ============================================================================
// 场景 3: 重置后重建 (spec §5.5 倒退检测 + reset + 新水位)
// 生产端重置 (DB 清空/重加) 后, 消费端 detect_reset → 重查 DB 得 new_w → reset_account。
// 本用例把 new_w 接到真实 query_max_seq (而非硬编码), 验证重置后新流按 DB 水位放行。
// (消费端 detect_reset→rebuild→reset 接线的端到端用例在 libs/strategy_api/td_ingest_wiring_test;
// 本文件覆盖 gate + DB 水位原语)
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, ResetThenRebuildWatermarkFromDb) {
    // 预置「重置后」的 DB: 新流 seq 1..2 已落库
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        for (int i = 1; i <= 2; ++i) {
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order,
                                  .data = make_order(i, static_cast<uint64_t>(i))});
        }
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    SQLite::Database ro(db_path_, SQLite::OPEN_READONLY);
    const uint64_t new_w = query_max_seq(ro, "acc1");  // 真实重查 DB 水位
    ASSERT_EQ(new_w, 2u);

    dztrader::TdIngestGate gate;
    gate.set_watermark("acc1", 5000);
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kApply, gate.admit("acc1", 5001));

    // 生产端重置, 消费者收到倒退帧 → 重查 DB 后按新水位重置
    EXPECT_TRUE(gate.detect_reset("acc1", 3));
    gate.reset_account("acc1", new_w);

    // 重置后新流正常: 3 > 2 → kApply; 且 no gap (顺序首帧)
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kApply, gate.admit("acc1", 3));
    EXPECT_FALSE(gate.take_pending_gap().has_value());
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kSkip, gate.admit("acc1", 3));  // 防重

    // 倒退检测在新基准下重新生效
    EXPECT_TRUE(gate.detect_reset("acc1", 2));
}

// ============================================================================
// 场景 4: 登录日装载 + 重放风暴 (契约 td-data-sync §3 / §4, ADR 0045 决策 5)
// 播当日 + 历史日订单/成交 -> load_trading_day 只装当日 -> 重放同批全 kSkip
// (不重复落库 / 不重复推帧: 校验 DB 行数不变、无新 seq 分配).
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, DayLoadThenReplayDoesNotDuplicate) {
    // ---- 生产端: 历史日 5 条 + 当日 10 条订单, 当日 3 条成交 ----
    std::vector<OrderRecord> history;
    for (int i = 1; i <= 5; ++i) {
        history.push_back(make_order(i, static_cast<uint64_t>(i), "20260831"));
    }
    std::vector<OrderRecord> today;
    for (int i = 11; i <= 20; ++i) {
        today.push_back(make_order(i, static_cast<uint64_t>(i), "20260901"));
    }
    std::vector<TradeRecord> trades;
    for (int i = 0; i < 3; ++i) {
        trades.push_back(make_trade(("T00" + std::to_string(i + 1)).c_str(),
                                    static_cast<uint64_t>(30 + i), "20260901"));
    }
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        for (const auto& r : history) {
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
        }
        for (const auto& r : today) {
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
        }
        for (const auto& r : trades) {
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = r});
        }
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    const int64_t orders_before = scalar_int("SELECT COUNT(*) FROM orders");
    const int64_t trades_before = scalar_int("SELECT COUNT(*) FROM trades");
    ASSERT_EQ(orders_before, 15);
    ASSERT_EQ(trades_before, 3);

    // ---- 登录: 只装当日 (历史日不装载), start_seq = 四表 MAX(seq) 含历史日 ----
    SQLite::Database ro = open_readonly(db_path_);
    auto day = load_trading_day(ro, "acc1", "20260901");
    ASSERT_EQ(day.orders.size(), 10u);
    ASSERT_EQ(day.trades.size(), 3u);
    EXPECT_EQ(day.start_seq, 32u);  // trades seq 30..32 最大

    auto filter = ReportFilter::load(day.orders, day.trades);

    // ---- 重放风暴: 当日同批全 kSkip → 不分配 seq, 不落库, 不推帧 ----
    // 「不分配 seq」用真实取号器观测: 重放路径只调 check_* (全 kSkip), 没有任何 accept/落库,
    // 因此取号器水位与 DB MAX(seq) 都必须停在 32; 取号器本身可用性由下面的 next() 反证
    // (若它自己在重放期间被推进, 唯一一次 next() 就不会得到 33)。
    AccountSeqAllocator allocator;
    SQLite::Database ro_for_alloc = open_readonly(db_path_);
    allocator.set_db_provider([&ro_for_alloc]() -> SQLite::Database* { return &ro_for_alloc; });
    for (const auto& rec : today) {
        bool warn = false;
        ASSERT_EQ(ReportFilter::Verdict::kSkip, filter.check_order(rec, &warn))
            << rec.base.order_id;
        EXPECT_FALSE(warn);
    }
    for (const auto& rec : trades) {
        EXPECT_EQ(ReportFilter::Verdict::kSkip, filter.check_trade(rec));
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), orders_before);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades"), trades_before);
    SQLite::Database ro2 = open_readonly(db_path_);
    EXPECT_EQ(query_max_seq(ro2, "acc1"), 32u);  // 全吞期间 DB 无新行
    EXPECT_EQ(allocator.next("acc1"), 33u);      // 取号器仍从 DB MAX+1 起 (未被重放推进)
}

// ============================================================================
// 场景 5: 跨日累积 (spec §2.1 累积不归零 + §3.2 trades 唯一键含 date/exchange_id)
// 两日 trades 同 trade_id 共存, W 跨日单调 (以最大 seq 为准, 不因跨日回落)
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, CrossDayAccumulation) {
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();

        // 第一日: trade T001 (seq 1), 第二日: trade T001 (seq 2, 同 trade_id 跨日)
        // 唯一键 (account_id, date, exchange_id, trade_id) → 两行共存
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Trade,
                              .data = make_trade("T001", 1, "20260901")});
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Trade,
                              .data = make_trade("T001", 2, "20260902")});
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades WHERE account_id='acc1'"), 2);

    SQLite::Database ro(db_path_, SQLite::OPEN_READONLY);
    // 登录日装载只装当日: 两日各 1 条成交, 同 trade_id 跨日不互相吞
    auto day1 = load_trading_day(ro, "acc1", "20260901");
    auto day2 = load_trading_day(ro, "acc1", "20260902");
    ASSERT_EQ(day1.trades.size(), 1u);
    ASSERT_EQ(day2.trades.size(), 1u);
    EXPECT_STREQ(day1.trades[0].base.trade_id, "T001");
    EXPECT_STREQ(day2.trades[0].base.trade_id, "T001");
    EXPECT_NE(day1.trades[0].base.seq, day2.trades[0].base.seq);

    // W 跨日单调 = 最大 seq, 不因交易日切换回落
    EXPECT_EQ(query_max_seq(ro, "acc1"), 2u);

    // 消费端: W=2 过滤同日重复, 跨日同 trade_id 的新帧 (新 seq) 仍放行
    dztrader::TdIngestGate gate;
    gate.set_watermark("acc1", query_max_seq(ro, "acc1"));
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kSkip, gate.admit("acc1", 2));  // 快照已含
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kApply, gate.admit("acc1", 3));
    // 成交去重二道防线: 跨日同 trade_id 必须放行 (新唯一键同语义)
    EXPECT_TRUE(gate.admit_trade("acc1", "20260901", "T001"));
    EXPECT_TRUE(gate.admit_trade("acc1", "20260902", "T001"));
}

// ============================================================================
// 终检发现 4(c): 吞同不推进 W — 重放风暴全吞后, 消费端水位不被重放推进, 基准不被污染
// (状态变化的记录仍 kForward)。
// 说明: 「生产端 kSkip 不落库/不分配 seq」由 AccountSession::on_rtn_order 的早退保证;
// 该路径无测试基建 (需 CTP/SHM 全套), 本用例不覆盖 — 重放循环期间没有 Writer 在跑,
// 任何「事后 DB 行数/MAX(seq) 不变」的断言都恒真 (§1.1), 已删除。
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, ReplayStormSkipsDoNotAdvanceConsumerWatermark) {
    // 生产端落一批入库 (seq 1..30), W = 30
    constexpr int kCount = 30;
    std::vector<OrderRecord> seed;
    seed.reserve(kCount);
    for (int i = 1; i <= kCount; ++i) {
        seed.push_back(make_order(i, static_cast<uint64_t>(i)));
    }
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        for (const auto& r : seed) {
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
        }
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    SQLite::Database ro(db_path_, SQLite::OPEN_READONLY);
    const uint64_t w_before = query_max_seq(ro, "acc1");
    ASSERT_EQ(w_before, static_cast<uint64_t>(kCount));

    // 消费端装载基准后重放同一批 → 全 kSkip (吞同)
    auto day = load_trading_day(ro, "acc1", "20260901");
    auto filter = ReportFilter::load(day.orders, day.trades);
    for (const auto& rec : seed) {
        bool warn = false;
        ASSERT_EQ(ReportFilter::Verdict::kSkip, filter.check_order(rec, &warn));
        EXPECT_FALSE(warn);
    }

    // 吞同不得污染基准: 状态变化的记录仍须 kForward (check_order 是 const 纯函数)
    OrderRecord changed = seed[7];
    changed.base.volume_traded = 3;
    changed.update_time = 2000;
    bool warn = false;
    EXPECT_EQ(ReportFilter::Verdict::kForward, filter.check_order(changed, &warn));
    EXPECT_FALSE(warn);

    // 消费端水位 (gate) 不因重放推进: W 本身仍 kSkip, W+1 才放行
    dztrader::TdIngestGate gate;
    gate.set_watermark("acc1", w_before);
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kSkip, gate.admit("acc1", w_before));
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kApply, gate.admit("acc1", w_before + 1));
}

// ============================================================================
// 场景 6: #13 无会话拒单落库 (契约 td-data-sync §3 "无会话拒单也必须分配账户级
// seq 并落库/推帧"). 预置该账户历史 seq (W>0) → 无会话拒单 seq=W+1..,
// 真 PersistWriter 提交 → DB 行 seq == 帧 seq, 且 TdIngestGate(W) 判 kApply
// (不再被当作"快照已含"吞掉).
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, NoSessionRejectAllocatesSeqAndPersists) {
    // ---- 预置历史水位: 该账户已有 seq 1..12 的订单 ----
    constexpr uint64_t kW = 12;
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        for (uint64_t i = 1; i <= kW; ++i) {
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order,
                                  .data = make_order(static_cast<int64_t>(i), i)});
        }
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    SQLite::Database ro = open_readonly(db_path_);
    ASSERT_EQ(query_max_seq(ro, "acc1"), kW);

    // ---- 无会话拒单: AccountSeqAllocator 从 DB 播种 (W) → 拒单 seq = W+1 ----
    AccountSeqAllocator alloc;
    alloc.set_db_provider([&ro]() -> SQLite::Database* { return &ro; });
    EXPECT_EQ(alloc.next("acc1"), kW + 1);

    DzOrderReq req{};
    req.order_id = 9001;
    std::strcpy(req.strategy_id, "strat1");
    std::strcpy(req.account_id, "acc1");
    std::strcpy(req.instrument_id, "IF2506");
    req.direction = DZ_DIRECTION_LONG;
    req.price_type = DZ_PRICE_LIMIT;
    req.position_effect = DZ_POSITION_EFFECT_OPEN;
    req.price = 3900.0;
    req.volume = 3;

    OrderRecord frame = build_no_session_reject(req, 20260901, 9 * 3600 + 30 * 60, "账户未连接");
    frame.base.seq = alloc.next("acc1");
    EXPECT_EQ(frame.base.seq, kW + 2);  // 第二次分配
    // 首条拒单 (seq=W+1, 独立 order_id, 验证 DB/帧同 seq)
    DzOrderReq req2 = req;
    req2.order_id = 9002;
    OrderRecord first = build_no_session_reject(req2, 20260901, 9 * 3600, "账户未连接");
    first.base.seq = kW + 1;

    // ---- 真 PersistWriter 提交两条拒单行 ----
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = first});
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = frame});
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }

    // ---- DB 有该行, seq == 帧 seq (同事件两路同值), 状态/remark 正确 ----
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE order_id=9001"), 1);
    EXPECT_EQ(scalar_int("SELECT seq FROM orders WHERE order_id=9001"),
              static_cast<int64_t>(frame.base.seq));
    EXPECT_EQ(scalar_int("SELECT seq FROM orders WHERE order_id=9002"),
              static_cast<int64_t>(kW + 1));
    EXPECT_EQ(scalar_int("SELECT status FROM orders WHERE order_id=9001"), DZ_ORDER_REJECTED);
    EXPECT_EQ(scalar_text("SELECT remark FROM orders WHERE order_id=9001"), "账户未连接");

    // ---- 消费端: W 之后的新 seq 必须 kApply (不再被 "快照已含" 吞) ----
    dztrader::TdIngestGate gate;
    gate.set_watermark("acc1", kW);
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kApply, gate.admit("acc1", kW + 1));
    EXPECT_FALSE(gate.take_pending_gap().has_value());  // 顺序首帧无断档
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kApply, gate.admit("acc1", kW + 2));
    // 拒单行已进水位: 重放同一帧被吞
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kSkip, gate.admit("acc1", kW + 1));
}

// ============================================================================
// 持仓增量落库 (spec §3.2)
// Position 单行 upsert → PositionRebuild 重灌 (未变化行沿用原 seq + 新行)
// → 空组重灌 = 清空 (全平幽灵持仓清除)
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, PositionUpsertThenRebuildThenClear) {
    auto make_pos = [](const char* inst, uint64_t seq) {
        DzPositionInfo p{};
        std::strcpy(p.account_id, "acc1");
        std::strcpy(p.instrument_id, inst);
        std::strcpy(p.exchange_id, "CFFEX");
        p.direction = DZ_DIRECTION_LONG;
        p.volume = 2;
        p.today_volume = 1;
        p.yd_volume = 1;
        p.price = 3900.0;
        p.date = 20260910;
        p.seq = seq;
        return p;
    };
    PersistWriter w(db_path_, "dztd_test");
    w.open();
    w.start_writer();
    // 1) 单行增量 upsert
    w.enqueue(PersistTask{.kind = PersistTask::Kind::Position,
                          .data = std::vector<DzPositionInfo>{make_pos("IF2506", 5)},
                          .account_id = "acc1",
                          .trading_day = 20260910});
    auto token = w.enqueue_flush_signal();
    ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
    EXPECT_EQ(
        scalar_int("SELECT seq FROM positions WHERE account_id='acc1' AND instrument_id='IF2506'"),
        5);
    // 2) 重灌: 未变化行沿用 seq 5 + 新行 seq 6
    w.enqueue(PersistTask{
        .kind = PersistTask::Kind::PositionRebuild,
        .data = std::vector<DzPositionInfo>{make_pos("IF2506", 5), make_pos("rb2510", 6)},
        .account_id = "acc1",
        .trading_day = 20260910});
    token = w.enqueue_flush_signal();
    ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 2);
    EXPECT_EQ(
        scalar_int("SELECT seq FROM positions WHERE account_id='acc1' AND instrument_id='IF2506'"),
        5);
    EXPECT_EQ(
        scalar_int("SELECT seq FROM positions WHERE account_id='acc1' AND instrument_id='rb2510'"),
        6);
    // 3) 空组重灌 = 清空 (全平幽灵持仓清除)
    w.enqueue(PersistTask{.kind = PersistTask::Kind::PositionRebuild,
                          .data = std::vector<DzPositionInfo>{},
                          .account_id = "acc1",
                          .trading_day = 20260910});
    token = w.enqueue_flush_signal();
    ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 0);
    w.stop();
}

// ============================================================================
// 场景 7 (#248): 成交均价实时累计 + 每笔成交刷新委托行 (契约 td-data-sync §3)
//
// seam ① 进程内组装: ReportFilter (去重基准) + OrderTradeAvgTracker (每单累计) +
// 真 PersistWriter (落库). on_rtn_trade 的转发次序在此按同序编排:
//   ① check_trade (kSkip 早退) ② 分配 seq ③ accept_trade ④ 落成交
//   ⑤ tracker.add_trade ⑥ 均价变化 → order_record_with_avg + 新 seq + accept_order + 落委托.
// 断言只看对外可观察行为: DB 行内容 / 跟踪器口径 / 重放裁决.
// ============================================================================

/// 生产端 (td 写端) 的进程内替身: 真实 ReportFilter + OrderTradeAvgTracker + PersistWriter.
/// 次序与 AccountSession::on_rtn_trade / refresh_order_avg 一致 (见上方注释).
class TdTradeProducer {
public:
    TdTradeProducer(std::string db_path,
                    const std::vector<OrderRecord>& orders,
                    const std::vector<TradeRecord>& trades,
                    uint64_t start_seq)
        : writer_(std::move(db_path), "dztd_test"),
          filter_(ReportFilter::load(orders, trades)),
          seq_(start_seq) {
        tracker_.seed(trades);
    }

    void start() {
        writer_.open();
        writer_.start_writer();
    }

    void stop() { writer_.stop(); }

    /// 转发一笔成交; 返回成交是否被吞 (去重命中).
    bool on_trade(TradeRecord rpt) {
        if (filter_.check_trade(rpt) == ReportFilter::Verdict::kSkip) {
            return true;
        }
        rpt.base.seq = ++seq_;
        filter_.accept_trade(rpt);
        writer_.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = rpt});
        // 每单成交累计 + 均价变化则刷新委托行.
        tracker_.add_trade(rpt.base.order_id, rpt.base.price, rpt.base.volume);
        refresh_order_avg(rpt.base.order_id);
        return false;
    }

    /// 转发一条委托回报 (含均价回填: 累计值优先 → 基准行值 → 0).
    void on_order(OrderRecord rpt) {
        if (const auto avg = tracker_.avg_price(rpt.base.order_id); avg.has_value()) {
            rpt.base.avg_price = *avg;
        } else if (const OrderRecord* base = filter_.find_order(rpt.base.order_id);
                   base != nullptr) {
            rpt.base.avg_price = base->base.avg_price;
        }
        rpt.base.seq = ++seq_;
        filter_.accept_order(rpt);
        writer_.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = rpt});
    }

    void refresh_order_avg(int64_t order_id) {
        const OrderRecord* base = filter_.find_order(order_id);
        if (base == nullptr) {
            return;
        }
        const std::optional<double> avg = tracker_.avg_price(order_id);
        if (!avg.has_value() || *avg == base->base.avg_price) {
            return;
        }
        OrderRecord updated = order_record_with_avg(*base, *avg);
        updated.base.seq = ++seq_;
        filter_.accept_order(updated);
        writer_.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = updated});
    }

    void flush() {
        const uint64_t token = writer_.enqueue_flush_signal();
        ASSERT_TRUE(writer_.wait_flush(token, std::chrono::seconds(5)));
    }

    [[nodiscard]] const ReportFilter& filter() const { return filter_; }
    [[nodiscard]] const OrderTradeAvgTracker& tracker() const { return tracker_; }
    /// 最近一次分配的 seq (0 = 尚未分配).
    [[nodiscard]] uint64_t last_seq() const { return seq_; }

private:
    PersistWriter writer_;
    ReportFilter filter_;
    OrderTradeAvgTracker tracker_;
    uint64_t seq_;
};

/// 该 order 的刷新路径: 两笔成交累加 → DB avg_price = 加权值, seq = 刷新 seq (帧/DB 同 seq).
TEST_F(TdDataSyncIntegrationTest, TradeAvgAccumulatesAndRefreshesOrderRow) {
    // 基准: 一条已存在的委托行 (seq 10, avg_price 0) + 空成交
    OrderRecord order = make_order(1, 10);
    order.base.status = DZ_ORDER_PART_TRADED;
    order.base.volume_traded = 0;
    order.update_time = 1000;
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = order});
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }

    auto day = load_day();
    ASSERT_EQ(day.orders.size(), 1u);
    ASSERT_EQ(day.start_seq, 10u);

    TdTradeProducer producer(db_path_, day.orders, day.trades, day.start_seq);
    producer.start();
    // 成交 1: 3900 x 1 → 均价 3900 → 刷新行 seq 12
    EXPECT_FALSE(producer.on_trade(make_trade("T001", 0, "20260901", "CFFEX", 1, 3900.0, 1)));
    EXPECT_EQ(producer.last_seq(), 12u);  // 成交 seq 11 (水位 11), 刷新 seq 12
    // 成交 2: 3903 x 3 → 均价 3902.25 → 刷新行 seq 14
    EXPECT_FALSE(producer.on_trade(make_trade("T002", 0, "20260901", "CFFEX", 1, 3903.0, 3)));
    EXPECT_EQ(producer.last_seq(), 14u);  // 成交 seq 13, 刷新 seq 14
    producer.flush();

    // DB: 该行均价 = 加权均价, seq = 最后一次刷新的 seq (帧/DB 同 seq)
    EXPECT_DOUBLE_EQ(scalar_double("SELECT avg_price FROM orders WHERE order_id=1"), 3902.25);
    EXPECT_EQ(scalar_int("SELECT seq FROM orders WHERE order_id=1"), 14);
    // 成交两行落库
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades WHERE account_id='acc1'"), 2);
    // 跟踪器口径 = Σ价×量 / Σ量
    ASSERT_TRUE(producer.tracker().avg_price(1).has_value());
    EXPECT_DOUBLE_EQ(*producer.tracker().avg_price(1), 3902.25);
    EXPECT_EQ(producer.tracker().volume(1), 4);
    producer.stop();
}

/// 成交去重: 同 (account_id, date, exchange_id, trade_id) 第二笔 kSkip → DB 仍一行且均价不加倍;
/// 同 trade_id 不同 exchange_id → 两行 (多交易所同号不丢).
TEST_F(TdDataSyncIntegrationTest, TradeDedupAndCrossExchangeSameTradeId) {
    OrderRecord order = make_order(1, 10);
    order.base.status = DZ_ORDER_PART_TRADED;
    order.base.volume_traded = 0;
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = order});
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    auto day = load_day();
    TdTradeProducer producer(db_path_, day.orders, day.trades, day.start_seq);
    producer.start();

    const TradeRecord t1 = make_trade("T001", 0, "20260901", "CFFEX", 1, 3900.0, 1);
    EXPECT_FALSE(producer.on_trade(t1));  // 首笔转发
    const uint64_t seq_after_first = producer.last_seq();
    // 同键重放 → 吞, 不落库/不累计/不刷新 (seq 水位不变)
    EXPECT_TRUE(producer.on_trade(t1));
    EXPECT_EQ(producer.last_seq(), seq_after_first);
    EXPECT_EQ(producer.tracker().volume(1), 1);

    // 同 trade_id 不同 exchange_id → 真实第二笔成交, 转发 + 累计
    EXPECT_FALSE(producer.on_trade(make_trade("T001", 0, "20260901", "SHFE", 1, 4000.0, 1)));
    producer.flush();

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades WHERE account_id='acc1' AND trade_id='T001'"),
              2);  // 多交易所同号不丢
    EXPECT_EQ(producer.tracker().volume(1), 2);
    // 均价 = (3900*1 + 4000*1) / 2 = 3950
    EXPECT_DOUBLE_EQ(scalar_double("SELECT avg_price FROM orders WHERE order_id=1"), 3950.0);
    producer.stop();
}

/// 重启重建: 写「委托行 + 两笔成交」→ 新 load_trading_day → 跟踪器均价 = Σ价×量/Σ量,
/// 装载委托 avg_price 一致 → 重放同批委托全 kSkip (不重复落库/不产生新 seq).
TEST_F(TdDataSyncIntegrationTest, RestartRebuildsAvgAndReplayStaysSkip) {
    // ---- 生产端: 委托行 seq 10, 两笔成交 seq 11/13, 刷新行 seq 12/14 ----
    OrderRecord order = make_order(1, 10);
    order.base.status = DZ_ORDER_PART_TRADED;
    order.base.volume_traded = 0;
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = order});
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    auto base_day = load_day();
    {
        TdTradeProducer producer(db_path_, base_day.orders, base_day.trades, base_day.start_seq);
        producer.start();
        (void)producer.on_trade(make_trade("T001", 0, "20260901", "CFFEX", 1, 3900.0, 1));
        (void)producer.on_trade(make_trade("T002", 0, "20260901", "CFFEX", 1, 3903.0, 3));
        producer.flush();
        producer.stop();
    }
    const int64_t max_seq = scalar_int("SELECT MAX(seq) FROM orders WHERE account_id='acc1'");
    EXPECT_EQ(max_seq, 14);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE account_id='acc1'"), 1);  // upsert 覆盖

    // ---- 重启: 新登录日装载重建 ----
    auto day = load_day();
    ASSERT_EQ(day.orders.size(), 1u);
    ASSERT_EQ(day.trades.size(), 2u);
    EXPECT_EQ(day.start_seq, 14u);
    ASSERT_TRUE(day.order_avgs.avg_price(1).has_value());
    EXPECT_DOUBLE_EQ(*day.order_avgs.avg_price(1), 3902.25);  // Σ价×量/Σ量
    EXPECT_EQ(day.order_avgs.volume(1), 4);
    EXPECT_DOUBLE_EQ(day.orders[0].base.avg_price, 3902.25);  // 装载委托与聚合均价一致

    // ---- 重放同批委托 → 全 kSkip (DB 行数/seq 不变, 无新行) ----
    auto filter = ReportFilter::load(day.orders, day.trades);
    bool warn = false;
    EXPECT_EQ(ReportFilter::Verdict::kSkip, filter.check_order(order, &warn));
    EXPECT_FALSE(warn);
    for (const auto& t : day.trades) {
        EXPECT_EQ(ReportFilter::Verdict::kSkip, filter.check_trade(t));
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE account_id='acc1'"), 1);
    EXPECT_EQ(scalar_int("SELECT MAX(seq) FROM orders WHERE account_id='acc1'"), 14);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades WHERE account_id='acc1'"), 2);

    // ---- 重放同批成交经由转发路径 → 全吞, 累计不得二次计入 ----
    // (去重判定先于累计: kSkip 早退, 跟踪器与 DB 均不因重放变化)
    {
        TdTradeProducer replay(db_path_, day.orders, day.trades, day.start_seq);
        replay.start();
        for (const auto& t : day.trades) {
            EXPECT_TRUE(replay.on_trade(t)) << t.base.trade_id;
        }
        EXPECT_EQ(replay.tracker().volume(1), 4);  // 未二次累计
        EXPECT_DOUBLE_EQ(*replay.tracker().avg_price(1), 3902.25);
        EXPECT_EQ(replay.last_seq(), day.start_seq);  // 未分配任何 seq
        replay.flush();
        replay.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE account_id='acc1'"), 1);
    EXPECT_EQ(scalar_int("SELECT MAX(seq) FROM orders WHERE account_id='acc1'"), 14);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades WHERE account_id='acc1'"), 2);
    EXPECT_EQ(scalar_int("SELECT seq FROM orders WHERE order_id=1"), 14);
}

/// 刷新行不破坏去重基准: 只改 avg_price + 新 seq 的刷新行进入基准后,
/// 重放同一业务记录仍 kSkip (对比字段 update_time/volume_traded/volume_canceled/status 未变).
TEST_F(TdDataSyncIntegrationTest, RefreshedRowStillSkipsReplay) {
    OrderRecord order = make_order(1, 10);
    order.base.status = DZ_ORDER_PART_TRADED;
    order.base.volume_traded = 0;
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = order});
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    auto day = load_day();
    TdTradeProducer producer(db_path_, day.orders, day.trades, day.start_seq);
    producer.start();
    EXPECT_FALSE(producer.on_trade(make_trade("T001", 0, "20260901", "CFFEX", 1, 3900.0, 1)));
    producer.flush();
    ASSERT_TRUE(producer.tracker().avg_price(1).has_value());

    // 基准中已是刷新行 (avg_price=3900, seq=12), 重放原业务记录仍 kSkip
    bool warn = false;
    EXPECT_EQ(ReportFilter::Verdict::kSkip, producer.filter().check_order(order, &warn));
    EXPECT_FALSE(warn);
    // 再重放同一笔成交也 kSkip (成交去重键已入基准)
    EXPECT_TRUE(producer.on_trade(make_trade("T001", 0, "20260901", "CFFEX", 1, 3900.0, 1)));
    producer.stop();

    // 重启后重放同批: 委托与成交全 kSkip, DB 无新行
    auto day2 = load_day();
    auto filter2 = ReportFilter::load(day2.orders, day2.trades);
    bool warn2 = false;
    EXPECT_EQ(ReportFilter::Verdict::kSkip, filter2.check_order(order, &warn2));
    EXPECT_FALSE(warn2);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE account_id='acc1'"), 1);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades WHERE account_id='acc1'"), 1);
}

/// 成交先于委托回报到达: 基准无该 order_id → 只累计, 不推委托帧;
/// 委托回报到达时带走累计均价 (顺序无关).
TEST_F(TdDataSyncIntegrationTest, TradeBeforeOrderCarriesAvgOnOrderReport) {
    // 空基准启动
    auto day = load_day();
    ASSERT_TRUE(day.orders.empty());
    ASSERT_TRUE(day.trades.empty());
    TdTradeProducer producer(db_path_, day.orders, day.trades, 0);
    producer.start();

    // 成交先到: 只累计 (无基准行 → 不刷新), 成交自身仍转发落库
    EXPECT_FALSE(producer.on_trade(make_trade("T001", 0, "20260901", "CFFEX", 7, 3900.0, 1)));
    EXPECT_FALSE(producer.on_trade(make_trade("T002", 0, "20260901", "CFFEX", 7, 3902.0, 3)));
    ASSERT_TRUE(producer.tracker().avg_price(7).has_value());
    EXPECT_DOUBLE_EQ(*producer.tracker().avg_price(7), 3901.5);
    // 委托行尚未存在 (无刷新写入)
    producer.flush();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE order_id=7"), 0);

    // 委托回报到达: 带走累计均价
    OrderRecord order = make_order(7, 0);
    order.base.status = DZ_ORDER_PART_TRADED;
    order.base.volume_traded = 4;
    producer.on_order(order);
    producer.flush();
    EXPECT_DOUBLE_EQ(scalar_double("SELECT avg_price FROM orders WHERE order_id=7"), 3901.5);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE order_id=7"), 1);
    producer.stop();
}

/// 当日无成交的委托: 委托回报不得把均价清零 (跨日 GFD 单保留基准行均价);
/// 均价刷新也不产生多余 seq.
TEST_F(TdDataSyncIntegrationTest, OrderReportKeepsBaselineAvgWhenNoDayTrades) {
    // 基准行: 昨日成交留下的均价 3888.5 (DB 值), 当日无成交
    OrderRecord order = make_order(2, 10);
    order.base.status = DZ_ORDER_NOT_TRADED;
    order.base.volume_traded = 0;
    order.base.avg_price = 3888.5;
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = order});
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    auto day = load_day();
    ASSERT_EQ(day.orders.size(), 1u);
    EXPECT_TRUE(day.trades.empty());
    EXPECT_DOUBLE_EQ(day.orders[0].base.avg_price, 3888.5);  // 当日无成交保留 DB 值

    TdTradeProducer producer(db_path_, day.orders, day.trades, day.start_seq);
    producer.start();
    // 状态变化触发转发, 无当日成交累计 → 回填基准行均价 (不清零)
    OrderRecord update = make_order(2, 0);
    update.base.status = DZ_ORDER_CANCELLED;
    update.base.volume_traded = 0;
    update.update_time = 2000;
    producer.on_order(update);
    producer.flush();
    EXPECT_DOUBLE_EQ(scalar_double("SELECT avg_price FROM orders WHERE order_id=2"), 3888.5);
    producer.stop();
}

/// 均价未变化: 第二笔成交价与现均价相同 → 累计量增加但均价不变 → 不刷新委托行
/// (不推帧/不落库/不分配 seq; 契约 §3「均价变化」才刷新).
TEST_F(TdDataSyncIntegrationTest, UnchangedAvgDoesNotRefreshOrderRow) {
    OrderRecord order = make_order(1, 10);
    order.base.status = DZ_ORDER_PART_TRADED;
    order.base.volume_traded = 0;
    {
        PersistWriter w(db_path_, "dztd_test");
        w.open();
        w.start_writer();
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = order});
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    auto day = load_day();
    TdTradeProducer producer(db_path_, day.orders, day.trades, day.start_seq);
    producer.start();
    // 成交 1: 3900 x 1 → 均价 3900 → 刷新 (成交 seq 11, 刷新 seq 12)
    EXPECT_FALSE(producer.on_trade(make_trade("T001", 0, "20260901", "CFFEX", 1, 3900.0, 1)));
    EXPECT_EQ(producer.last_seq(), 12u);
    // 成交 2: 同价 3900 x 2 → 均价仍 3900 → 只分配成交 seq 13, 无刷新 seq
    EXPECT_FALSE(producer.on_trade(make_trade("T002", 0, "20260901", "CFFEX", 1, 3900.0, 2)));
    EXPECT_EQ(producer.last_seq(), 13u);  // 仅成交取号
    producer.flush();

    // 累计量确实增加 (累计口径独立于刷新), 但委托行未被重写
    EXPECT_EQ(producer.tracker().volume(1), 3);
    EXPECT_DOUBLE_EQ(*producer.tracker().avg_price(1), 3900.0);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT avg_price FROM orders WHERE order_id=1"), 3900.0);
    EXPECT_EQ(scalar_int("SELECT seq FROM orders WHERE order_id=1"), 12);  // 仍是首次刷新 seq
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades WHERE account_id='acc1'"), 2);
    producer.stop();
}

}  // namespace
}  // namespace dztrader::ctp
