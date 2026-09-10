#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/this_process.h>
#include <dztrader/trading/td_ingest.h>

#include "td/td_persist_writer.h"
#include "td/td_prescan.h"
#include "td/td_report_filter.h"
#include "td/td_schema.h"

namespace dztrader::ctp {
namespace {

/// 进程唯一临时目录名 (ctest -j 并行时避免多个测试 exe 共用固定目录名冲突).
std::filesystem::path unique_temp_dir(const std::string& name) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           (name + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
            "_" + std::to_string(dist(gen)));
}

/// 进程内组装 td 侧组件: 临时库 + PersistWriter 真线程 + ReportFilter + TdIngestGate.
/// 测试按场景手工编排生产者/消费者时序, 模拟 spec §6 场景矩阵.
class TdDataSyncIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = unique_temp_dir("dz_td_data_sync_it");
        std::filesystem::create_directories(tmp_dir_);
        db_path_ = (tmp_dir_ / "test.db").string();
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_ + "-journal");
    }
    void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

    /// 只读连接 (与生产 td_persist_writer.cpp:125 一致设置 busy_timeout,
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

    std::filesystem::path tmp_dir_;
    std::string db_path_;
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

/// 构造成交记录. (trading_day, trade_id) 为唯一区分点.
TradeRecord make_trade(const char* trade_id, uint64_t seq, const char* day = "20260901") {
    TradeRecord r{};
    r.base.seq = seq;
    std::strcpy(r.base.account_id, "acc1");
    std::strcpy(r.trading_day, day);
    std::strcpy(r.base.trade_id, trade_id);
    r.base.order_id = 1;
    std::strcpy(r.base.instrument_id, "IF2506");
    std::strcpy(r.base.exchange_id, "CFFEX");
    r.base.direction = DZ_DIRECTION_LONG;
    r.base.price = 3900.0;
    r.base.volume = 1;
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
        PersistWriter w(db_path_);
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

    // ---- 消费者启动: 独立只读连接查 W + 全量装载过滤器基准 ----
    SQLite::Database ro(db_path_, SQLite::OPEN_READONLY);
    const uint64_t W = query_max_seq(ro, "acc1");
    EXPECT_EQ(W, static_cast<uint64_t>(kCount));  // 四表取大 = orders 最大 seq

    auto filter = ReportFilter::load(load_orders(ro, "acc1"), load_trades(ro, "acc1"));

    // ---- 重放风暴: CTP 重放同 50 条 (同 order_id/同字段) → 全 kSkip ----
    for (const auto& rec : seed) {
        bool warn = false;
        EXPECT_EQ(ReportFilter::Verdict::kSkip, filter.check_order(rec, &warn)) << rec.base.order_id;
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
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();
        w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = changed});
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE account_id='acc1'"), kCount);  // 覆盖非新增
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
// 变体 A: 帧 101-110 写入 (seq 已分配) + persist 入队未提交 → 消费者 W=100
//         → 首帧 101 无断档正常应用 (DB 提交在后, W 读自快照, 不误报 gap)
// 变体 B: 帧 101-105 在 reader 开启前完成 (帧先于快照), DB 提交在后
//         → 首帧 106 → gap(101,105) → 回补查 DB (提交后) → 5 行返回
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, ConsumerRacesProducerInFlightWindow) {
    constexpr uint64_t kW = 100;

    // 先提交旧快照 seq 1..100 (W=100 的来源), 再在内存产生在途帧 101-105.
    {
        PersistWriter w(db_path_);
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

    // ===================== 变体 A: 首帧 101 无断档 =====================
    {
        // 生产者: 帧已写 (内存已见 seq 101-105), persist 入队但 Writer 未启动 (未提交)
        // 队列堆积, 无 Writer 线程持锁 → 消费者可无锁快照.
        PersistWriter w(db_path_);
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

        // gate 首帧 101 == W+1 → 无 gap, 正常应用
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

    // ===================== 变体 B: 首帧 106 → gap(101,105) → 回补 =====================
    {
        // 独立临时库: 先提交 1..100 (W=100 快照), 帧 101-105 在 reader 开启前完成写入,
        // DB 提交在后 (在途窗口).
        std::filesystem::path tmp2 = unique_temp_dir("dz_td_data_sync_it_b");
        std::filesystem::create_directories(tmp2);
        const std::string db2 = (tmp2 / "b.db").string();
        std::filesystem::remove(db2);
        std::filesystem::remove(db2 + "-journal");
        {
            PersistWriter w(db2);
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

        // 帧 101-105 在 reader 开启前完成写入 (内存), DB 提交在后.
        // open() 即建 schema; 不 start_writer → 队列堆积未提交, 读者无锁快照.
        PersistWriter w(db2);
        w.open();
        for (uint64_t s = 101; s <= 105; ++s) {
            auto r = make_order(static_cast<int64_t>(s), s);
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
        }

        // 消费者: reader 在 101-105 写完后开启 → 首个可见帧 = 106. W = 100 (旧快照).
        SQLite::Database ro(db2, SQLite::OPEN_READONLY);
        const uint64_t w_read = query_max_seq(ro, "acc1");
        EXPECT_EQ(w_read, 100u);

        // 首帧 106 → gate 标记 gap(101,105)
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
        auto backfilled = load_orders_since(ro2, "acc1", 100u);  // seq > 100
        ASSERT_EQ(backfilled.size(), 5u);
        EXPECT_EQ(backfilled.front().base.seq, 101u);
        EXPECT_EQ(backfilled.back().base.seq, 105u);

        std::filesystem::remove_all(tmp2);
    }
}

// ============================================================================
// 场景 3: 重置后重建 (spec §5.5 倒退检测 + reset + 新水位)
// gate 应用到 5001 → 收到倒退 seq 3 → detect_reset → reset(新W=2) → 新流正常
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, ResetThenRebuild) {
    dztrader::TdIngestGate gate;
    gate.set_watermark("acc1", 5000);
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kApply, gate.admit("acc1", 5001));

    // 生产端重置 (DB 清空/重加), 消费者收到倒退帧
    EXPECT_TRUE(gate.detect_reset("acc1", 3));
    gate.reset_account("acc1", 2);  // 重查 DB 得新 W=2

    // 重置后新流正常: 3 > 2 → kApply; 且 no gap (顺序首帧)
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kApply, gate.admit("acc1", 3));
    EXPECT_FALSE(gate.take_pending_gap().has_value());
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kSkip, gate.admit("acc1", 3));  // 防重

    // 倒退检测在新基准下重新生效
    EXPECT_TRUE(gate.detect_reset("acc1", 2));
}

// ============================================================================
// 场景 4: 重连重建基准 (spec §4.3; 覆盖审查③发现的 §4.3 遗漏:
// 预扫缓存须按连接生命周期刷新, 非 TdApi 一次性)
// 断开前 flush 一批 → 重连时重建 SessionBootData (增量装载 seq>上次水位)
// → 重放同一批全 kSkip (重建基准正确去重, 无重推风暴)
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, ReconnectRebuildsBaseline) {
    // ---- 第一次连接周期: 落一批入库 (seq 1..10), 基准 = 全量 ----
    std::vector<OrderRecord> batch1;
    for (int i = 1; i <= 10; ++i) {
        batch1.push_back(make_order(i, static_cast<uint64_t>(i)));
    }
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();
        for (const auto& r : batch1) {
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
        }
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }

    // 首次连接装载 (模拟 connect 时预扫缓存): 全量基准 + 水位
    SQLite::Database ro1(db_path_, SQLite::OPEN_READONLY);
    auto boot1 = prescan_accounts(ro1, {"acc1"});
    EXPECT_EQ(boot1.at("acc1").start_seq, 10u);
    auto filter1 = ReportFilter::load(boot1.at("acc1").orders, boot1.at("acc1").trades);
    for (const auto& rec : batch1) {
        EXPECT_EQ(ReportFilter::Verdict::kSkip, filter1.check_order(rec));
    }

    // ---- 运行期: 断连后又有新提交 (seq 11..15, 模拟断连期间生产端持续) ----
    std::vector<OrderRecord> batch2;
    for (int i = 11; i <= 15; ++i) {
        batch2.push_back(make_order(i, static_cast<uint64_t>(i)));
    }
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();
        for (const auto& r : batch2) {
            w.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = r});
        }
        auto token = w.enqueue_flush_signal();
        ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
        w.stop();
    }

    // ---- 重连: 预扫缓存必须按连接生命周期刷新 (非 TdApi 一次性) ----
    // 旧缓存若复用 (start_seq=10 且基准=seq1..10), 重放 seq 11..15 会被误判 kForward → 重推风暴.
    // 重连装载 = 全量重扫 (prescan_accounts 重新装载) → 基准含 11..15 → 重放全 kSkip.
    SQLite::Database ro2(db_path_, SQLite::OPEN_READONLY);
    auto boot2 = prescan_accounts(ro2, {"acc1"});
    EXPECT_EQ(boot2.at("acc1").start_seq, 15u);
    auto filter2 = ReportFilter::load(boot2.at("acc1").orders, boot2.at("acc1").trades);
    for (const auto& rec : batch2) {
        EXPECT_EQ(ReportFilter::Verdict::kSkip, filter2.check_order(rec));
    }
}

// ============================================================================
// 场景 5: 跨日累积 (spec §2.1 累积不归零 + §3.2 trades 唯一键含 trading_day)
// 两日 trades 同 trade_id 共存, W 跨日单调 (以最大 seq 为准, 不因跨日回落)
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, CrossDayAccumulation) {
    {
        PersistWriter w(db_path_);
        w.open();
        w.start_writer();

        // 第一日: trade T001 (seq 1), 第二日: trade T001 (seq 2, 同 trade_id 跨日)
        // 唯一键 (account_id, trading_day, trade_id) → 两行共存
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
    auto trades = load_trades(ro, "acc1");
    ASSERT_EQ(trades.size(), 2u);
    // 跨日同 trade_id 共存, seq 各不相同
    EXPECT_STREQ(trades[0].base.trade_id, "T001");
    EXPECT_STREQ(trades[1].base.trade_id, "T001");
    EXPECT_NE(trades[0].base.seq, trades[1].base.seq);

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
// 终检发现 4(c): 吞同不推进 W — 重放风暴全吞后, query_max_seq / DB MAX(seq)
// 不变 (kSkip 路径: 不推不落不分配 seq, 消费端水位不被重放污染)
// ============================================================================
TEST_F(TdDataSyncIntegrationTest, ReplayStormSkipsLeaveWatermarkUnchanged) {
    // 生产端落一批入库 (seq 1..30), W = 30
    constexpr int kCount = 30;
    std::vector<OrderRecord> seed;
    seed.reserve(kCount);
    for (int i = 1; i <= kCount; ++i) {
        seed.push_back(make_order(i, static_cast<uint64_t>(i)));
    }
    {
        PersistWriter w(db_path_);
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
    const int64_t rows_before = scalar_int("SELECT COUNT(*) FROM orders WHERE account_id='acc1'");

    // 消费端装载基准后重放同一批 → 全 kSkip (吞同)
    auto filter = ReportFilter::load(load_orders(ro, "acc1"), load_trades(ro, "acc1"));
    for (const auto& rec : seed) {
        bool warn = false;
        ASSERT_EQ(ReportFilter::Verdict::kSkip, filter.check_order(rec, &warn));
    }

    // 消费端水位 (gate) 不被重放推进: kSkip 后仍应按 W=30 过滤, 31 才放行
    dztrader::TdIngestGate gate;
    gate.set_watermark("acc1", w_before);
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kSkip, gate.admit("acc1", w_before));
    EXPECT_EQ(dztrader::TdIngestGate::Verdict::kApply, gate.admit("acc1", w_before + 1));

    // DB 无新行, MAX(seq) 不变 (kSkip: 不推不落不分配 seq)
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE account_id='acc1'"), rows_before);
    EXPECT_EQ(query_max_seq(ro, "acc1"), w_before);
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
    PersistWriter w(db_path_);
    w.open();
    w.start_writer();
    // 1) 单行增量 upsert
    w.enqueue(PersistTask{.kind = PersistTask::Kind::Position,
                          .data = std::vector<DzPositionInfo>{make_pos("IF2506", 5)},
                          .account_id = "acc1",
                          .trading_day = 20260910});
    auto token = w.enqueue_flush_signal();
    ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
    EXPECT_EQ(scalar_int("SELECT seq FROM positions WHERE account_id='acc1' AND instrument_id='IF2506'"), 5);
    // 2) 重灌: 未变化行沿用 seq 5 + 新行 seq 6
    w.enqueue(PersistTask{.kind = PersistTask::Kind::PositionRebuild,
                          .data = std::vector<DzPositionInfo>{make_pos("IF2506", 5), make_pos("rb2510", 6)},
                          .account_id = "acc1",
                          .trading_day = 20260910});
    token = w.enqueue_flush_signal();
    ASSERT_TRUE(w.wait_flush(token, std::chrono::seconds(5)));
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 2);
    EXPECT_EQ(scalar_int("SELECT seq FROM positions WHERE account_id='acc1' AND instrument_id='IF2506'"), 5);
    EXPECT_EQ(scalar_int("SELECT seq FROM positions WHERE account_id='acc1' AND instrument_id='rb2510'"), 6);
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

}  // namespace
}  // namespace dztrader::ctp
