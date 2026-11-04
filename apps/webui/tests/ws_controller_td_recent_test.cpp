/**
 * @file ws_controller_td_recent_test.cpp
 * @brief `/ws` 建连补发 `td_recent` 钩子（契约 webui-ws §1/§2.5）。
 *
 * 覆盖验收点：鉴权成功建连后、`snapshot` 之后定向发送一次
 * `{"type":"td_recent","payload":{"events":[...]}}`；provider 未装配时不发送。
 * 用假连接捕获 send 序列（WsController 依赖 drogon 连接对象，无法脱离框架实例化，
 * 但 WebSocketConnection 是纯虚接口，可测）。
 */

#include "ws_controller.h"
#include "test_temp_dir.h"

#include <drogon/HttpRequest.h>
#include <drogon/WebSocketConnection.h>
#include <dztrader/core/core_data_type.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <trantor/net/InetAddress.h>

#include <memory>
#include <string>
#include <vector>

#include "jwt.h"
#include "md_control_domain_service.h"
#include "mirror_store.h"
#include "process_mirror.h"
#include "repository.h"

namespace dztrader::webui {
namespace {

/// 假 WS 连接：按顺序记录 send 的文本消息；其余接口无需实现语义。
class FakeConnection : public drogon::WebSocketConnection {
public:
    std::vector<std::string> sent;

    void send(const char* msg, uint64_t len, drogon::WebSocketMessageType) override {
        sent.emplace_back(msg, static_cast<size_t>(len));
    }
    void send(std::string_view msg, drogon::WebSocketMessageType) override {
        sent.emplace_back(msg);
    }
    void sendJson(const Json::Value&, drogon::WebSocketMessageType) override {}
    const trantor::InetAddress& localAddr() const override { return address_; }
    const trantor::InetAddress& peerAddr() const override { return address_; }
    bool connected() const override { return true; }
    bool disconnected() const override { return false; }
    void shutdown(drogon::CloseCode, const std::string&) override { closed = true; }
    void forceClose() override { closed = true; }
    void setPingMessage(const std::string&, const std::chrono::duration<double>&) override {}
    void disablePing() override {}

    bool closed = false;
    trantor::InetAddress address_;
};

class WsControllerTdRecentTest : public ::testing::Test {
protected:
    std::shared_ptr<Repository> repo_;
    std::unique_ptr<MirrorStore> mirror_;
    std::unique_ptr<ProcessMirror> process_mirror_;
    std::unique_ptr<MdControlDomainService> md_control_;
    std::shared_ptr<WsController> controller_;
    WebuiConfig cfg_;

    void SetUp() override {
        repo_ = std::make_shared<Repository>(":memory:");
        repo_->create_user("tester", "Tester", "t@test.com", "Password!1234", "admin");
        mirror_ = std::make_unique<MirrorStore>();
        process_mirror_ = std::make_unique<ProcessMirror>();
        md_control_ = std::make_unique<MdControlDomainService>(*process_mirror_, nullptr);
        cfg_.jwt_secret = "test_secret_for_td_recent_hook";
        cfg_.admin_password_is_default = false;
        controller_ =
            std::make_shared<WsController>(cfg_, repo_, nullptr, nullptr, *mirror_, *md_control_);
    }

    void TearDown() override {
        controller_.reset();
        md_control_.reset();
        process_mirror_.reset();
        mirror_.reset();
    }

    drogon::HttpRequestPtr authenticated_request() {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setParameter(
            "token", jwt_sign("tester", cfg_.token_ttl_sec, cfg_.jwt_secret, /*token_version=*/0));
        return req;
    }

    /// 解析已发送消息里第一条指定 type 的 JSON。
    static std::optional<nlohmann::json> find_sent(const FakeConnection& conn,
                                                   const std::string& type) {
        for (const auto& raw : conn.sent) {
            const auto parsed = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                continue;
            }
            if (parsed.value("type", "") == type) {
                return parsed;
            }
        }
        return std::nullopt;
    }
};

TEST_F(WsControllerTdRecentTest, SendsTdRecentAfterSnapshot) {
    const nlohmann::json payload = {
        {"events",
         {{{"type", "td_order_report"}, {"data", {{"account_id", "ACC1"}, {"order_id", 7}}}}}}};
    controller_->set_td_recent_provider([payload]() { return payload; });

    auto fake = std::make_shared<FakeConnection>();
    controller_->handleNewConnection(authenticated_request(), fake);

    const auto snapshot = find_sent(*fake, "snapshot");
    const auto recent = find_sent(*fake, "td_recent");
    ASSERT_TRUE(snapshot.has_value()) << "snapshot must be pushed on connect";
    ASSERT_TRUE(recent.has_value()) << "td_recent must be pushed after snapshot";

    // 契约 webui-ws §2.3：td_recent 用 payload 键（控制消息），不是 data
    ASSERT_TRUE(recent->contains("payload"));
    EXPECT_FALSE(recent->contains("data"));
    EXPECT_EQ((*recent)["payload"]["events"].size(), 1U);
    EXPECT_EQ((*recent)["payload"]["events"][0]["type"].get<std::string>(), "td_order_report");
    EXPECT_EQ((*recent)["payload"]["events"][0]["data"]["order_id"].get<int>(), 7);

    // 顺序：snapshot 先于 td_recent（契约 webui-ws §1）
    size_t snapshot_index = 0;
    size_t recent_index = 0;
    for (size_t i = 0; i < fake->sent.size(); ++i) {
        const auto parsed = nlohmann::json::parse(fake->sent[i], nullptr, false);
        if (parsed.is_discarded()) {
            continue;
        }
        const std::string type = parsed.value("type", "");
        if (type == "snapshot") {
            snapshot_index = i;
        }
        if (type == "td_recent") {
            recent_index = i;
        }
    }
    EXPECT_LT(snapshot_index, recent_index);
}

TEST_F(WsControllerTdRecentTest, SendsTdRecentWithEmptyEventsArray) {
    controller_->set_td_recent_provider(
        []() { return nlohmann::json{{"events", nlohmann::json::array()}}; });
    auto fake = std::make_shared<FakeConnection>();
    controller_->handleNewConnection(authenticated_request(), fake);

    const auto recent = find_sent(*fake, "td_recent");
    // 契约 webui-ws §2.5：无事件时仍发送 {"events": []}（前端据此确定补发阶段结束）
    ASSERT_TRUE(recent.has_value());
    EXPECT_TRUE((*recent)["payload"]["events"].is_array());
    EXPECT_TRUE((*recent)["payload"]["events"].empty());
}

TEST_F(WsControllerTdRecentTest, DoesNotSendTdRecentWhenProviderMissing) {
    auto fake = std::make_shared<FakeConnection>();
    controller_->handleNewConnection(authenticated_request(), fake);

    EXPECT_TRUE(find_sent(*fake, "snapshot").has_value());
    EXPECT_FALSE(find_sent(*fake, "td_recent").has_value());
}

TEST_F(WsControllerTdRecentTest, DoesNotSendTdRecentWhenProviderThrows) {
    controller_->set_td_recent_provider(
        []() -> nlohmann::json { throw std::runtime_error("provider failed"); });
    auto fake = std::make_shared<FakeConnection>();
    // 补发失败不阻断连接建立
    EXPECT_NO_THROW(controller_->handleNewConnection(authenticated_request(), fake));
    EXPECT_FALSE(find_sent(*fake, "td_recent").has_value());
    EXPECT_FALSE(fake->closed) << "td_recent failure must not close the connection";
}

TEST_F(WsControllerTdRecentTest, RejectsUnauthenticatedConnectionWithoutTdRecent) {
    controller_->set_td_recent_provider(
        []() { return nlohmann::json{{"events", nlohmann::json::array()}}; });
    auto fake = std::make_shared<FakeConnection>();
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setParameter("token", "not-a-jwt");
    controller_->handleNewConnection(request, fake);

    EXPECT_TRUE(fake->closed);
    EXPECT_FALSE(find_sent(*fake, "snapshot").has_value());
    EXPECT_FALSE(find_sent(*fake, "td_recent").has_value());
    EXPECT_TRUE(find_sent(*fake, "error").has_value());
}

}  // namespace
}  // namespace dztrader::webui
