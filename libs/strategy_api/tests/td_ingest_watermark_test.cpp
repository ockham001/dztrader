// 终检发现 4(b): SDK 四表 W 装载 — load_all_watermarks 对 positions/trading_accounts
// 的 MAX(seq) 参与该账户 W 计算 (四表共享计数器, W 取大)。
// 独立二进制 (独立进程): paths::home() 按进程缓存, fixture 需要在 dz_init 前
// 预置多表 seq, 无法与 td_ingest_wiring_test 共享 home 的进程共存。

#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/string_util.h>
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

class IngestWatermarkTest : public ::testing::Test {
protected:
    std::string home_;
    std::filesystem::path db_path_;
    DzContext* ctx_ = nullptr;

    void SetUp() override {
        home_ = (std::filesystem::temp_directory_path() / "dz_test_strategy_ingest_wm")
                    .string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        std::filesystem::create_directories(home_ + "/db");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
        ChannelConfig evt{
            .channel_name = dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
            .shm_dir = home_ + "/shm",
            .meta_file_size = 1 * kMB,
            .page_size = 1 * kMB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        (void)ChannelMeta::open_or_create(evt);
        ChannelConfig md{
            .channel_name = "test_md",
            .shm_dir = home_ + "/shm",
            .meta_file_size = 1 * kMB,
            .page_size = 1 * kMB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        (void)ChannelMeta::open_or_create(md);

        db_path_ = std::filesystem::path(home_) / "db" / "td.db";
        {
            auto database = dztrader::db::Database::open(
                dztrader::db::Config{.backend = "sqlite",
                                     .options = {{"path", db_path_.string()}}},
                dztrader::tdstore::schemas());
            database->migrate();
        }
        SQLite::Database db(db_path_.string(), SQLite::OPEN_READWRITE);
        // 四表预置 (账户 CTP001): orders 最大 seq=5, trades=7, positions=9, trading_accounts=11
        // → W 必须取四表最大 = 11 (非任何单表的 5/7/9)。
        SQLite::Statement o(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " ?, ?, ?, 'own', ?)");
        for (int64_t seq = 1; seq <= 5; ++seq) {
            o.bind(1, static_cast<int64_t>(1000 + seq));
            o.bind(2, 3800.0);
            o.bind(3, static_cast<int64_t>(1));
            o.bind(4, static_cast<int64_t>(seq));
            o.bind(5, static_cast<int64_t>(seq));
            o.exec();
            o.reset();
        }
        SQLite::Statement t(db,
            "INSERT INTO trades (account_id, trading_day, trade_id, order_id, instrument_id,"
            " exchange_id, direction, position_effect, price, volume, trade_time, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', ?, ?, 'IF2603', 'CFFEX', '1', '1', ?, ?, ?, 'own', ?)");
        for (int64_t seq = 6; seq <= 7; ++seq) {
            t.bind(1, std::to_string(seq));
            t.bind(2, static_cast<int64_t>(1000 + seq));
            t.bind(3, 3900.0);
            t.bind(4, static_cast<int64_t>(1));
            t.bind(5, static_cast<int64_t>(0));
            t.bind(6, static_cast<int64_t>(seq));
            t.exec();
            t.reset();
        }
        // positions: seq 8, 9 (两个方向)
        SQLite::Statement p(db,
            "INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id,"
            " direction, volume, frozen_volume, today_volume, yd_volume, price, seq)"
            " VALUES ('CTP001', '20260901', 'IF2603', 'CFFEX', ?, 3, 0, 3, 0, 3950.0, ?)");
        p.bind(1, "1");
        p.bind(2, static_cast<int64_t>(8));
        p.exec();
        p.reset();
        p.bind(1, "2");
        p.bind(2, static_cast<int64_t>(9));
        p.exec();
        // trading_accounts: seq 11 (留 seq 10 空洞模拟取号交错)
        SQLite::Statement a(db,
            "INSERT INTO trading_accounts (account_id, trading_day, balance, available, frozen,"
            " commission, margin, withdraw_quota, deposit, withdraw, seq)"
            " VALUES ('CTP001', '20260901', 100000.0, 90000.0, 1000.0, 0, 9000, 0, 0, 0, 11)");
        a.exec();
    }

    void TearDown() override {
        dz_release(ctx_);
        std::filesystem::remove_all(home_);
    }

    void init_ctx() {
        ctx_ = dz_init();  // 装载水位: CTP001 -> W = 11 (四表取大)
        ASSERT_NE(nullptr, ctx_) << "dz_init failed: " << dz_errmsg();
    }

    std::shared_ptr<ChannelMeta> open_event_meta() {
        return std::make_shared<ChannelMeta>(ChannelMeta::open_only(
            dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT), home_ + "/shm"));
    }

    template <typename T>
    void emit_struct(DzFrameType type, const T& payload) {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "ingest_wm_writer");
        ASSERT_TRUE(writer.write_frame(type, payload));
        writer.notify_subscribers();
    }
};

// W = 四表最大 seq: seq ≤ DZ_FRAME_PRELOAD_EVENT_SHM 的帧被拦截 (含 orders 表快照外但 ≤ 其他表 max 的帧)。
TEST_F(IngestWatermarkTest, WatermarkTakesMaxAcrossFourTables) {
    init_ctx();
    // seq=6: > orders 表 max(5) 但 ≤ trades 表 max(7) → 快照已含, 必须拦截。
    // (若 W 只算 orders, seq=6 会被误放行。)
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = 6;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
    // seq=10: ≤ trading_accounts max(11) → 拦截
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = 10;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    EXPECT_EQ(nullptr, dz_next_event(ctx_));
    // seq=12: > W=11 → 放行
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "CTP001", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = 12;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(12u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);
}

// DZ_FRAME_POSITION_INFO (positions) 帧同样受四表 W 过滤: seq=9 (positions 表自身快照行) 拦截。
TEST_F(IngestWatermarkTest, PositionFrameFilteredByCrossTableWatermark) {
    init_ctx();
    DzPositionInfo p{};
    dztrader::copy_string(p.account_id, "CTP001", true);
    p.seq = 9;  // positions 表 max 自身
    emit_struct(DZ_FRAME_POSITION_INFO, p);
    EXPECT_EQ(nullptr, dz_next_event(ctx_));  // ≤ W=11 拦截
}

}  // namespace
