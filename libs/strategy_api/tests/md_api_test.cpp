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
#include <optional>
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
// 预建事件/md 通道 + dz_init。测试进程用独立 writer 模拟 master/行情进程写广播帧,
// 用独立 probe reader 观察 SDK 写出的事件帧 (订阅请求等)。SDK 内部帧
// (NOTIFY_MD_STARTED / PRELOAD / 订阅回声等) 经 dz_next_event 自动消费,
// 不再返回给策略用户, 故断言走 probe 而非 dz_next_event。
class MdApiTest : public ::testing::Test {
protected:
    std::string home_;
    std::filesystem::path shm_dir_;
    DzContext* ctx_ = nullptr;
    /// dz_init 后创建, 定位其后: 可观察 SDK 写出的全部帧 (Reader 无默认构造, 用 optional)
    std::optional<Reader> probe_;

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
        probe_.emplace(Reader::create(dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
                                      shm_dir_, "md_test_probe"));
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
        MultiWriter writer =
            MultiWriter::create(std::make_shared<ChannelMeta>(std::move(meta)), "md_test_writer");
        ASSERT_TRUE(writer.write_ext_inst_frame(type, instance.c_str(), nullptr, 0));
        writer.notify_subscribers();
    }

    /// 模拟行情进程写 NOTIFY_MD_STARTED 广播帧。
    void emit_md_started(const std::string& source) {
        emit_inst_frame(DZ_FRAME_NOTIFY_MD_STARTED, source);
    }

    /// 模拟行情进程向 test_md 通道写一帧行情数据 (验证策略侧 md reader 仍打开)。
    void emit_md_tick() {
        auto meta = ChannelMeta::open_only("test_md", shm_dir_);
        MultiWriter writer = MultiWriter::create(std::make_shared<ChannelMeta>(std::move(meta)),
                                                 "md_test_md_writer");
        DzTick tick{};
        dztrader::copy_string(tick.instrument_id, "IF2506", true);
        ASSERT_TRUE(writer.write_frame(DZ_FRAME_TICK, tick));
        writer.notify_subscribers();
    }

    /// 订阅者身份 = stg.<裸策略名> (契约 md-subscription: 订阅者身份在 payload instance_id)。
    std::string expected_subscriber_id() const {
        return std::string(dztrader::STRATEGY_PREFIX) + "." + dz_strategy_id(ctx_);
    }

    /// 从 probe reader 读取下一帧, 返回解析后的 SubscribeReq (仅当帧为订阅类)。
    /// 非订阅帧则跳过, 返回 nullopt。帧头 instance_id 必须是本策略行情进程名
    /// (契约 md-subscription: 定向帧, instance_id = 行情进程名)。
    /// raw_payload 非空时回填 payload 原文, 供按契约字段 (数字枚举) 断言。
    std::optional<dztrader::SubscribeReq> read_next_subscribe(std::string* raw_payload = nullptr) {
        for (;;) {
            const auto* frame = probe_->next_frame();
            if (frame == nullptr) {
                return std::nullopt;
            }
            const auto view = FrameView(frame);
            if (view.type() != DZ_FRAME_REQUEST_MD_SUBSCRIBE) {
                continue;
            }
            EXPECT_EQ(std::string_view(view.ext_inst_id()), "test_md")
                << "subscribe frame target must be the md process";
            if (raw_payload != nullptr) {
                raw_payload->assign(reinterpret_cast<const char*>(view.ext_inst_payload()),
                                    view.ext_inst_payload_size());
            }
            return dztrader::shm::decode_ext_inst_json<dztrader::SubscribeReq>(view);
        }
    }

    /// 排干 probe reader 中已写出的帧 (跳过全部非订阅帧)。
    void drain_non_subscribe() {
        while (probe_->next_frame() != nullptr) {
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
    std::string raw;
    const auto req = read_next_subscribe(&raw);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->instance_id, expected_subscriber_id());
    EXPECT_EQ(req->action, dztrader::SubscribeAction::Subscribe);
    EXPECT_EQ(req->replace, true);
    EXPECT_EQ(req->instruments, std::vector<std::string>({"IC2506", "IF2506"}));
    // action 数字枚举按契约锚定 raw 值 (serialize 表 round-trip 是同义反复)
    const auto payload = nlohmann::json::parse(raw);
    EXPECT_EQ(payload.at("action").get<int>(), 0);
    EXPECT_EQ(payload.at("instance_id").get<std::string>(), expected_subscriber_id());
}

TEST_F(MdApiTest, SubscribeReplaceFalseUnionsWithExisting) {
    const char* const first[] = {"IF2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, first, 1, true));
    drain_non_subscribe();
    const char* const second[] = {"IC2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, second, 1, false));
    const auto req = read_next_subscribe();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->instance_id, expected_subscriber_id());
    EXPECT_EQ(req->replace, false);
    EXPECT_EQ(req->instruments, std::vector<std::string>({"IC2506", "IF2506"}));
}

TEST_F(MdApiTest, SubscribeDedupsInstruments) {
    const char* const insts[] = {"IF2506", "IF2506", "IC2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 3, true));
    const auto req = read_next_subscribe();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->instance_id, expected_subscriber_id());
    EXPECT_EQ(req->instruments, std::vector<std::string>({"IC2506", "IF2506"}));
}

TEST_F(MdApiTest, SubscribeNullInstrumentsRejected) {
    EXPECT_FALSE(dz_subscribe(ctx_, nullptr, 1, true));
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
}

TEST_F(MdApiTest, SubscribeZeroCountRejected) {
    // api.h: NULL 或 count=0 均为无效调用 (数组非空也必须拒绝)
    const char* const insts[] = {"IF2506"};
    EXPECT_FALSE(dz_subscribe(ctx_, insts, 0, true));
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
    EXPECT_EQ(read_next_subscribe(), std::nullopt);  // 拒绝时不得写帧
}

TEST_F(MdApiTest, SubscribeAllNullArrayRejected) {
    // 数组非 NULL 但元素全为 nullptr: 去重后无有效合约 -> 无效调用
    const char* const insts[] = {nullptr, nullptr};
    EXPECT_FALSE(dz_subscribe(ctx_, insts, 2, true));
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
    EXPECT_EQ(read_next_subscribe(), std::nullopt);
}

TEST_F(MdApiTest, UnsubscribeZeroCountMeansAll) {
    // api.h: dz_unsubscribe 的 NULL/count=0 表示退订全部 (与 dz_subscribe 的拒绝语义不同)
    const char* const insts[] = {"IF2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 1, true));
    drain_non_subscribe();
    ASSERT_TRUE(dz_unsubscribe(ctx_, insts, 0));
    std::string raw;
    const auto req = read_next_subscribe(&raw);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->instance_id, expected_subscriber_id());
    EXPECT_EQ(req->action, dztrader::SubscribeAction::UnsubscribeAll);
    EXPECT_EQ(nlohmann::json::parse(raw).at("action").get<int>(), 2);
}

TEST_F(MdApiTest, UnsubscribeAllNullArrayRejected) {
    const char* const insts[] = {nullptr, nullptr};
    EXPECT_FALSE(dz_unsubscribe(ctx_, insts, 2));
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
    EXPECT_EQ(read_next_subscribe(), std::nullopt);
}

TEST_F(MdApiTest, UnsubscribeAllClearsDesiredSet) {
    const char* const insts[] = {"IF2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 1, true));
    drain_non_subscribe();
    ASSERT_TRUE(dz_unsubscribe(ctx_, nullptr, 0));
    std::string raw;
    const auto req = read_next_subscribe(&raw);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->instance_id, expected_subscriber_id());
    EXPECT_EQ(req->action, dztrader::SubscribeAction::UnsubscribeAll);
    EXPECT_EQ(nlohmann::json::parse(raw).at("action").get<int>(), 2);

    // 期望集合已清空: md 重启补订阅不得再下发旧合约
    emit_md_started("test_md");
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
    EXPECT_EQ(read_next_subscribe(), std::nullopt);
}

TEST_F(MdApiTest, UnsubscribeListRemovesOnlyListed) {
    const char* const insts[] = {"IF2506", "IC2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 2, true));
    drain_non_subscribe();
    const char* const drop[] = {"IF2506"};
    ASSERT_TRUE(dz_unsubscribe(ctx_, drop, 1));
    std::string raw;
    const auto req = read_next_subscribe(&raw);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->instance_id, expected_subscriber_id());
    EXPECT_EQ(req->action, dztrader::SubscribeAction::Unsubscribe);
    EXPECT_EQ(req->instruments, std::vector<std::string>({"IF2506"}));
    EXPECT_EQ(nlohmann::json::parse(raw).at("action").get<int>(), 1);
}

// ── NOTIFY_MD_STARTED 自动补订阅 (dz_next_event 内部消费) ──

TEST_F(MdApiTest, NotifyMdStartedOtherSourceIgnoredNoResubscribe) {
    const char* const insts[] = {"IF2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 1, true));
    drain_non_subscribe();

    emit_md_started("other_md");
    // SDK 内部消费 STARTED 帧: 非本策略源, 不补订阅, 用户拿不到该帧
    EXPECT_EQ(dz_next_event(ctx_), nullptr);

    // 无订阅帧产出 (probe 流中只有 STARTED(other) 被跳过)
    EXPECT_EQ(read_next_subscribe(), std::nullopt);
}

TEST_F(MdApiTest, NotifyMdStartedOwnSourceResubscribesDesiredSet) {
    const char* const insts[] = {"IF2506", "IC2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 2, true));
    drain_non_subscribe();

    emit_md_started("test_md");
    // SDK 内部消费 STARTED 帧并自动补订阅; 用户拿不到该帧
    EXPECT_EQ(dz_next_event(ctx_), nullptr);

    // 本策略源: 全量补订阅, replace=true
    const auto req = read_next_subscribe();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->instance_id, expected_subscriber_id());
    EXPECT_EQ(req->action, dztrader::SubscribeAction::Subscribe);
    EXPECT_EQ(req->replace, true);
    EXPECT_EQ(req->instruments, std::vector<std::string>({"IC2506", "IF2506"}));
}

TEST_F(MdApiTest, NotifyMdStoppedInterceptedAndStartStillResubscribes) {
    // 契约 md-subscription: NOTIFY_MD_STOPPED 由策略 SDK 拦截忽略 (reader 保持打开),
    // 重启后 STARTED 仍按期望集合自动补订阅。
    const char* const insts[] = {"IF2506", "IC2506"};
    ASSERT_TRUE(dz_subscribe(ctx_, insts, 2, true));
    drain_non_subscribe();

    emit_inst_frame(DZ_FRAME_NOTIFY_MD_STOPPED, "test_md");
    EXPECT_EQ(dz_next_event(ctx_), nullptr);         // 用户拿不到停止通知
    EXPECT_EQ(read_next_subscribe(), std::nullopt);  // 停止通知本身不产生订阅帧

    // reader 保持打开: 停止后行情帧仍可由 dz_next_md 读到
    emit_md_tick();
    const void* md_frame = dz_next_md(ctx_);
    ASSERT_NE(md_frame, nullptr) << "md reader must stay open after NOTIFY_MD_STOPPED";
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(md_frame)).type(), DZ_FRAME_TICK);

    // 行情服务重启: STARTED 仍能补订阅期望集合
    emit_md_started("test_md");
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
    const auto req = read_next_subscribe();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->instance_id, expected_subscriber_id());
    EXPECT_EQ(req->action, dztrader::SubscribeAction::Subscribe);
    EXPECT_EQ(req->replace, true);
    EXPECT_EQ(req->instruments, std::vector<std::string>({"IC2506", "IF2506"}));
}

TEST_F(MdApiTest, NotifyMdStartedEmptyDesiredSetNoOp) {
    emit_md_started("test_md");
    // STARTED 被内部消费, 不返回用户; 无期望集合, 不补订阅
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
    EXPECT_EQ(read_next_subscribe(), std::nullopt);
}

// ── dz_next_md ──

TEST_F(MdApiTest, NextMdNullWhenNoData) { EXPECT_EQ(dz_next_md(ctx_), nullptr); }

}  // namespace
