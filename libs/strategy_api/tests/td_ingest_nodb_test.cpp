#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/env.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/writer.h>
#include <dztrader/struct.h>
#include <dztrader/tdstore/trade_db.h>

#include <filesystem>
#include <string>

#include "db_database.h"
#include "test_temp_dir.h"

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::FrameView;
using dztrader::shm::MultiWriter;

namespace {

constexpr uint64_t kMB = 1024 * 1024;

/// 独立进程测试 (独立二进制): 无 td 库 (不建 db/trade.db) 的降级场景。
/// 与 td_ingest_wiring_test 分开: paths::home() 按进程缓存 + context_registry 进程全局,
/// 本 fixture 必须独占进程 (先设 DZTRADER_HOME 再 dz_init)。
class IngestNoDbTest : public ::testing::Test {
protected:
    std::string home_;
    DzContext* ctx_ = nullptr;

    void SetUp() override {
        home_ = dztrader::test::unique_temp_dir("dz_test_strategy_ingest_nodb").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");

        ChannelConfig cfg{
            .channel_name = dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
            .shm_dir = home_ + "/shm",
            .meta_file_size = 1 * kMB,
            .page_size = 1 * kMB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        (void)ChannelMeta::open_or_create(cfg);
        ChannelConfig mdcfg{
            // NOLINT(temp-dir-unique): test_md 是 SDK 约定的行情通道名，其 shm_dir 每用例唯一
            .channel_name = "test_md", .shm_dir = home_ + "/shm", .meta_file_size = 1 * kMB,
            .page_size = 1 * kMB,      .lock_memory = false,      .prefetch_memory = false,
        };
        (void)ChannelMeta::open_or_create(mdcfg);

        // 无 db/trade.db: 水位装载降级为不过滤全放行, dz_init 不失败。
        ctx_ = dz_init();
        ASSERT_NE(nullptr, ctx_) << "dz_init failed (no db): " << dz_errmsg();
    }

    void TearDown() override {
        dz_release(ctx_);
        dztrader::strategy_api_internal::reset_default_db_cache_for_test();
        std::filesystem::remove_all(home_);
    }

    std::shared_ptr<ChannelMeta> open_event_meta() {
        return std::make_shared<ChannelMeta>(ChannelMeta::open_only(
            dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT), home_ + "/shm"));
    }

    template <typename T>
    void emit_struct(DzFrameType type, const T& payload) {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "ingest_nodb_writer");
        ASSERT_TRUE(writer.write_frame(type, payload));
        writer.notify_subscribers();
    }
};

// AC (ADR 0044 决策 3): `dz_init` 不提前建立**默认查询库入口**。
// 计数器统计"真实打开（建 Session）的尝试次数"，与 ingest 缓存分开：
// init 只走 ingest 路径 → 必须为 0；无库首次调用尝试一次（失败，计 1）；
// 建库后每次成功调用各计一次（2、3）——因为每次调用都新建独立 Session。
// 这个断言会真的变红 —— 若 dz_init 提前调用 default_db_handle(), 首个 EXPECT_EQ 即失败
// （已实测：临时让 dz_init 调用 default_db_handle() 后本用例失败）。
TEST_F(IngestNoDbTest, InitDoesNotOpenDefaultDbEntryAndOpenIsLazy) {
    EXPECT_EQ(0u, dztrader::strategy_api_internal::default_db_open_attempts_for_test())
        << "dz_init must not open the default query db entry";

    // 无库时首次调用: 失败（不自动建库）, 且确实尝试打开过一次
    EXPECT_EQ(nullptr, dz_db_open_default());
    EXPECT_EQ(DZ_EC_DB_OPEN_FAILED, dz_errcode());
    EXPECT_EQ(1u, dztrader::strategy_api_internal::default_db_open_attempts_for_test());
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(home_) / "db" / "trade.db"));

    // 建库后再调用: 懒打开成功
    std::filesystem::create_directories(home_ + "/db");
    (void)dztrader::tdstore::ensure_trade_db(
        (std::filesystem::path(home_) / "db" / "trade.db").string());
    DzDatabase* db = dz_db_open_default();
    ASSERT_NE(nullptr, db) << dz_errmsg();
    EXPECT_EQ(2u, dztrader::strategy_api_internal::default_db_open_attempts_for_test());

    // 每次调用都新建独立 Session/句柄（一线程一 Session）, 计数随之递增
    DzDatabase* again = dz_db_open_default();
    ASSERT_NE(nullptr, again) << dz_errmsg();
    EXPECT_NE(db, again);
    EXPECT_EQ(3u, dztrader::strategy_api_internal::default_db_open_attempts_for_test());
    dz_db_close(db);
    dz_db_close(again);
}

// 评审发现 4: 无水位降级 (DB 缺失全放行) — 库不存在时 dz_init 不失败, ingest 全放行,
// 仅 last_applied 防重 (seq 相同重复帧拦截), 新 seq 正常放行。
TEST_F(IngestNoDbTest, NoWatermarkDegradesToPassThrough) {
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "NODB", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = 0;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    const void* f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(f)).type());
    EXPECT_EQ(0u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);

    // 同 seq 重复: last_applied 防重 (无水位也防重)
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "NODB", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = 0;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    EXPECT_EQ(nullptr, dz_next_event(ctx_));

    // 新 seq: 全放行
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "NODB", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = 1;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(1u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);

    // 多账户隔离: 他账户无快照也全放行
    {
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, "NODB2", true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = 5;
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    f = dz_next_event(ctx_);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(5u, FrameView(static_cast<const std::byte*>(f)).payload<DzOrderReport>().seq);
}

}  // namespace
