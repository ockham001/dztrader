/**
 * @file td_refresh_controller_test.cpp
 * @brief 资金手动刷新 REST 端点（契约 rest §2.9.11 / 契约 td-refresh）。
 *
 * 真 handler 直调 + 捕获响应（与 `td_account_controller_test` 同型），真 SHM 通道做帧断言，
 * 真账户注册表 + 进程镜像做预检。断言 HTTP 语义：预检失败码（404/409/503）与失败时不写帧。
 */

#include "td_data_controller.h"
#include "fake_data_change_notifier.h"
#include "frame_router.h"
#include "ready_shm_fixture.h"
#include "td_account_registry.h"
#include "td_domain_service.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/platform/process.h>
#include <dztrader/struct.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace dztrader::webui {
namespace {

using test::ReadyShmFixture;

/// 记录广播的 WsBroadcaster 假实现（域服务构造需要；本文件不断言 WS）。
class NullBroadcaster : public WsBroadcaster {
public:
    void broadcast(const std::string&, const std::string&, const nlohmann::json&) override {}
};

class TdRefreshControllerTest : public ::testing::Test {
protected:
    ReadyShmFixture shm_;
    FrameRouter router_{[](std::function<void()> f) { f(); }};
    NullBroadcaster ws_;
    FakeDataChangeNotifier notifier_;
    std::shared_ptr<TdDomainService> domain_;
    std::shared_ptr<TdAccountRegistry> registry_;
    std::shared_ptr<ProcessMirror> process_mirror_;
    std::shared_ptr<ShmWriter> shm_writer_;
    std::shared_ptr<TdCtrl> ctrl_;

    void SetUp() override {
        domain_ = std::make_shared<TdDomainService>(router_, ws_, notifier_);
        registry_ = std::make_shared<TdAccountRegistry>();
        process_mirror_ = std::make_shared<ProcessMirror>();
        rebuild_ctrl();
    }

    void TearDown() override {
        ctrl_.reset();
        shm_writer_.reset();
        process_mirror_.reset();
        registry_.reset();
        domain_.reset();
    }

    /// 用当前依赖重建 Ctrl（用例中途替换 writer / 撤掉 registry 时用）。
    void rebuild_ctrl() {
        shm_writer_ = std::make_shared<ShmWriter>(shm_.writer);
        ctrl_ = std::make_shared<TdCtrl>(/*repo=*/nullptr, domain_, /*orders=*/nullptr,
                                         /*trades=*/nullptr, /*positions=*/nullptr,
                                         /*trading_accounts=*/nullptr, registry_, process_mirror_,
                                         shm_writer_);
    }

    /// 目标网关注册 + Running（契约 rest §2.9.11 预检 3）。
    void prime_gateway(const std::string& gateway) {
        nlohmann::json configs = nlohmann::json::object();
        configs[gateway] = nlohmann::json{
            {"args", nlohmann::json::array()},
            {"env", nlohmann::json::object()},
            {"restart",
             nlohmann::json{{"enabled", false}, {"max_attempts", 0}, {"backoff_sec", 5}}}};
        process_mirror_->update_process_configs(configs);
        dztrader::platform::ProcessStatus status;
        status.name = gateway;
        status.state = dztrader::platform::ChildState::Running;
        process_mirror_->update_status(gateway, status);
    }

    /// 账户写进注册表（等价收到配置帧，契约 td-config §5）。
    void seed_account(const std::string& gateway, const std::string& account_id) {
        registry_->reconcile_gateway(
            gateway, {TdAccountRegistryEntry{
                         .account_id = account_id, .gateway = gateway, .enabled = true}});
        registry_->note_config_frame_received();
    }

    /// 账户 Ready（在线集合来源，契约 rest §2.9.1）。
    void mark_ready(const std::string& account_id) {
        DzAccountStatus status{};
        dztrader::copy_string(status.account_id, account_id.c_str(), true);
        status.state = DZ_ACCOUNT_READY;
        status.date = 20735;
        domain_->on_account_status(status);
    }

    /// 任意已认证用户（非 admin）请求：认证由 JWT advice 层负责，这里只注入 user_id。
    drogon::HttpResponsePtr invoke_refresh(const std::string& account_id) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("regular"));
        drogon::HttpResponsePtr captured;
        ctrl_->refresh_trading_account(
            req, [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; }, account_id);
        return captured;
    }

    static nlohmann::json parse_body(const drogon::HttpResponsePtr& resp) {
        return nlohmann::json::parse(resp->getBody());
    }
};

TEST_F(TdRefreshControllerTest, DispatchesBasicJsonFrameForOnlineAccount) {
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");
    mark_ready("acc_a");

    auto resp = invoke_refresh("acc_a");

    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(parse_body(resp), (nlohmann::json{{"ok", true}}));
    // 契约 td-refresh：JSON ext 帧、无 instance_id，payload {account_id, request_id}。
    const auto frame = shm_.capture_ext_frame(DZ_FRAME_TD_REFRESH_REQ);
    ASSERT_TRUE(frame.found);
    EXPECT_EQ(frame.payload, (nlohmann::json{{"account_id", "acc_a"}, {"request_id", 0}}));
}

TEST_F(TdRefreshControllerTest, UnknownAccountReturns404AndWritesNoFrame) {
    prime_gateway("dztd_ctp");
    // 已收到过配置帧（注册表可用）但其中没有该账户 → 404，而不是降级 503。
    registry_->note_config_frame_received();

    auto resp = invoke_refresh("acc_missing");

    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "account_not_found");
    EXPECT_FALSE(shm_.has_any_frame());
}

TEST_F(TdRefreshControllerTest, GatewayNotRunningReturns409AndWritesNoFrame) {
    // 注册表能解析出网关，但进程镜像里没有它的 Running 状态。
    seed_account("dztd_ctp", "acc_a");
    mark_ready("acc_a");

    auto resp = invoke_refresh("acc_a");

    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k409Conflict);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "gateway_not_running");
    EXPECT_FALSE(shm_.has_any_frame());
}

TEST_F(TdRefreshControllerTest, OfflineAccountReturns409AndWritesNoFrame) {
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");
    // 不 mark_ready：注册表有账户但会话未 Ready（契约 rest §2.9.11 预检 4）。

    auto resp = invoke_refresh("acc_a");

    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k409Conflict);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "account_offline");
    EXPECT_FALSE(shm_.has_any_frame());
}

TEST_F(TdRefreshControllerTest, DegradedRegistryReturns503BeforeAccountLookup) {
    // 注册表未收到任何配置帧（available=false）：503，且不因「账户查不到」误判 404。
    prime_gateway("dztd_ctp");
    auto resp = invoke_refresh("acc_a");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
    EXPECT_EQ(parse_body(resp).at("error").get<std::string>(), "account registry unavailable");
    EXPECT_FALSE(shm_.has_any_frame());
}

TEST_F(TdRefreshControllerTest, PrecheckOrderAccountBeforeGateway) {
    // 契约 rest §2.9.11 预检顺序：账户不存在（2）先于网关 Running（3）。
    // 网关也没 Running、账户也不存在时，必须回 404 而不是 409。
    registry_->note_config_frame_received();  // 注册表可用但无该账户
    auto resp = invoke_refresh("acc_missing");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "account_not_found");
    EXPECT_FALSE(shm_.has_any_frame());
}

TEST_F(TdRefreshControllerTest, RegistryUnavailableReturns503AndWritesNoFrame) {
    registry_.reset();
    ctrl_ = std::make_shared<TdCtrl>(/*repo=*/nullptr, domain_, /*orders=*/nullptr,
                                     /*trades=*/nullptr, /*positions=*/nullptr,
                                     /*trading_accounts=*/nullptr, /*registry=*/nullptr,
                                     process_mirror_, shm_writer_);

    auto resp = invoke_refresh("acc_a");

    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
    EXPECT_EQ(parse_body(resp).at("error").get<std::string>(), "account registry unavailable");
    EXPECT_FALSE(shm_.has_any_frame());
}

TEST_F(TdRefreshControllerTest, ShmWriterNotAvailableReturns503AndWritesNoFrame) {
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");
    mark_ready("acc_a");
    // 未就绪 writer（nullptr 底层 MultiWriter）→ is_ready() false（不依赖环境）。
    shm_writer_ = std::make_shared<ShmWriter>(std::shared_ptr<shm::MultiWriter>());
    ctrl_ = std::make_shared<TdCtrl>(/*repo=*/nullptr, domain_, /*orders=*/nullptr,
                                     /*trades=*/nullptr, /*positions=*/nullptr,
                                     /*trading_accounts=*/nullptr, registry_, process_mirror_,
                                     shm_writer_);

    auto resp = invoke_refresh("acc_a");

    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
    EXPECT_EQ(parse_body(resp).at("error").get<std::string>(), "shm writer not available");
    EXPECT_FALSE(shm_.has_any_frame());
}

}  // namespace
}  // namespace dztrader::webui
