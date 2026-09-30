#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/string_util.h>
#include <dztrader/date_time/date.h>
#include <dztrader/db/database.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/writer.h>
#include <dztrader/struct.h>
#include <dztrader/tdstore/schema_catalog.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <filesystem>
#include <string>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::FrameView;
using dztrader::shm::MultiWriter;

namespace {

constexpr uint64_t kMB = 1024 * 1024;

void create_event_channel(const std::filesystem::path& shm_dir) {
    ChannelConfig cfg{
        .channel_name = dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
        .shm_dir = shm_dir,
        .meta_file_size = 1 * kMB,
        .page_size = 1 * kMB,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    (void)ChannelMeta::open_or_create(cfg);
}

void create_md_channel(const std::filesystem::path& shm_dir) {
    ChannelConfig cfg{
        .channel_name = "test_md",
        .shm_dir = shm_dir,
        .meta_file_size = 1 * kMB,
        .page_size = 1 * kMB,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    (void)ChannelMeta::open_or_create(cfg);
}

/// SDK ingest 接线端到端: 临时 DZTRADER_HOME + 预置统一 td 库 (db/td.db)。
/// 独立二进制 (独立进程): paths::home() 按进程缓存, 本 fixture 先设 DZTRADER_HOME
/// 再 dz_init, 保证库路径发现落在本测试目录。
class IngestWiringTest : public ::testing::Test {
protected:
    std::string home_;
    std::filesystem::path db_path_;
    DzContext* ctx_ = nullptr;

    void SetUp() override {
        home_ = (std::filesystem::temp_directory_path() / "dz_test_strategy_ingest_wiring")
                    .string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        std::filesystem::create_directories(home_ + "/db");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
        create_event_channel(home_ + "/shm");
        create_md_channel(home_ + "/shm");

        db_path_ = std::filesystem::path(home_) / "db" / "td.db";
        {
            auto database = dztrader::db::Database::open(
                dztrader::db::Config{.backend = "sqlite",
                                     .options = {{"path", db_path_.string()}}},
                dztrader::tdstore::schemas());
            database->migrate();
        }
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        // 预置快照行: 账户 CTP001, orders seq 1..5 (W 快照 = MAX(seq) = 5)
        {
            SQLite::Statement ins(db,
                "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
                " exchange_id, direction, position_effect, price_type, status, price, volume,"
                " volume_traded, strategy_id, seq)"
                " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
                " ?, ?, ?, 'stale', ?)");
            for (int64_t seq = 1; seq <= 5; ++seq) {
                ins.bind(1, static_cast<int64_t>(1000 + seq));
                ins.bind(2, 3800.0 + static_cast<double>(seq));
                ins.bind(3, static_cast<int64_t>(1));
                ins.bind(4, static_cast<int64_t>(1));
                ins.bind(5, static_cast<int64_t>(seq));
                ins.exec();
                ins.reset();
            }
        }

        ctx_ = dz_init();  // 装载水位: CTP001 -> W=5
        ASSERT_NE(nullptr, ctx_) << "dz_init failed: " << dz_errmsg();
    }

    void TearDown() override {
        dz_release(ctx_);
        std::filesystem::remove_all(home_);
    }

    std::shared_ptr<ChannelMeta> open_event_meta() {
        return std::make_shared<ChannelMeta>(ChannelMeta::open_only(
            dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT), home_ + "/shm"));
    }

    /// basic struct 帧 (任意对齐 struct payload)
    template <typename T>
    void emit_struct(DzFrameType type, const T& payload) {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "ingest_wiring_writer");
        ASSERT_TRUE(writer.write_frame(type, payload));
        writer.notify_subscribers();
    }
};

// W 过滤: seq ≤ W (快照已含) 的帧被拦截, 不返回策略用户。
TEST_F(IngestWiringTest, WatermarkFilterSkipsSnapshotFrames) {
    DzOrderReport rpt{};
    dztrader::copy_string(rpt.account_id, "CTP001", true);
    dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
    rpt.seq = 5;  // ≤ W=5
    emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    EXPECT_EQ(nullptr, dz_next_event(ctx_));  // 快照已含: 拦截
}

// 断档回补: 首帧 seq > W+1 → gap → 查库回补区间行 → 前置 replay FIFO 派发。
// 触发帧 (seq=8) 先返回, 其后回补帧按 seq 序送达, 实时帧 (seq=9) 在后。
TEST_F(IngestWiringTest, GapTriggersBackfillReplay) {
    // 模拟启动竞态窗口: 快照查询后落库的行 (在途窗口事件), 首帧到达前已提交
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " ?, ?, ?, ?, ?)");
        const char* own = dz_strategy_id(ctx_);
        for (int64_t seq = 6; seq <= 7; ++seq) {
            ins.bind(1, static_cast<int64_t>(2000 + seq));
            ins.bind(2, 3900.0 + static_cast<double>(seq));
            ins.bind(3, static_cast<int64_t>(2));
            ins.bind(4, static_cast<int64_t>(2));
            ins.bind(5, own);
            ins.bind(6, static_cast<int64_t>(seq));
            ins.exec();
            ins.reset();
        }
    }

    // 首帧 seq=8 (缺口 6,7) 触发断档
    DzOrderReport trigger{};
    dztrader::copy_string(trigger.account_id, "CTP001", true);
    dztrader::copy_string(trigger.strategy_id, dz_strategy_id(ctx_), true);
    trigger.seq = 8;
    emit_struct(DZ_FRAME_ORDER_REPORT, trigger);

    // 触发帧先返回
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(8u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);

    // 回补帧前置派发: seq 6, 7 (FIFO)
    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(6u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);
    EXPECT_STREQ("IF2603",
                 FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().instrument_id);

    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(7u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);

    // 回补派发完: 通道空 -> nullptr
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
}

// 回补帧按 strategy_id 定向: 他策略/外部单 (strategy_id 空) 的回补订单不回放给本策略。
TEST_F(IngestWiringTest, BackfillReplayFiltersByStrategy) {
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " ?, ?, ?, ?, ?)");
        // seq 6: 他策略; seq 7: 外部单 (strategy_id 空)
        ins.bind(1, static_cast<int64_t>(2001));
        ins.bind(2, 3901.0);
        ins.bind(3, static_cast<int64_t>(2));
        ins.bind(4, static_cast<int64_t>(2));
        ins.bind(5, "other_strategy");
        ins.bind(6, static_cast<int64_t>(6));
        ins.exec();
        ins.reset();
        ins.bind(1, static_cast<int64_t>(2002));
        ins.bind(2, 3902.0);
        ins.bind(3, static_cast<int64_t>(2));
        ins.bind(4, static_cast<int64_t>(2));
        ins.bind(5, "");
        ins.bind(6, static_cast<int64_t>(7));
        ins.exec();
    }

    DzOrderReport trigger{};
    dztrader::copy_string(trigger.account_id, "CTP001", true);
    dztrader::copy_string(trigger.strategy_id, dz_strategy_id(ctx_), true);
    trigger.seq = 8;
    emit_struct(DZ_FRAME_ORDER_REPORT, trigger);

    // 触发帧返回
    ASSERT_NE(nullptr, dz_next_event(ctx_));
    // 回补帧 (6,7) 均非本策略: 被定向过滤, 无回放
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
}

// 评审发现 1 (wiring): TRADE_REPORT dispatch 接 admit_trade — 同 (account, day, trade_id)
// 的重复成交被拦截 (SDK 二道防线), 不返回策略用户。
TEST_F(IngestWiringTest, TradeDedupWiredInDispatch) {
    const char* own = dz_strategy_id(ctx_);
    // 首笔成交: seq=6 (W=5) 应用并通过
    {
        DzTradeReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, own, true);
        dztrader::copy_string(rpt.trade_id, "T1", true);
        rpt.date = dztrader::Date::from_year_month_day(2026, 9, 1).days_since_epoch();
        rpt.seq = 6;
        emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    }
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_TRADE_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(6u, FrameView(static_cast<const std::byte*>(f)).payload<DzTradeReport>().seq);

    // 同 trade_id 重复帧: seq=7 且 seq>W, admit(seq) 放行, 但 admit_trade 二道防线拦截
    {
        DzTradeReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, own, true);
        dztrader::copy_string(rpt.trade_id, "T1", true);
        rpt.date = dztrader::Date::from_year_month_day(2026, 9, 1).days_since_epoch();
        rpt.seq = 7;
        emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    }
    EXPECT_EQ(nullptr, dz_next_event(ctx_));  // 二道防线拦截: 不返回策略用户

    // 新 trade_id: seq=8 正常放行
    {
        DzTradeReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, own, true);
        dztrader::copy_string(rpt.trade_id, "T2", true);
        rpt.date = dztrader::Date::from_year_month_day(2026, 9, 1).days_since_epoch();
        rpt.seq = 8;
        emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    }
    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(8u, FrameView(static_cast<const std::byte*>(f)).payload<DzTradeReport>().seq);
}

// 评审发现 1 (wiring): ACCOUNT_STATUS (2018) 携带 trading_day 时触发 on_trading_day_changed —
// 日切后同 (account, trade_id) 在新交易日的成交不再被旧日段拦截。
TEST_F(IngestWiringTest, AccountStatusTradingDayChangeClearsTradeDedup) {
    const char* own = dz_strategy_id(ctx_);
    auto day_dz = [](int y, int m, int d) {
        return dztrader::Date::from_year_month_day(y, m, d).days_since_epoch();
    };
    // 首笔成交: 2026-09-01, seq=6
    {
        DzTradeReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, own, true);
        dztrader::copy_string(rpt.trade_id, "T1", true);
        rpt.date = day_dz(2026, 9, 1);
        rpt.seq = 6;
        emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    }
    ASSERT_NE(nullptr, dz_next_event(ctx_));

    // 同日重复: seq=7 被二道防线拦截
    {
        DzTradeReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, own, true);
        dztrader::copy_string(rpt.trade_id, "T1", true);
        rpt.date = day_dz(2026, 9, 1);
        rpt.seq = 7;
        emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    }
    EXPECT_EQ(nullptr, dz_next_event(ctx_));

    // 2018 ACCOUNT_STATUS 携带新交易日 2026-09-02: 触发 on_trading_day_changed, 清旧日段。
    // DZ_FRAME_ACCOUNT_STATUS 仍全量放行给策略用户 (on_account_status 回调), 故返回该帧。
    {
        DzAccountStatus st{};
        dztrader::copy_string(st.account_id, "CTP001", true);
        st.trading_day = day_dz(2026, 9, 2);
        st.state = DZ_ACCOUNT_READY;
        emit_struct(DZ_FRAME_ACCOUNT_STATUS, st);
    }
    const void* stf = dz_next_event(ctx_);
    ASSERT_NE(stf, nullptr);
    EXPECT_EQ(DZ_FRAME_ACCOUNT_STATUS, FrameView(static_cast<const std::byte*>(stf)).type());

    // 新交易日同 trade_id: 不被旧日段拦截 (seq=8)
    {
        DzTradeReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, own, true);
        dztrader::copy_string(rpt.trade_id, "T1", true);
        rpt.date = day_dz(2026, 9, 2);
        rpt.seq = 8;
        emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    }
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(8u, FrameView(static_cast<const std::byte*>(f)).payload<DzTradeReport>().seq);
}

// 评审发现 1: TRADE_REPORT 非本策略 (strategy_id 不匹配) 在 admit_trade 之前即被定向过滤,
// 不污染去重段 (他策略同名 trade_id 不影响本策略)。
TEST_F(IngestWiringTest, TradeDedupAccountedPerStrategy) {
    const char* own = dz_strategy_id(ctx_);
    // 本策略成交 seq=6
    {
        DzTradeReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, own, true);
        dztrader::copy_string(rpt.trade_id, "T9", true);
        rpt.seq = 6;
        emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    }
    ASSERT_NE(nullptr, dz_next_event(ctx_));
    // 他策略同 trade_id seq=7: is_own_report 拦截, 去重段不入账
    {
        DzTradeReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, "other", true);
        dztrader::copy_string(rpt.trade_id, "T9", true);
        rpt.seq = 7;
        emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    }
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
    // 本策略新成交 seq=8: 不受他策略影响 (若 admit_trade 在定向之前执行, 此处会被误拦)
    {
        DzTradeReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, own, true);
        dztrader::copy_string(rpt.trade_id, "T9", true);
        rpt.seq = 8;
        emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    }
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(8u, FrameView(static_cast<const std::byte*>(f)).payload<DzTradeReport>().seq);
}

// 评审发现 2 (回补帧 time=0): orders 回补解析 DB update_time/insert_time (epoch 秒)
// 的当日秒填 rpt.time; trades 回补解析 trade_time。
TEST_F(IngestWiringTest, BackfillFillsOrderAndTradeTime) {
    // 回补区间 6..7: orders 两行 + trades 两行, 均填时间戳
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, insert_time, update_time, seq)"
            " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " ?, ?, ?, ?, ?, ?, ?)");
        const char* own = dz_strategy_id(ctx_);
        // seq 6: 只有 insert_time (update_time=0) -> 用 insert_time 当日秒
        ins.bind(1, static_cast<int64_t>(3001));
        ins.bind(2, 4100.0);
        ins.bind(3, static_cast<int64_t>(1));
        ins.bind(4, static_cast<int64_t>(1));
        ins.bind(5, own);
        ins.bind(6, static_cast<int64_t>(4 * 86400 + 9 * 3600 + 15 * 60 + 5));  // insert 09:15:05
        ins.bind(7, static_cast<int64_t>(0));  // update_time 空
        ins.bind(8, static_cast<int64_t>(6));
        ins.exec();
        ins.reset();
        // seq 7: update_time 非 0 -> 用 update_time 当日秒 (11:30:45)
        ins.bind(1, static_cast<int64_t>(3002));
        ins.bind(2, 4200.0);
        ins.bind(3, static_cast<int64_t>(2));
        ins.bind(4, static_cast<int64_t>(2));
        ins.bind(5, own);
        ins.bind(6, static_cast<int64_t>(4 * 86400 + 9 * 3600 + 16 * 60 + 0));  // insert 09:16:00
        ins.bind(7, static_cast<int64_t>(4 * 86400 + 11 * 3600 + 30 * 60 + 45));  // update 11:30:45
        ins.bind(8, static_cast<int64_t>(7));
        ins.exec();

        SQLite::Statement tins(db,
            "INSERT INTO trades (account_id, trading_day, trade_id, order_id, instrument_id,"
            " exchange_id, direction, position_effect, price, volume, trade_time, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, ?, 'IF2603', 'CFFEX', '1', '1', ?, ?, ?, ?, ?)");
        tins.bind(1, "TT1");
        tins.bind(2, static_cast<int64_t>(3001));
        tins.bind(3, 4100.0);
        tins.bind(4, static_cast<int64_t>(1));
        tins.bind(5, static_cast<int64_t>(4 * 86400 + 10 * 3600 + 1 * 60 + 2));  // trade 10:01:02
        tins.bind(6, own);
        tins.bind(7, static_cast<int64_t>(6));
        tins.exec();
        tins.reset();
        tins.bind(1, "TT2");
        tins.bind(2, static_cast<int64_t>(3002));
        tins.bind(3, 4200.0);
        tins.bind(4, static_cast<int64_t>(1));
        tins.bind(5, static_cast<int64_t>(4 * 86400 + 12 * 3600 + 3 * 60 + 4));  // trade 12:03:04
        tins.bind(6, own);
        tins.bind(7, static_cast<int64_t>(7));
        tins.exec();
    }

    // 首帧 seq=8 触发断档 (缺口 6,7)
    DzOrderReport trigger{};
    dztrader::copy_string(trigger.account_id, "CTP001", true);
    dztrader::copy_string(trigger.strategy_id, dz_strategy_id(ctx_), true);
    trigger.seq = 8;
    emit_struct(DZ_FRAME_ORDER_REPORT, trigger);

    // 触发帧 seq=8
    ASSERT_NE(nullptr, dz_next_event(ctx_));

    // 回补 FIFO: 先 orders (表序), seq 6, 7
    {
        const void* f = dz_next_event(ctx_);
        ASSERT_NE(f, nullptr);
        const auto view = FrameView(static_cast<const std::byte*>(f));
        EXPECT_EQ(DZ_FRAME_ORDER_REPORT, view.type());
        const DzOrderReport& o6 = view.payload<DzOrderReport>();
        EXPECT_EQ(6u, o6.seq);
        EXPECT_EQ(9 * 3600 + 15 * 60 + 5, o6.time);  // insert_time 当日秒
    }
    {
        const void* f = dz_next_event(ctx_);
        ASSERT_NE(f, nullptr);
        const auto view = FrameView(static_cast<const std::byte*>(f));
        const DzOrderReport& o7 = view.payload<DzOrderReport>();
        EXPECT_EQ(7u, o7.seq);
        EXPECT_EQ(11 * 3600 + 30 * 60 + 45, o7.time);  // update_time 当日秒
    }

    // trades 回补
    {
        const void* f = dz_next_event(ctx_);
        ASSERT_NE(f, nullptr);
        const auto view = FrameView(static_cast<const std::byte*>(f));
        EXPECT_EQ(DZ_FRAME_TRADE_REPORT, view.type());
        const DzTradeReport& t1 = view.payload<DzTradeReport>();
        EXPECT_EQ(6u, t1.seq);
        EXPECT_EQ(10 * 3600 + 1 * 60 + 2, t1.time);  // trade_time 当日秒
    }
    {
        const void* f = dz_next_event(ctx_);
        ASSERT_NE(f, nullptr);
        const auto view = FrameView(static_cast<const std::byte*>(f));
        const DzTradeReport& t2 = view.payload<DzTradeReport>();
        EXPECT_EQ(7u, t2.seq);
        EXPECT_EQ(12 * 3600 + 3 * 60 + 4, t2.time);
    }

    EXPECT_EQ(nullptr, dz_next_event(ctx_));
}

// 评审发现 3 (回补缓冲溢出): gap 区间过宽导致回补帧数 > 缓冲容量时, 触发帧被拦截
// (宁缺勿乱, 不再静默丢帧后由 DB "兜底" 的假象)。容量 512, 溢出需 >512 帧。
TEST_F(IngestWiringTest, BackfillBufferOverflowInterceptsTrigger) {
    // 用 W=5 账户, gap 宽 600 (seq 6..605), orders 表 600 行 > 512 槽, 必然溢出。
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " ?, ?, ?, ?, ?)");
        for (int64_t seq = 6; seq <= 605; ++seq) {
            ins.bind(1, static_cast<int64_t>(4000 + seq));
            ins.bind(2, 4300.0 + static_cast<double>(seq));
            ins.bind(3, static_cast<int64_t>(1));
            ins.bind(4, static_cast<int64_t>(1));
            ins.bind(5, dz_strategy_id(ctx_));
            ins.bind(6, static_cast<int64_t>(seq));
            ins.exec();
            ins.reset();
        }
    }

    // 首帧 seq=606 (gap 6..605, orders 600 行) 触发; 600 帧 > 512 槽溢出。
    DzOrderReport trigger{};
    dztrader::copy_string(trigger.account_id, "CTP001", true);
    dztrader::copy_string(trigger.strategy_id, dz_strategy_id(ctx_), true);
    trigger.seq = 606;
    emit_struct(DZ_FRAME_ORDER_REPORT, trigger);

    // 溢出拦截: 触发帧的 dispatch 在首次 dz_next_event 内发生 (handle_gap 入 512 帧后
    // 返回 false 吞掉触发帧, 该次调用返回 nullptr); 其后调用派发 512 回补帧。
    // 关键断言 (区分旧静默丢帧行为): 旧代码回补 64 帧后触发帧放行 (drained=64 后
    // 非空), 新代码 512 帧后触发帧被拦截 (drained=512 后彻底空)。
    uint32_t drained = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const void* f;
        while ((f = dz_next_event(ctx_)) != nullptr) {
            ++drained;
        }
    }
    EXPECT_EQ(512u, drained);  // 512 槽塞满, 触发帧被拦截 (非 64+触发帧放行)
}

// 评审发现 4: 端到端 reset-re-admit — detect_reset (seq 倒退) → rebuild → reset →
// 新帧不被 W 过滤 (重置后新水位下正常应用)。验证"重置后新帧不被 W 过滤"链路。
TEST_F(IngestWiringTest, ResetReAdmitNotFilteredByWatermark) {
    // 账户 CTP001 W=5 (SetUp 预置). 先应用 seq=6,7 建立 last_applied=7
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = 6;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    ASSERT_NE(nullptr, dz_next_event(ctx_));
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = 7;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    ASSERT_NE(nullptr, dz_next_event(ctx_));

    // 重置场景: DB 清空 (模拟数据被重置, 库重新从 seq=1 开始), 新帧 seq=3 < last_applied=7
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        db.exec("DELETE FROM orders");
        // 新基准 seq 1,2 (重置后 DB 从头累计): 重置后首帧 seq=3 的 gap [1,2]
        // 可由 DB 覆盖 (回补重试语义: 行存在则不拦截)
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " ?, ?, ?, 'own', ?)");
        for (int64_t seq = 1; seq <= 2; ++seq) {
            ins.bind(1, static_cast<int64_t>(7000 + seq));
            ins.bind(2, 4700.0 + static_cast<double>(seq));
            ins.bind(3, static_cast<int64_t>(1));
            ins.bind(4, static_cast<int64_t>(1));
            ins.bind(5, static_cast<int64_t>(seq));
            ins.exec();
            ins.reset();
        }
    }
    DzOrderReport rpt{};
    dztrader::copy_string(rpt.account_id, "CTP001", true);
    dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
    rpt.seq = 3;  // < last_applied=7 -> detect_reset 触发
    emit_struct(DZ_FRAME_ORDER_REPORT, rpt);

    // rebuild_watermark: DB 重建后 W=2; reset_account(CTP001, 2). 新帧 seq=3 > W=2 应用
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(3u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);
}

// 评审发现 4: 逐表容错 — positions 表缺失时, orders 区间行仍回补 (覆盖判定按
// "查得行数 ≥ 区间宽", 缺失表按 0 行计; 区间可由现存表完整覆盖则立即放行)。
TEST_F(IngestWiringTest, BackfillToleratesMissingTable) {
    // positions 表缺失 (其余表已建); gap 区间 [6,6] 仅 orders 一行
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        db.exec("DROP TABLE positions");
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " ?, ?, ?, ?, ?)");
        ins.bind(1, static_cast<int64_t>(5001));
        ins.bind(2, 4500.0);
        ins.bind(3, static_cast<int64_t>(1));
        ins.bind(4, static_cast<int64_t>(1));
        ins.bind(5, dz_strategy_id(ctx_));
        ins.bind(6, static_cast<int64_t>(6));
        ins.exec();
    }

    DzOrderReport trigger{};
    dztrader::copy_string(trigger.account_id, "CTP001", true);
    dztrader::copy_string(trigger.strategy_id, dz_strategy_id(ctx_), true);
    trigger.seq = 7;  // gap [6,6]
    emit_struct(DZ_FRAME_ORDER_REPORT, trigger);

    // 触发帧返回
    ASSERT_NE(nullptr, dz_next_event(ctx_));
    // orders 回补 seq=6 正常送达 (positions 表缺失被跳过, 不阻断覆盖判定)
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(6u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
}

// 评审发现 4: 无水位降级 (DB 缺失全放行) — 见 td_ingest_nodb_test.cpp (需独立进程:
// paths::home() 按进程缓存 + context_registry 进程全局, 无法与本 fixture 共存)。

// 终检发现 4(a): 回补 2002/2003 路径 — gap 区间的 positions/trading_accounts 行
// 经回补派发为 POSITION_INFO / TRADING_ACCOUNT 帧 (列序/seq 正确)。
TEST_F(IngestWiringTest, BackfillDispatchesPositionAndTradingAccount) {
    // gap 区间 6,7: positions 一行 (seq 6) + trading_accounts 一行 (seq 7)
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        SQLite::Statement pins(db,
            "INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id,"
            " direction, volume, frozen_volume, today_volume, yd_volume, price, seq)"
            " VALUES ('CTP001', '20260901', 'IF2603', 'CFFEX', '1', 3, 1, 3, 2, 3950.5, 6)");
        pins.exec();
        SQLite::Statement ains(db,
            "INSERT INTO trading_accounts (account_id, trading_day, balance, available, frozen,"
            " commission, margin, withdraw_quota, deposit, withdraw, seq)"
            " VALUES ('CTP001', '20260901', 100000.5, 90000.25, 1000.0, 12.5, 9000.0,"
            " 500.0, 2000.0, 1000.0, 7)");
        ains.exec();
    }

    // 首帧 seq=8 触发断档 (缺口 6,7)
    DzOrderReport trigger{};
    dztrader::copy_string(trigger.account_id, "CTP001", true);
    dztrader::copy_string(trigger.strategy_id, dz_strategy_id(ctx_), true);
    trigger.seq = 8;
    emit_struct(DZ_FRAME_ORDER_REPORT, trigger);

    // 触发帧先返回
    ASSERT_NE(nullptr, dz_next_event(ctx_));

    // 回补: positions 行 → DZ_FRAME_POSITION_INFO (帧 DZ_FRAME_POSITION_INFO)
    {
        const void* f = dz_next_event(ctx_);
        ASSERT_NE(f, nullptr);
        const auto view = FrameView(static_cast<const std::byte*>(f));
        EXPECT_EQ(DZ_FRAME_POSITION_INFO, view.type());
        const DzPositionInfo& pos = view.payload<DzPositionInfo>();
        EXPECT_STREQ("CTP001", pos.account_id);
        EXPECT_STREQ("IF2603", pos.instrument_id);
        EXPECT_STREQ("CFFEX", pos.exchange_id);
        EXPECT_EQ(DZ_DIRECTION_LONG, pos.direction);
        EXPECT_EQ(3u, pos.volume);
        EXPECT_EQ(1u, pos.frozen_volume);
        EXPECT_EQ(3u, pos.today_volume);
        EXPECT_EQ(2u, pos.yd_volume);
        EXPECT_DOUBLE_EQ(3950.5, pos.price);
        EXPECT_EQ(6u, pos.seq);
    }

    // 回补: trading_accounts 行 → DZ_FRAME_TRADING_ACCOUNT (帧 DZ_FRAME_TRADING_ACCOUNT)
    {
        const void* f = dz_next_event(ctx_);
        ASSERT_NE(f, nullptr);
        const auto view = FrameView(static_cast<const std::byte*>(f));
        EXPECT_EQ(DZ_FRAME_TRADING_ACCOUNT, view.type());
        const DzTradingAccount& acct = view.payload<DzTradingAccount>();
        EXPECT_STREQ("CTP001", acct.account_id);
        EXPECT_DOUBLE_EQ(100000.5, acct.balance);
        EXPECT_DOUBLE_EQ(90000.25, acct.available);
        EXPECT_DOUBLE_EQ(1000.0, acct.frozen);
        EXPECT_DOUBLE_EQ(12.5, acct.commission);
        EXPECT_DOUBLE_EQ(9000.0, acct.margin);
        EXPECT_DOUBLE_EQ(500.0, acct.withdraw_quota);
        EXPECT_DOUBLE_EQ(2000.0, acct.deposit);
        EXPECT_DOUBLE_EQ(1000.0, acct.withdraw);
        EXPECT_EQ(7u, acct.seq);
    }

    EXPECT_EQ(nullptr, dz_next_event(ctx_));
}

// 终检发现 3: 回补重试 — gap 区间行未提交 (触发帧到达 ≠ persist 已提交) 时首次
// 回补不足 → 触发帧拦截暂存, 后续 dz_next_event 每次调用重试一次; 行提交后重试
// 覆盖 → 回补帧先行、暂存触发帧随后 (终检发现 B: 按 seq 序派发, 回补行最小在前)。
TEST_F(IngestWiringTest, GapBackfillRetriesUntilRowsCommitted) {
    // gap 区间 6,7 无行 (persist 未提交)
    DzOrderReport trigger{};
    dztrader::copy_string(trigger.account_id, "CTP001", true);
    dztrader::copy_string(trigger.strategy_id, dz_strategy_id(ctx_), true);
    trigger.seq = 8;
    emit_struct(DZ_FRAME_ORDER_REPORT, trigger);

    // 首次调用: 回补不足, 触发帧被拦截 (宁缺勿乱)
    EXPECT_EQ(nullptr, dz_next_event(ctx_));

    // 重试期间行仍未提交: 继续拦截 (每次调用查一次)
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
    EXPECT_EQ(nullptr, dz_next_event(ctx_));

    // 模拟 persist 提交: gap 行入库
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " ?, ?, ?, ?, ?)");
        const char* own = dz_strategy_id(ctx_);
        for (int64_t seq = 6; seq <= 7; ++seq) {
            ins.bind(1, static_cast<int64_t>(6000 + seq));
            ins.bind(2, 4600.0 + static_cast<double>(seq));
            ins.bind(3, static_cast<int64_t>(2));
            ins.bind(4, static_cast<int64_t>(2));
            ins.bind(5, own);
            ins.bind(6, static_cast<int64_t>(seq));
            ins.exec();
            ins.reset();
        }
    }

    // 重试覆盖: 按 seq 序派发 — 回补(6,7) → 触发帧(8) (终检发现 B: 回补行最小在前,
    // 触发帧经 replay 缓冲在其后, 避免同键旧状态覆盖新状态)
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(6u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);

    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(7u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);

    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(8u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);

    EXPECT_EQ(nullptr, dz_next_event(ctx_));
}

// 终检发现 3: 重试耗尽放行 — gap 区间行永不提交 (td 崩溃丢失) 时, 重试达上限
// (kGapRetryMax=10) 后放行暂存触发帧 + 不派发任何回补帧 (按"崩溃丢失"语义)。
TEST_F(IngestWiringTest, GapBackfillRetryExhaustedReleasesTrigger) {
    DzOrderReport trigger{};
    dztrader::copy_string(trigger.account_id, "CTP001", true);
    dztrader::copy_string(trigger.strategy_id, dz_strategy_id(ctx_), true);
    trigger.seq = 8;
    emit_struct(DZ_FRAME_ORDER_REPORT, trigger);

    // 首次调用: 回补不足, 触发帧拦截 (attempts=1); 其后每次调用重试一次
    // (attempts 2..10), 第 10 次耗尽 → 触发帧经 replay 缓冲补发 (该次调用返回)。
    // 全程不向 DB 插入 gap 行。
    const void* f = nullptr;
    uint32_t drained = 0;
    uint64_t first_seq = 0;
    for (int call = 0; call < 20 && drained == 0; ++call) {
        f = dz_next_event(ctx_);
        if (f != nullptr) {
            ++drained;
            first_seq = FrameView(static_cast<const std::byte*>(f))
                            .payload<DzOrderReport>()
                            .seq;
        }
    }
    ASSERT_NE(f, nullptr);       // 耗尽后触发帧确实放行 (不永久卡死)
    EXPECT_EQ(1u, drained);      // 只放行触发帧, 无回补帧 (行确已丢失)
    EXPECT_EQ(8u, first_seq);    // 放行的正是暂存的触发帧
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
}

// 评审发现 4: DZ_FRAME_POSITION_INFO/DZ_FRAME_TRADING_ACCOUNT 帧 (POSITION_INFO / TRADING_ACCOUNT) 过 gate — W 过滤拦截
// seq ≤ W 的帧, seq > W 放行 (全量放行语义, 不经 strategy_id 定向)。
// seq 取值与 DB 快照连续 (W=5, 首帧 6): 避免合成空洞触发回补重试 (见发现 3 测试)。
TEST_F(IngestWiringTest, PositionAndTradingAccountGoThroughGate) {    // seq ≤ W=5: 拦截
    {
        DzPositionInfo p{};
        dztrader::copy_string(p.account_id, "CTP001", true);
        p.seq = 5;
        emit_struct(DZ_FRAME_POSITION_INFO, p);
    }
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
    // seq > W: 放行 (首帧 6 == W+1, 无断档)
    {
        DzPositionInfo p{};
        dztrader::copy_string(p.account_id, "CTP001", true);
        p.seq = 6;
        emit_struct(DZ_FRAME_POSITION_INFO, p);
    }
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_POSITION_INFO, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(6u, FrameView(static_cast<const std::byte*>(f)).payload<DzPositionInfo>().seq);

    // TRADING_ACCOUNT 同语义: seq=6 被 last_applied 防重拦截 (不触发倒退重置),
    // seq=7 > last_applied 放行
    {
        DzTradingAccount ta{};
        dztrader::copy_string(ta.account_id, "CTP001", true);
        ta.seq = 6;
        emit_struct(DZ_FRAME_TRADING_ACCOUNT, ta);
    }
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
    {
        DzTradingAccount ta{};
        dztrader::copy_string(ta.account_id, "CTP001", true);
        ta.seq = 7;
        emit_struct(DZ_FRAME_TRADING_ACCOUNT, ta);
    }
    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_TRADING_ACCOUNT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(7u, FrameView(static_cast<const std::byte*>(f)).payload<DzTradingAccount>().seq);
}

// 终检发现 B【Critical】回补交付序: 重试窗口期间到达的后续帧 (seq > 触发帧) 必须
// 拦截暂存, 覆盖成功后按 seq 序派发 — 回补行(旧) → 触发帧 → 后续帧 (新), 同键
// 状态不回退 (旧代码: 后续帧直接放行 → 旧状态覆盖新状态且无再修复)。
// 场景: W=5, gap [6,7] 未提交; 触发帧 seq=8 (order 100 PART_TRADED) 拦截暂存;
// 期间 seq=9 后续帧 (order 100 FILLED) 到达 → 也暂存; 行 6,7 提交后重试覆盖 →
// 派发序 = 触发帧(8) → 回补(6,7) → 暂存帧(9); 同键 order 100 终态 = FILLED 不回退。
TEST_F(IngestWiringTest, GapRetryStagesSubsequentFramesThenDeliversInSeqOrder) {
    // 构造 gap 区间行 6,7: seq6 = order 101 (他键), seq7 = order 100 旧状态 PART_TRADED
    // (回补后到达, 与触发/后续帧同键 order 100 — 若不暂存后续帧, seq7 旧状态会晚于
    // seq9 新状态到达 → 终态回退)。
    auto insert_gap_rows = [this]() {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " ?, ?, ?, ?, ?)");
        const char* own = dz_strategy_id(ctx_);
        // seq 6: order 101 (PART_TRADED)
        ins.bind(1, static_cast<int64_t>(101));
        ins.bind(2, 3900.0);
        ins.bind(3, static_cast<int64_t>(2));
        ins.bind(4, static_cast<int64_t>(2));
        ins.bind(5, own);
        ins.bind(6, static_cast<int64_t>(6));
        ins.exec();
        ins.reset();
        // seq 7: order 100 (PART_TRADED 旧状态)
        ins.bind(1, static_cast<int64_t>(100));
        ins.bind(2, 3901.0);
        ins.bind(3, static_cast<int64_t>(3));
        ins.bind(4, static_cast<int64_t>(3));
        ins.bind(5, own);
        ins.bind(6, static_cast<int64_t>(7));
        ins.exec();
    };

    // 首次回补必须不足 → 先不插 6,7 (gap 起始时行未提交)
    DzOrderReport trigger{};
    dztrader::copy_string(trigger.account_id, "CTP001", true);
    dztrader::copy_string(trigger.strategy_id, dz_strategy_id(ctx_), true);
    trigger.order_id = 100;
    trigger.status = DZ_ORDER_PART_TRADED;
    trigger.volume = 5;
    trigger.volume_traded = 3;
    trigger.seq = 8;
    emit_struct(DZ_FRAME_ORDER_REPORT, trigger);

    // 首次调用: 回补不足, 触发帧拦截 (宁缺勿乱)
    EXPECT_EQ(nullptr, dz_next_event(ctx_));

    // 重试在途: 后续帧 seq=9 (order 100 FILLED, 同键) 到达 → 必须暂存 (不直接放行)
    DzOrderReport later{};
    dztrader::copy_string(later.account_id, "CTP001", true);
    dztrader::copy_string(later.strategy_id, dz_strategy_id(ctx_), true);
    later.order_id = 100;
    later.status = DZ_ORDER_ALL_TRADED;
    later.volume = 5;
    later.volume_traded = 5;
    later.seq = 9;
    emit_struct(DZ_FRAME_ORDER_REPORT, later);
    // 后续帧被拦截暂存: 不得先于回补帧返回 (旧代码此处放行 seq=9)
    EXPECT_EQ(nullptr, dz_next_event(ctx_));

    // 模拟 persist 提交 gap 行 6,7
    insert_gap_rows();

    // 重试覆盖: 按 seq 序派发 — 回补行(6,7) → 触发帧(8) → 暂存帧(9) (终检发现 B:
    // 触发帧/后续帧经 replay 缓冲, 在回补行之后; 旧代码"触发帧先返回"已被替换)。
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(6u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);
    EXPECT_EQ(101, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().order_id);

    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(7u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);
    EXPECT_EQ(100, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().order_id);

    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(8u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);

    // 暂存帧 seq=9 最后派发 → 同键 order 100 终态 = FILLED (不回退)
    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(9u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);
    EXPECT_EQ(DZ_ORDER_ALL_TRADED,
              FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().status);

    EXPECT_EQ(nullptr, dz_next_event(ctx_));
}

// 终检发现 B 回归: 重试期间到达的 DZ_FRAME_POSITION_INFO/DZ_FRAME_TRADING_ACCOUNT 绝对态帧同样暂存 (次序无关紧要但统一处理),
// 不直接放行。
TEST_F(IngestWiringTest, GapRetryStagesPositionFrameDuringRetry) {
    // gap 6,7 无行 (首次回补不足)
    DzPositionInfo trigger{};
    dztrader::copy_string(trigger.account_id, "CTP001", true);
    trigger.seq = 8;
    emit_struct(DZ_FRAME_POSITION_INFO, trigger);
    EXPECT_EQ(nullptr, dz_next_event(ctx_));  // 触发帧拦截

    // 重试在途: DZ_FRAME_POSITION_INFO 后续帧 seq=9 → 暂存 (不直接放行)
    DzPositionInfo later{};
    dztrader::copy_string(later.account_id, "CTP001", true);
    later.seq = 9;
    emit_struct(DZ_FRAME_POSITION_INFO, later);
    EXPECT_EQ(nullptr, dz_next_event(ctx_));  // 暂存拦截

    // 覆盖: 行 6,7 提交 → 触发帧(8) → 回补(6,7) → 暂存(9)
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        SQLite::Statement pins(db,
            "INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id,"
            " direction, volume, frozen_volume, today_volume, yd_volume, price, seq)"
            " VALUES ('CTP001', '20260901', 'IF2603', 'CFFEX', '1', 3, 1, 3, 2, 3950.5, 6)");
        pins.exec();
        SQLite::Statement pins2(db,
            "INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id,"
            " direction, volume, frozen_volume, today_volume, yd_volume, price, seq)"
            " VALUES ('CTP001', '20260901', 'IF2606', 'CFFEX', '1', 5, 0, 5, 0, 4000.0, 7)");
        pins2.exec();
    }
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_POSITION_INFO, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(6u, FrameView(static_cast<const std::byte*>(f)).payload<DzPositionInfo>().seq);
    // 回补 7
    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(7u, FrameView(static_cast<const std::byte*>(f)).payload<DzPositionInfo>().seq);
    // 触发帧 8 (回补行之后)
    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_POSITION_INFO, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(8u, FrameView(static_cast<const std::byte*>(f)).payload<DzPositionInfo>().seq);
    // 暂存 DZ_FRAME_POSITION_INFO 帧 seq=9
    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_POSITION_INFO, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(9u, FrameView(static_cast<const std::byte*>(f)).payload<DzPositionInfo>().seq);
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
}

// 终检发现 D【Important】: 2018 Offline→Ready 翻转触发该账户 gate 重置 —
// td 重启复用 seq (PositionRebuild 删行压低 DB MAX) 时, 复用 seq 帧不再被旧
// last_applied 拦截 (自愈, 与 dzweb 2018 重建对齐)。
TEST_F(IngestWiringTest, AccountStatusFlipToReadyResetsGateForReusedSeq) {
    auto day_dz = [](int y, int m, int d) {
        return dztrader::Date::from_year_month_day(y, m, d).days_since_epoch();
    };
    // 建立在线基准: CTP001 已应用 seq=6,7 (last_applied=7, W=5)
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = 6;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    ASSERT_NE(nullptr, dz_next_event(ctx_));
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = 7;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    ASSERT_NE(nullptr, dz_next_event(ctx_));

    // Offline (td 断连/重启) → Ready (重登收尾完成): 触发 gate 重置
    {
        DzAccountStatus offline{};
        dztrader::copy_string(offline.account_id, "CTP001", true);
        offline.state = DZ_ACCOUNT_OFFLINE;
        offline.trading_day = 0;
        emit_struct(DZ_FRAME_ACCOUNT_STATUS, offline);
    }
    ASSERT_NE(nullptr, dz_next_event(ctx_));  // 2018 全量放行

    // 重置后 DB 新水位 = 2 (PositionRebuild 删行压低 MAX): 设库行 seq 1,2
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        db.exec("DELETE FROM orders");
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " ?, ?, ?, 'own', ?)");
        for (int64_t seq = 1; seq <= 2; ++seq) {
            ins.bind(1, static_cast<int64_t>(8000 + seq));
            ins.bind(2, 4800.0 + static_cast<double>(seq));
            ins.bind(3, static_cast<int64_t>(1));
            ins.bind(4, static_cast<int64_t>(1));
            ins.bind(5, static_cast<int64_t>(seq));
            ins.exec();
            ins.reset();
        }
    }

    // Ready: Offline→Ready 翻转 → reset_account(rebuild_watermark=2), last_applied 复位
    {
        DzAccountStatus ready{};
        dztrader::copy_string(ready.account_id, "CTP001", true);
        ready.state = DZ_ACCOUNT_READY;
        ready.trading_day = day_dz(2026, 9, 1);
        emit_struct(DZ_FRAME_ACCOUNT_STATUS, ready);
    }
    ASSERT_NE(nullptr, dz_next_event(ctx_));  // 2018 全量放行

    // td 复用 seq=3 的新帧 (seq=3 ≤ 旧 last_applied=7, 但 > 新 W=2): 必须放行
    // (无修复时被旧 last_applied 拦截 → 新事件丢失, 策略无自愈)
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.order_id = 900;
        rpt.status = DZ_ORDER_ALL_TRADED;
        rpt.volume = 2;
        rpt.volume_traded = 2;
        rpt.seq = 3;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(3u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);
}

// 终检发现 D: 2018 Ready (非翻转, 首见即 Ready) 不得误触 gate 重置 — 否则启动
// 首次 Ready 就把已装载水位清掉, 破坏 W 过滤。首见状态视为未知 (不重置)。
TEST_F(IngestWiringTest, FirstReadyDoesNotResetGate) {
    // 直接 2018 Ready (无前序 Offline): 不得触发 reset_account
    {
        DzAccountStatus ready{};
        dztrader::copy_string(ready.account_id, "CTP001", true);
        ready.state = DZ_ACCOUNT_READY;
        ready.trading_day =
            dztrader::Date::from_year_month_day(2026, 9, 1).days_since_epoch();
        emit_struct(DZ_FRAME_ACCOUNT_STATUS, ready);
    }
    ASSERT_NE(nullptr, dz_next_event(ctx_));

    // W=5 快照帧 seq=5 仍被过滤 (若误重置为 W=2 会把 seq=5 准入 → 镜像/回调重复)
    DzOrderReport rpt{};
    dztrader::copy_string(rpt.account_id, "CTP001", true);
    dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
    rpt.seq = 5;
    emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    EXPECT_EQ(nullptr, dz_next_event(ctx_));  // 仍按 W=5 过滤
}

// 终检发现 E 回归 (第二轮复审 open item 1): gap_covered 绝对态容差 —
// PositionRebuild 删行 (positions 全平) 后, 被删行 seq 在 DB 无行 → 行数 < 区间宽,
// 旧判定误报"未覆盖" → 10 次重试耗尽放行且不回补现存行 (orders/trades 追加流缺条)。
// 修复 (契约 §74): 该账户当前 MAX(seq) ≥ gap 上界即视为覆盖, 缺的中间 seq 是被删的
// 历史行, 当前态已到位 → 覆盖成功, 现存回补行 (orders 6,8) 送达 + 触发帧放行。
TEST_F(IngestWiringTest, GapCoverageToleratesDeletedPositionRow) {
    // W=5 (SetUp 预置 orders seq 1..5)。gap 区间 [6,8]:
    //   orders seq 6,8 (追加流, 现存) + positions seq 7 (绝对态, 随后 PositionRebuild
    //   全平删行 — 模拟先建仓后全平: positions 表 DELETE, 该 seq 在 DB 无行)。
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        SQLite::Statement o1(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " ?, ?, ?, ?, ?)");
        o1.bind(1, static_cast<int64_t>(6006));
        o1.bind(2, 4606.0);
        o1.bind(3, static_cast<int64_t>(2));
        o1.bind(4, static_cast<int64_t>(2));
        o1.bind(5, dz_strategy_id(ctx_));
        o1.bind(6, static_cast<int64_t>(6));
        o1.exec();
        o1.reset();
        o1.bind(1, static_cast<int64_t>(6008));
        o1.bind(2, 4608.0);
        o1.bind(3, static_cast<int64_t>(2));
        o1.bind(4, static_cast<int64_t>(2));
        o1.bind(5, dz_strategy_id(ctx_));
        o1.bind(6, static_cast<int64_t>(8));
        o1.exec();
        // positions seq 7 存在 → 模拟 PositionRebuild 全平删行 (INSERT 后 DELETE)。
        SQLite::Statement pin(db,
            "INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id,"
            " direction, volume, frozen_volume, today_volume, yd_volume, price, seq)"
            " VALUES ('CTP001', '20260901', 'IF2603', 'CFFEX', '1', 3, 0, 3, 0, 3950.0, 7)");
        pin.exec();
        db.exec("DELETE FROM positions WHERE account_id='CTP001'");
    }

    // 首帧 seq=9 (> W+1=6): gap [6,8] span=3。现存行 = orders 6,8 = 2 < 3。
    // 旧判定: 2 < 3 → 未覆盖 → 重试 10 次耗尽 → 触发帧放行 + orders 6,8 不回补 (缺条)。
    // 新判定: 该账户 MAX(seq)=8 ≥ 上界 8 → 覆盖 → orders 6,8 回补 + 触发帧 9 放行。
    DzOrderReport trigger{};
    dztrader::copy_string(trigger.account_id, "CTP001", true);
    dztrader::copy_string(trigger.strategy_id, dz_strategy_id(ctx_), true);
    trigger.order_id = 900;
    trigger.status = DZ_ORDER_ALL_TRADED;
    trigger.seq = 9;
    emit_struct(DZ_FRAME_ORDER_REPORT, trigger);

    // 覆盖成功 (首查即覆盖, handle_gap 放行路径): 触发帧先返回, 回补帧随后 (同
    // GapTriggersBackfillReplay 语义)。不耗尽不回补现存行 (缺条修复)。
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(9u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);

    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(6u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);

    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(8u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);

    EXPECT_EQ(nullptr, dz_next_event(ctx_));
}

}  // namespace
