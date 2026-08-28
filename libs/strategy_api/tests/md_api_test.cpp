#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/env.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::FrameView;
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

// 与 notify_ui_test / trade_api_test 同款 fixture: 临时 DZTRADER_HOME +
// 预建事件/md 通道 + dz_init。测试进程用独立 writer 模拟 master 写
// NOTIFY_MD_STARTED 广播帧, 再经 dz_on_md_started 触发 SDK 补订阅。
class MdApiTest : public ::testing::Test {
protected:
    std::string home_;
    std::filesystem::path shm_dir_;
    DzContext* ctx_ = nullptr;

    void SetUp() override {
        home_ = (std::filesystem::temp_directory_path() / "dz_test_strategy_md").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        shm_dir_ = home_ + "/shm";
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
        create_event_channel(shm_dir_);
        create_md_channel(shm_dir_);
        ctx_ = dz_init();
        ASSERT_NE(ctx_, nullptr) << "dz_init failed: " << dz_errmsg();
    }

    void TearDown() override {
        dz_release(ctx_);
        std::filesystem::remove_all(home_);
    }

    /// 用独立 writer 模拟 master 写含 instance_id 的广播帧。
    /// 必须在 dz_init 之后调用 (策略事件 reader 定位在 init 时写位置)。
    void emit_inst_frame(DzFrameType type, const std::string& instance) {
        auto meta = ChannelMeta::open_only(
            dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT), shm_dir_);
        MultiWriter writer = MultiWriter::create(
            std::make_shared<ChannelMeta>(std::move(meta)), "md_test_writer");
        ASSERT_TRUE(writer.write_ext_inst_frame(type, instance.c_str(), nullptr, 0));
        writer.notify_subscribers();
    }

    /// 模拟 master 写 NOTIFY_MD_STARTED 广播帧。
    void emit_md_started(const std::string& source) {
        emit_inst_frame(DZ_FRAME_NOTIFY_MD_STARTED, source);
    }

    /// 从事件通道读取下一帧, 返回解析后的 SubscribeReq (仅当帧为订阅类)。
    /// 非订阅帧则跳过, 返回 nullopt。
    std::optional<dztrader::SubscribeReq> read_next_subscribe() {
        const void* frame = dz_next_event(ctx_);
        if (frame == nullptr) {
            return std::nullopt;
        }
        const auto view = FrameView(static_cast<const std::byte*>(frame));
        if (view.type() != DZ_FRAME_REQUEST_MD_SUBSCRIBE) {
            return std::nullopt;
        }
        return dztrader::shm::decode_ext_inst_json<dztrader::SubscribeReq>(view);
    }

    /// 排干事件通道, 跳过全部非订阅帧。
    void drain_non_subscribe() {
        while (dz_next_event(ctx_) != nullptr) {
        }
    }
};

// 专测 dz_init 环境依赖: 不预建通道、不调用 dz_init (否则与已有会话冲突)。
// 仅验证 DZTRADER_MD_SOURCE 缺失/为空时的错误码。
// 注意: paths::shm() 按 DZTRADER_HOME 进程级缓存, 与本文件 MdApiTest 共享同一 home。
class MdApiInitEnvTest : public ::testing::Test {
protected:
    void SetUp() override {
        home_ = (std::filesystem::temp_directory_path() / "dz_test_strategy_md").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
    }

    void TearDown() override { std::filesystem::remove_all(home_); }

    std::string home_;
};

TEST_F(MdApiInitEnvTest, InitWithoutMdSourceFailsWithNotConfigured) {
    dztrader::env::unset("DZTRADER_MD_SOURCE");
    EXPECT_EQ(dz_init(), nullptr);
    EXPECT_EQ(dz_errcode(), DZ_EC_MD_SOURCE_NOT_CONFIGURED);
}

TEST_F(MdApiInitEnvTest, InitWithEmptyMdSourceFailsWithNotConfigured) {
    dztrader::env::set("DZTRADER_MD_SOURCE", "");
    EXPECT_EQ(dz_init(), nullptr);
    EXPECT_EQ(dz_errcode(), DZ_EC_MD_SOURCE_NOT_CONFIGURED);
}

// ── dz_subscribe / dz_unsubscribe 事务语义与期望集合 ──

TEST_F(MdApiTest, SubscribeReplaceTrueWritesFullCandidate) {
    const char* const insts[] = {"IF2506", "IC2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 2, true));
    const auto req = read_next_subscribe();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->action, dztrader::SubscribeAction::Subscribe);
    EXPECT_EQ(req->replace, true);
    EXPECT_EQ(req->instruments, std::vector<std::string>({"IC2506", "IF2506"}));
}

TEST_F(MdApiTest, SubscribeReplaceFalseUnionsWithExisting) {
    const char* const first[] = {"IF2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, first, 1, true));
    drain_non_subscribe();
    const char* const second[] = {"IC2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, second, 1, false));
    const auto req = read_next_subscribe();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->replace, false);
    EXPECT_EQ(req->instruments, std::vector<std::string>({"IC2506", "IF2506"}));
}

TEST_F(MdApiTest, SubscribeDedupsInstruments) {
    const char* const insts[] = {"IF2506", "IF2506", "IC2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 3, true));
    const auto req = read_next_subscribe();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->instruments, std::vector<std::string>({"IC2506", "IF2506"}));
}

TEST_F(MdApiTest, SubscribeNullInstrumentsRejected) {
    EXPECT_FALSE(dz_subscribe(ctx_, nullptr, 1, true));
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
}

TEST_F(MdApiTest, UnsubscribeAllClearsDesiredSet) {
    const char* const insts[] = {"IF2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 1, true));
    drain_non_subscribe();
    ASSERT_TRUE(dz_unsubscribe(ctx_, nullptr, 0));
    const auto req = read_next_subscribe();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->action, dztrader::SubscribeAction::UnsubscribeAll);
}

TEST_F(MdApiTest, UnsubscribeListRemovesOnlyListed) {
    const char* const insts[] = {"IF2506", "IC2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 2, true));
    drain_non_subscribe();
    const char* const drop[] = {"IF2506"};
    ASSERT_TRUE(dz_unsubscribe(ctx_, drop, 1));
    const auto req = read_next_subscribe();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->action, dztrader::SubscribeAction::Unsubscribe);
    EXPECT_EQ(req->instruments, std::vector<std::string>({"IF2506"}));
}

// ── dz_on_md_started ──

TEST_F(MdApiTest, OnMdStartedNullFrameReturnsFalse) {
    EXPECT_FALSE(dz_on_md_started(ctx_, nullptr));
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
}

TEST_F(MdApiTest, OnMdStartedWrongFrameTypeReturnsFalse) {
    // 非 STARTED 帧 (用 TD 订单请求帧类型, 模拟其他业务帧)
    emit_inst_frame(DZ_FRAME_TD_ORDER_REQ, "test_md");
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_FALSE(dz_on_md_started(ctx_, frame));
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
}

TEST_F(MdApiTest, OnMdStartedOtherSourceIgnoredNoResubscribe) {
    const char* const insts[] = {"IF2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 1, true));
    drain_non_subscribe();

    emit_md_started("other_md");
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_TRUE(dz_on_md_started(ctx_, frame));

    // 非本策略源: 不补订阅, 无订阅帧产出
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
}

TEST_F(MdApiTest, OnMdStartedOwnSourceResubscribesDesiredSet) {
    const char* const insts[] = {"IF2506", "IC2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 2, true));
    drain_non_subscribe();

    emit_md_started("test_md");
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_TRUE(dz_on_md_started(ctx_, frame));

    // 本策略源: 全量补订阅, replace=true
    const auto req = read_next_subscribe();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->action, dztrader::SubscribeAction::Subscribe);
    EXPECT_EQ(req->replace, true);
    EXPECT_EQ(req->instruments, std::vector<std::string>({"IC2506", "IF2506"}));
}

TEST_F(MdApiTest, OnMdStartedOwnSourceEmptyDesiredSetNoOp) {
    emit_md_started("test_md");
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_TRUE(dz_on_md_started(ctx_, frame));
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
}

// ── dz_next_md ──

TEST_F(MdApiTest, NextMdNullWhenNoData) {
    EXPECT_EQ(dz_next_md(ctx_), nullptr);
}

}  // namespace
