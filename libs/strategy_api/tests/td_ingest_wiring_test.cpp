#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/string_util.h>
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

}  // namespace
