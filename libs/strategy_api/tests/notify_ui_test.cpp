#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace {

constexpr uint64_t kMB = 1024 * 1024;

/// 预创建事件通道 (名称/目录/页大小与 master ShmManager 一致), 供 dz_init 打开。
/// 契约 shm: 策略进程仅 reader; master 负责创建事件通道。
/// 测试进程模拟 master 的角色创建通道, dz_notify_ui 的写入侧仍走 dz_init 的 writer。
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

// dz_init 构造 DzContext: 打开 dzevent 通道 writer/reader + stg.* 信号量 +
// order_id 元数据 + 必选行情 reader。测试直接经其 writer 写帧, 不依赖任何进程间协调。
class NotifyUiTest : public ::testing::Test {
protected:
    std::string home_;
    std::filesystem::path shm_dir_;
    DzContext* ctx_ = nullptr;
    /// dz_init 后创建, 定位其后: NOTIFY_UI 帧在新语义下被 dz_next_event 拦截,
    /// 故用独立 probe reader 观察 SDK 写帧 (Reader 无默认构造, 用 optional)
    std::optional<Reader> probe_;

    void SetUp() override {
        home_ = (std::filesystem::temp_directory_path() / "dz_test_strategy_notify").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        shm_dir_ = home_ + "/shm";
        // paths::shm() 按 DZTRADER_HOME 缓存: 进程内单例, 只能在首次调用前设好,
        // 之后不得再改 (所有用例共享同一 DZTRADER_HOME)。
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
        create_event_channel(shm_dir_);
        create_md_channel(shm_dir_);

        ctx_ = dz_init();
        ASSERT_NE(ctx_, nullptr) << "dz_init failed: " << dz_errmsg();
        probe_.emplace(Reader::create(dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
                                      shm_dir_, "notify_test_probe"));
    }

    void TearDown() override {
        dz_release(ctx_);
        std::filesystem::remove_all(home_);
    }

    /// 读取 dz_notify_ui 写入的下一帧 payload (经 probe reader)
    nlohmann::json read_notify_payload() {
        const std::byte* frame = probe_->next_frame();
        if (frame == nullptr) {
            return {};
        }
        const auto view = dztrader::shm::FrameView(frame);
        EXPECT_EQ(view.type(), DZ_FRAME_NOTIFY_UI);
        const auto* data = reinterpret_cast<const char*>(view.ext_payload());
        return nlohmann::json::parse(data, data + view.ext_payload_size());
    }
};

TEST_F(NotifyUiTest, InfoLevelSerializedAsString) {
    ASSERT_TRUE(dz_notify_ui(ctx_, DZ_NOTIFY_INFO, "msg", false));
    const auto payload = read_notify_payload();
    EXPECT_EQ(payload["level"], "info");
    EXPECT_EQ(payload["source"], dz_strategy_id(ctx_));
    EXPECT_EQ(payload["message"], "msg");
    EXPECT_EQ(payload["popup"], false);
    EXPECT_TRUE(payload.contains("timestamp"));
}

TEST_F(NotifyUiTest, WarnLevelSerializedAsString) {
    ASSERT_TRUE(dz_notify_ui(ctx_, DZ_NOTIFY_WARN, "msg", false));
    const auto payload = read_notify_payload();
    EXPECT_EQ(payload["level"], "warning");
}

// 回归: level 曾是整数 (DZ_NOTIFY_ERROR=4), 契约 notify-ui 规定为字符串 "error",
// 数值会被 dzweb 按 info 处理导致错误通知降级。
TEST_F(NotifyUiTest, ErrorLevelSerializedAsString) {
    ASSERT_TRUE(dz_notify_ui(ctx_, DZ_NOTIFY_ERROR, "msg", true));
    const auto payload = read_notify_payload();
    EXPECT_EQ(payload["level"], "error");
    EXPECT_EQ(payload["popup"], true);
}

TEST_F(NotifyUiTest, InvalidLevelRejected) {
    EXPECT_FALSE(dz_notify_ui(ctx_, static_cast<DzNotifyLevel>(99), "msg", false));
}

TEST_F(NotifyUiTest, NullMessageRejected) {
    EXPECT_FALSE(dz_notify_ui(ctx_, DZ_NOTIFY_INFO, nullptr, false));
}

}  // namespace
