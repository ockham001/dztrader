#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/env.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

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

// 与 notify_ui_test 同款 fixture: 临时 DZTRADER_HOME + 预建事件/md 通道 + dz_init。
// SDK 写出的 TD_ORDER_REQ/TD_ORDER_CANCEL_REQ/SET_LOGICAL_POSITION 等帧在新语义下
// 被 dz_next_event 拦截 (平台内部帧), 故用独立 probe reader 观察写帧。
class TradeApiTest : public ::testing::Test {
protected:
    std::string home_;
    DzContext* ctx_ = nullptr;
    /// dz_init 后创建, 定位其后: 可观察 SDK 写出的全部帧 (Reader 无默认构造, 用 optional)
    std::optional<Reader> probe_;

    void SetUp() override {
        home_ = (std::filesystem::temp_directory_path() / "dz_test_strategy_trade").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
        create_event_channel(home_ + "/shm");
        create_md_channel(home_ + "/shm");
        ctx_ = dz_init();
        ASSERT_NE(ctx_, nullptr) << "dz_init failed: " << dz_errmsg();
        probe_.emplace(Reader::create(dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
                                      home_ + "/shm", "trade_test_probe"));
    }

    void TearDown() override {
        dz_release(ctx_);
        std::filesystem::remove_all(home_);
    }

    /// 从 probe reader 读取下一帧并断言为 basic struct 帧, 返回 payload 指针 (非拥有)
    const void* read_next_basic(DzFrameType type) {
        const std::byte* frame = probe_->next_frame();
        if (frame == nullptr) {
            return nullptr;
        }
        const auto view = dztrader::shm::FrameView(frame);
        EXPECT_EQ(view.type(), type);
        return frame + sizeof(DzFrameHeader);
    }
};

// ── 成功路径: payload 正确落帧 ──

TEST_F(TradeApiTest, PlaceOrderWritesOrderReq) {
    const DzOrderId id = dz_place_order(ctx_, "CTP001", "IF2401", DZ_DIRECTION_LONG, DZ_PRICE_LIMIT,
                                        1234.5, 2, DZ_POSITION_EFFECT_OPEN);
    ASSERT_GE(id, 0);
    const auto* req = static_cast<const DzOrderReq*>(read_next_basic(DZ_FRAME_TD_ORDER_REQ));
    ASSERT_NE(req, nullptr);
    EXPECT_EQ(req->order_id, id);
    EXPECT_STREQ(req->strategy_id, dz_strategy_id(ctx_));
    EXPECT_STREQ(req->account_id, "CTP001");
    EXPECT_STREQ(req->instrument_id, "IF2401");
    EXPECT_EQ(req->direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(req->price_type, DZ_PRICE_LIMIT);
    EXPECT_DOUBLE_EQ(req->price, 1234.5);
    EXPECT_EQ(req->volume, 2);
    EXPECT_EQ(req->position_effect, DZ_POSITION_EFFECT_OPEN);
}

TEST_F(TradeApiTest, CancelOrderWritesOrderCancelReq) {
    ASSERT_TRUE(dz_cancel_order(ctx_, "CTP001", 42));
    const auto* req =
        static_cast<const DzOrderCancelReq*>(read_next_basic(DZ_FRAME_TD_ORDER_CANCEL_REQ));
    ASSERT_NE(req, nullptr);
    EXPECT_EQ(req->order_id, 42);
    EXPECT_STREQ(req->account_id, "CTP001");
}

TEST_F(TradeApiTest, SetLogicalPositionWritesFrame) {
    ASSERT_TRUE(dz_set_logical_position(ctx_, "CTP001", "IF2401", 3));
    const auto* pos =
        static_cast<const DzLogicalPosition*>(read_next_basic(DZ_FRAME_SET_LOGICAL_POSITION));
    ASSERT_NE(pos, nullptr);
    EXPECT_STREQ(pos->account_id, "CTP001");
    EXPECT_STREQ(pos->instrument_id, "IF2401");
    EXPECT_STREQ(pos->strategy_id, dz_strategy_id(ctx_));
    EXPECT_EQ(pos->net_volume, 3);
}

// ── 生命周期边界 ──

TEST_F(TradeApiTest, SecondInitReturnsNullWithAlreadyInitialized) {
    // 失败的重复 init 不得破坏活动会话: 身份与下单能力应保持
    const char* const id_before = dz_strategy_id(ctx_);
    EXPECT_EQ(dz_init(), nullptr);
    EXPECT_EQ(dz_errcode(), DZ_EC_STRATEGY_ALREADY_INITIALIZED);
    EXPECT_STREQ(dz_strategy_id(ctx_), id_before);
    EXPECT_GE(dz_place_order(ctx_, "CTP001", "IF2401", DZ_DIRECTION_LONG, DZ_PRICE_LIMIT, 1, 1,
                             DZ_POSITION_EFFECT_OPEN),
              0);
}

TEST_F(TradeApiTest, ReleaseNullIsNoOp) {
    // release(NULL) no-op: 不得破坏当前会话的可用性
    const char* const id_before = dz_strategy_id(ctx_);
    dz_release(nullptr);
    EXPECT_STREQ(dz_strategy_id(ctx_), id_before);
    const DzOrderId order_id = dz_place_order(ctx_, "CTP001", "IF2401", DZ_DIRECTION_LONG,
                                              DZ_PRICE_LIMIT, 1, 1, DZ_POSITION_EFFECT_OPEN);
    EXPECT_GE(order_id, 0);
}

TEST_F(TradeApiTest, ReinitAfterReleaseSucceeds) {
    dz_release(ctx_);
    ctx_ = dz_init();
    ASSERT_NE(ctx_, nullptr);
    // 新句柄应功能正常
    const DzOrderId order_id = dz_place_order(ctx_, "CTP001", "IF2401", DZ_DIRECTION_LONG,
                                              DZ_PRICE_LIMIT, 1, 1, DZ_POSITION_EFFECT_OPEN);
    EXPECT_GE(order_id, 0);
}

// ── 句柄化会话函数覆盖（防签名回归） ──

TEST_F(TradeApiTest, StrategyHomeNonEmpty) {
    EXPECT_NE(dz_strategy_home(ctx_), nullptr);
    EXPECT_NE(dz_strategy_home(ctx_)[0], '\0');
}

TEST_F(TradeApiTest, NotifySelfWakesWait) {
    dz_notify_self(ctx_);  // 投递一次信号量
    dz_wait(ctx_);         // 已有计数应立即返回 (挂起即失败)
}

TEST_F(TradeApiTest, WaitReturnsAfterNotify) {
    std::thread notifier([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        dz_notify_self(ctx_);
    });
    dz_wait(ctx_);  // 阻塞至 notifier 投递信号量
    notifier.join();
}

}  // namespace
