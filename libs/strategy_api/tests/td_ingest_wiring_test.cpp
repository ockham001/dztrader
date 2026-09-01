#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/string_util.h>
#include <dztrader/date_time/date.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/writer.h>
#include <dztrader/struct.h>

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

// orders 建表 SQL: 与 apps/ctp/td/td_schema.cpp v2 迁移后最终形态逐字一致 (含 seq 列)。
constexpr const char* kCreateOrders =
    "CREATE TABLE IF NOT EXISTS orders ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    account_id TEXT NOT NULL,"
    "    trading_day TEXT NOT NULL,"
    "    order_id INTEGER NOT NULL,"
    "    order_ref TEXT NOT NULL,"
    "    external_order_id TEXT,"
    "    is_external INTEGER NOT NULL DEFAULT 0,"
    "    instrument_id TEXT NOT NULL,"
    "    exchange_id TEXT NOT NULL,"
    "    direction CHAR(1),"
    "    position_effect CHAR(1),"
    "    price_type CHAR(1),"
    "    status CHAR(1),"
    "    price REAL,"
    "    volume INTEGER,"
    "    volume_traded INTEGER,"
    "    volume_canceled INTEGER,"
    "    insert_time INTEGER,"
    "    update_time INTEGER,"
    "    error_id INTEGER,"
    "    error_msg TEXT,"
    "    strategy_id TEXT,"
    "    remark TEXT,"
    "    seq INTEGER NOT NULL DEFAULT 0,"
    "    UNIQUE(account_id, order_id))";

// trades 建表 SQL: 与 apps/ctp/td/td_schema.cpp v2 迁移后最终形态逐字一致 (含 seq 列)。
constexpr const char* kCreateTrades =
    "CREATE TABLE IF NOT EXISTS trades ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    account_id TEXT NOT NULL,"
    "    trading_day TEXT NOT NULL,"
    "    trade_id TEXT NOT NULL,"
    "    order_id INTEGER NOT NULL,"
    "    instrument_id TEXT NOT NULL,"
    "    exchange_id TEXT NOT NULL,"
    "    direction CHAR(1),"
    "    position_effect CHAR(1),"
    "    price REAL NOT NULL,"
    "    volume INTEGER NOT NULL,"
    "    trade_time INTEGER,"
    "    trade_date INTEGER,"
    "    commission REAL,"
    "    strategy_id TEXT,"
    "    seq INTEGER NOT NULL DEFAULT 0,"
    "    UNIQUE(account_id, trading_day, trade_id))";

constexpr const char* kCreateTradingAccounts =
    "CREATE TABLE IF NOT EXISTS trading_accounts ("
    "    account_id TEXT NOT NULL PRIMARY KEY,"
    "    trading_day TEXT NOT NULL,"
    "    balance REAL, available REAL, frozen REAL,"
    "    commission REAL, margin REAL, withdraw_quota REAL,"
    "    deposit REAL, withdraw REAL,"
    "    seq INTEGER NOT NULL DEFAULT 0)";

/// SDK ingest 接线端到端: 临时 DZTRADER_HOME + 预置 td 库 (flow/dztd_ctp/dztd_ctp.db)。
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
        std::filesystem::create_directories(home_ + "/flow/dztd_ctp");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
        create_event_channel(home_ + "/shm");
        create_md_channel(home_ + "/shm");

        db_path_ = std::filesystem::path(home_) / "flow" / "dztd_ctp" / "dztd_ctp.db";
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec(kCreateOrders);
        db.exec(kCreateTrades);
        db.exec(kCreateTradingAccounts);
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
                ins.bind(2, 3800.0 + seq);
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
            ins.bind(2, 3900.0 + seq);
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
    // 2018 仍全量放行给策略用户 (on_account_status 回调), 故返回该帧。
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
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    const DzOrderReport& o6 = FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>();
    EXPECT_EQ(6u, o6.seq);
    EXPECT_EQ(9 * 3600 + 15 * 60 + 5, o6.time);  // insert_time 当日秒

    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    const DzOrderReport& o7 = FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>();
    EXPECT_EQ(7u, o7.seq);
    EXPECT_EQ(11 * 3600 + 30 * 60 + 45, o7.time);  // update_time 当日秒

    // trades 回补
    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_TRADE_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    const DzTradeReport& t1 = FrameView(static_cast<const std::byte*>(f)).payload<DzTradeReport>();
    EXPECT_EQ(6u, t1.seq);
    EXPECT_EQ(10 * 3600 + 1 * 60 + 2, t1.time);  // trade_time 当日秒

    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    const DzTradeReport& t2 = FrameView(static_cast<const std::byte*>(f)).payload<DzTradeReport>();
    EXPECT_EQ(7u, t2.seq);
    EXPECT_EQ(12 * 3600 + 3 * 60 + 4, t2.time);

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
            ins.bind(2, 4300.0 + seq);
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
    }
    DzOrderReport rpt{};
    dztrader::copy_string(rpt.account_id, "CTP001", true);
    dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
    rpt.seq = 3;  // < last_applied=7 -> detect_reset 触发
    emit_struct(DZ_FRAME_ORDER_REPORT, rpt);

    // rebuild_watermark: DB 清空 -> W=0; reset_account(CTP001, 0). 新帧 seq=3 > W=0 应用
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(3u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);
}

// 评审发现 4: 逐表容错 — trades 表缺失时, orders/positions 仍回补 (不整体失败)。
TEST_F(IngestWiringTest, BackfillToleratesMissingTable) {
    // 仅预置 orders 行 (trades/positions/trading_accounts 表未建)
    {
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
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
    trigger.seq = 8;  // gap 6,7
    emit_struct(DZ_FRAME_ORDER_REPORT, trigger);

    // 触发帧返回
    ASSERT_NE(nullptr, dz_next_event(ctx_));
    // orders 回补 seq=6 正常送达 (trades/positions 表缺失被跳过)
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(6u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
}

// 评审发现 4: 无水位降级 (DB 缺失全放行) — 见 td_ingest_nodb_test.cpp (需独立进程:
// paths::home() 按进程缓存 + context_registry 进程全局, 无法与本 fixture 共存)。

// 评审发现 4: 2002/2003 帧 (POSITION_INFO / TRADING_ACCOUNT) 过 gate — W 过滤拦截
// seq ≤ W 的帧, seq > W 放行 (全量放行语义, 不经 strategy_id 定向)。
TEST_F(IngestWiringTest, PositionAndTradingAccountGoThroughGate) {
    // seq ≤ W=5: 拦截
    {
        DzPositionInfo p{};
        dztrader::copy_string(p.account_id, "CTP001", true);
        p.seq = 5;
        emit_struct(DZ_FRAME_POSITION_INFO, p);
    }
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
    // seq > W: 放行
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

    // TRADING_ACCOUNT 同语义
    {
        DzTradingAccount ta{};
        dztrader::copy_string(ta.account_id, "CTP001", true);
        ta.seq = 5;
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

}  // namespace
