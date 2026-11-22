/**
 * @file td_control_domain_service_test.cpp
 * @brief 交易账户会话控制领域服务（契约 td-connection / 契约 webui-ws §3）。
 *
 * 真 SHM 通道做帧断言（`ReadyShmFixture` 同型）；账户注册表与进程镜像用真实实现，
 * 不引入 mock——守卫判定与写帧形状都要逐字段锁定。
 */

#include "td_control_domain_service.h"
#include "mirror_store.h"
#include "process_mirror.h"
#include "td_account_registry.h"
#include "test_util.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/platform/process.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dztrader::webui {
namespace {

/// 真 SHM 通道 fixture：捕获服务写出的定向帧（帧断言用）。
struct SessionShmFixture {
    std::string channel_name;
    std::filesystem::path shm_dir;
    std::shared_ptr<shm::ChannelMeta> meta;
    std::shared_ptr<shm::MultiWriter> writer;
    std::optional<shm::Reader> reader;
    static constexpr uint64_t MB = 1024ull * 1024;

    SessionShmFixture() {
        static std::atomic<uint64_t> instance_seq{0};
        channel_name = "dz_test_td_session_svc_" + test::unique_suffix() + "_" +
                       std::to_string(instance_seq.fetch_add(1, std::memory_order_relaxed));
        shm_dir = std::filesystem::temp_directory_path() / channel_name;
        std::filesystem::remove_all(shm_dir);
        std::filesystem::create_directories(shm_dir);
        const shm::ChannelConfig cfg{.channel_name = channel_name,
                                     .shm_dir = shm_dir,
                                     .meta_file_size = 4 * MB,
                                     .page_size = 1 * MB,
                                     .lock_memory = false,
                                     .prefetch_memory = false};
        meta = std::make_shared<shm::ChannelMeta>(shm::ChannelMeta::open_or_create(cfg));
        writer = std::make_shared<shm::MultiWriter>(shm::MultiWriter::create(meta, "test_writer"));
        reader = shm::Reader::create(meta, "test_reader");
    }
    ~SessionShmFixture() {
        reader.reset();
        writer.reset();
        meta.reset();
        dztrader::shm::test::cleanup_channel(channel_name, shm_dir);
    }
    SessionShmFixture(const SessionShmFixture&) = delete;
    SessionShmFixture& operator=(const SessionShmFixture&) = delete;

    struct Captured {
        bool found = false;
        std::string instance_id;
        nlohmann::json payload;
    };

    Captured capture_inst_frame(DzFrameType type, const std::string& instance) {
        Captured out;
        while (const auto* raw = reader->next_frame()) {
            const shm::FrameView view(raw);
            if (view.type() != type) {
                continue;
            }
            if (std::string(view.ext_inst_id()) != instance) {
                continue;
            }
            const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
            out.payload = nlohmann::json::parse(data, data + view.ext_inst_payload_size());
            out.instance_id = instance;
            out.found = true;
            break;
        }
        return out;
    }

    /// 是否写入了任何帧（守卫失败路径必须一帧不写）。
    bool has_any_frame() {
        while (const auto* raw = reader->next_frame()) {
            (void)raw;
            return true;
        }
        return false;
    }
};

class TdControlDomainServiceTest : public ::testing::Test {
protected:
    MirrorStore store_;
    ProcessMirror process_mirror_{store_};
    TdAccountRegistry registry_;

    void prime_gateway(const std::string& gateway) {
        dztrader::platform::ProcessStatus status;
        status.name = gateway;
        status.state = dztrader::platform::ChildState::Running;
        process_mirror_.update_status(gateway, status);
    }

    void seed_account(const std::string& gateway, const std::string& account_id) {
        registry_.reconcile_gateway(
            gateway, {TdAccountRegistryEntry{
                         .account_id = account_id, .gateway = gateway, .enabled = true}});
    }

    static std::vector<nlohmann::json> capture_replies(TdControlDomainService& svc,
                                                       bool is_connect,
                                                       bool is_admin,
                                                       const std::string& account_id,
                                                       uint64_t seq) {
        std::vector<nlohmann::json> replies;
        TdControlDomainService::Reply reply = [&replies](nlohmann::json frame) {
            replies.push_back(std::move(frame));
        };
        if (is_connect) {
            svc.handle_td_connect(is_admin, account_id, seq, reply);
        } else {
            svc.handle_td_disconnect(is_admin, account_id, seq, reply);
        }
        return replies;
    }
};

// ============================================================================
// 守卫失败：不写帧、回 error
// ============================================================================

TEST_F(TdControlDomainServiceTest, ConnectNonAdminRepliesErrorNoFrame) {
    SessionShmFixture fx;
    ShmWriter writer(fx.writer);
    TdControlDomainService svc(registry_, process_mirror_, &writer);
    const auto replies = capture_replies(svc, /*is_connect=*/true, /*is_admin=*/false, "acc_a", 42);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0]["type"], "error");
    EXPECT_EQ(replies[0]["seq"], 42);
    EXPECT_EQ(replies[0]["payload"]["message"], "admin required");
    EXPECT_FALSE(fx.has_any_frame());
}

TEST_F(TdControlDomainServiceTest, ConnectUnknownAccountRepliesErrorNoFrame) {
    SessionShmFixture fx;
    registry_.note_config_frame_received();  // 注册表可用（已收到过配置帧），但账户不在此
    ShmWriter writer(fx.writer);
    TdControlDomainService svc(registry_, process_mirror_, &writer);
    const auto replies = capture_replies(svc, true, true, "missing", 1);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0]["type"], "error");
    EXPECT_EQ(replies[0]["payload"]["message"], "account not found: missing");
    EXPECT_FALSE(fx.has_any_frame());
}

TEST_F(TdControlDomainServiceTest, ConnectRegistryUnavailableRepliesErrorNoFrame) {
    // 注册表尚未收到任何配置帧（available=false）：无法解析归属 → 不写帧。
    SessionShmFixture fx;
    ShmWriter writer(fx.writer);
    TdControlDomainService svc(registry_, process_mirror_, &writer);
    const auto replies = capture_replies(svc, true, true, "acc_a", 2);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0]["payload"]["message"], "account registry unavailable");
    EXPECT_FALSE(fx.has_any_frame());
}

TEST_F(TdControlDomainServiceTest, ConnectGatewayNotRunningRepliesErrorNoFrame) {
    SessionShmFixture fx;
    seed_account("dztd_ctp", "acc_a");  // 已登记但进程镜像非 Running
    ShmWriter writer(fx.writer);
    TdControlDomainService svc(registry_, process_mirror_, &writer);
    const auto replies = capture_replies(svc, true, true, "acc_a", 3);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0]["payload"]["message"], "td gateway not running: dztd_ctp");
    EXPECT_FALSE(fx.has_any_frame());
}

TEST_F(TdControlDomainServiceTest, ConnectWriterUnavailableRepliesErrorNoFrame) {
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");
    TdControlDomainService svc(registry_, process_mirror_, nullptr);
    const auto replies = capture_replies(svc, true, true, "acc_a", 4);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0]["payload"]["message"], "shm writer not available");
}

// ============================================================================
// 守卫通过：写 TD_CONNECT / TD_DISCONNECT + 回执
// ============================================================================

TEST_F(TdControlDomainServiceTest, ConnectOkWritesConnectFrameAndAck) {
    SessionShmFixture fx;
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");
    ShmWriter writer(fx.writer);
    TdControlDomainService svc(registry_, process_mirror_, &writer);
    const auto replies = capture_replies(svc, /*is_connect=*/true, /*is_admin=*/true, "acc_a", 7);

    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0]["type"], "td_connect_ack");
    EXPECT_EQ(replies[0]["seq"], 7);
    EXPECT_EQ(replies[0]["payload"]["account_id"], "acc_a");
    EXPECT_EQ(replies[0]["payload"]["ok"], true);

    const auto frame = fx.capture_inst_frame(DZ_FRAME_TD_CONNECT, "dztd_ctp");
    ASSERT_TRUE(frame.found);
    EXPECT_EQ(frame.payload, (nlohmann::json{{"account_id", "acc_a"}}));
}

TEST_F(TdControlDomainServiceTest, DisconnectOkWritesDisconnectFrameAndAck) {
    SessionShmFixture fx;
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");
    ShmWriter writer(fx.writer);
    TdControlDomainService svc(registry_, process_mirror_, &writer);
    const auto replies = capture_replies(svc, /*is_connect=*/false, /*is_admin=*/true, "acc_a", 9);

    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0]["type"], "td_disconnect_ack");
    EXPECT_EQ(replies[0]["payload"]["account_id"], "acc_a");
    EXPECT_EQ(replies[0]["payload"]["ok"], true);

    const auto frame = fx.capture_inst_frame(DZ_FRAME_TD_DISCONNECT, "dztd_ctp");
    ASSERT_TRUE(frame.found);
    EXPECT_EQ(frame.payload, (nlohmann::json{{"account_id", "acc_a"}}));
}

}  // namespace
}  // namespace dztrader::webui
