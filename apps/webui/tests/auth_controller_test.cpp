#include <gtest/gtest.h>
#include "auth_controller.h"
#include "repository.h"
#include "jwt.h"
#include "config.h"
#include "fake_data_change_notifier.h"

#include <nlohmann/json.hpp>
#include <memory>
#include <utility>
#include <vector>

namespace dztrader::webui {
namespace {

class AuthControllerTest : public ::testing::Test {
protected:
    std::shared_ptr<Repository> repo_;
    std::shared_ptr<WebuiConfig> cfg_;
    std::shared_ptr<FakeDataChangeNotifier> notifier_;
    std::shared_ptr<LoginCtrl> ctrl_;

    void SetUp() override {
        repo_ = std::make_shared<Repository>(":memory:");
        cfg_ = std::make_shared<WebuiConfig>();
        cfg_->jwt_secret = "test_secret";
        cfg_->token_ttl_sec = 3600;
        cfg_->admin_username = "admin";
        cfg_->admin_password = "admin_pass";
        notifier_ = std::make_shared<FakeDataChangeNotifier>();
        // Create a test user with the default security config (lockout enabled, max=5, dur=900)
        repo_->create_user("testuser", "Test User", "test@test.com", "password123", "user");
        ctrl_ = std::make_shared<LoginCtrl>(cfg_, repo_, *notifier_);
    }

    drogon::HttpResponsePtr do_login(const std::string& username, const std::string& password) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setPath("/api/login");
        const nlohmann::json body = {{"username", username}, {"password", password}};
        req->setBody(body.dump());
        req->setContentTypeString("application/nlohmann::json");
        drogon::HttpResponsePtr captured;
        ctrl_->login(req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
        return captured;
    }

    drogon::HttpResponsePtr do_login_raw(const std::string& body) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setPath("/api/login");
        req->setBody(body);
        req->setContentTypeString("application/nlohmann::json");
        drogon::HttpResponsePtr captured;
        ctrl_->login(req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
        return captured;
    }

    static nlohmann::json parse_body(const drogon::HttpResponsePtr& resp) {
        return nlohmann::json::parse(resp->getBody());
    }

    /// 断言 400 形状（契约 rest §1）：请求体非法是通用失败，只有 error、**无 code**。
    static void expect_bad_request(const drogon::HttpResponsePtr& resp) {
        EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
        const auto body = parse_body(resp);
        EXPECT_EQ(body["error"].get<std::string>(), "bad request");
        EXPECT_FALSE(body.contains("code")) << body.dump();
    }

    /// 断言登录失败错误体形状（契约 rest §1/§2.1）：
    /// {"error": <human message>, "code": <snake_case machine code>}。
    /// 机器码只出现在 `code`；`error` 是人类可读消息，不得再承载机器码。
    static void expect_login_error(const drogon::HttpResponsePtr& resp,
                                   drogon::HttpStatusCode status,
                                   const std::string& code) {
        EXPECT_EQ(resp->getStatusCode(), status);
        const auto body = parse_body(resp);
        ASSERT_TRUE(body.contains("code")) << body.dump();
        EXPECT_EQ(body["code"].get<std::string>(), code);
        ASSERT_TRUE(body.contains("error")) << body.dump();
        ASSERT_TRUE(body["error"].is_string()) << body.dump();
        const auto message = body["error"].get<std::string>();
        EXPECT_FALSE(message.empty());
        EXPECT_NE(message, code);  // error 是消息，不是机器码
    }
};

// 1. Valid credentials → 200, body has token, expires_in, user with correct fields
TEST_F(AuthControllerTest, LoginSuccessReturns200) {
    auto resp = do_login("testuser", "password123");
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_TRUE(body.contains("token"));
    EXPECT_EQ(body["expires_in"].get<uint32_t>(), cfg_->token_ttl_sec);
    EXPECT_TRUE(body.contains("user"));
    auto user = body["user"];
    EXPECT_EQ(user["username"].get<std::string>(), "testuser");
    EXPECT_EQ(user["display_name"].get<std::string>(), "Test User");
    EXPECT_EQ(user["email"].get<std::string>(), "test@test.com");
    EXPECT_EQ(user["role"].get<std::string>(), "user");
    // password_hash must never appear in the response
    EXPECT_FALSE(user.contains("password_hash"));
    // 普通用户不触发默认密码告警
    EXPECT_FALSE(body["is_default_password"].get<bool>());
}

// 2. Wrong password → 401 invalid_credentials
TEST_F(AuthControllerTest, LoginWrongPasswordReturns401) {
    auto resp = do_login("testuser", "wrongpassword");
    // 契约 rest §1/§2.1: 错误体 {"error": <human message>, "code": <machine code>}
    expect_login_error(resp, drogon::k401Unauthorized, "invalid_credentials");
}

// 3. User doesn't exist → 401 invalid_credentials
TEST_F(AuthControllerTest, LoginNonexistentUserReturns401) {
    auto resp = do_login("ghost", "whatever");
    expect_login_error(resp, drogon::k401Unauthorized, "invalid_credentials");
}

// 4. Disabled user → 401 account_disabled
TEST_F(AuthControllerTest, LoginDisabledUserReturns401) {
    auto user = repo_->get_user_by_username("testuser");
    repo_->update_user_status(user->id, "disabled");
    auto resp = do_login("testuser", "password123");
    expect_login_error(resp, drogon::k401Unauthorized, "account_disabled");
}

// 5. Locked user → 423 account_locked with locked_until (绝对 Unix 秒级时间戳)
TEST_F(AuthControllerTest, LoginLockedUserReturns423) {
    auto user = repo_->get_user_by_username("testuser");
    const int64_t future = static_cast<int64_t>(std::time(nullptr)) + 600;
    repo_->lock_user(user->id, future);
    auto resp = do_login("testuser", "password123");
    expect_login_error(resp, drogon::k423Locked, "account_locked");
    auto body = parse_body(resp);
    EXPECT_TRUE(body.contains("locked_until"));
    // 契约 rest §1: locked_until 为绝对 Unix 秒级时间戳（与 User.locked_until 同语义）
    EXPECT_EQ(body["locked_until"].get<int64_t>(), future);
}

// 6. After N failed attempts (max_failed_attempts), account gets locked → 423
TEST_F(AuthControllerTest, LoginFailedAttemptsLockAccount) {
    auto config = repo_->get_security_config();
    ASSERT_TRUE(config.login_lockout_enabled);
    const int max_attempts = config.max_failed_attempts;
    ASSERT_GT(max_attempts, 0);

    // Trigger (max_attempts - 1) failures: each should be 401 invalid_credentials
    for (int i = 0; i < max_attempts - 1; ++i) {
        auto resp = do_login("testuser", "badpwd");
        expect_login_error(resp, drogon::k401Unauthorized, "invalid_credentials");
    }

    // The Nth failure should trigger lockout → 423
    const int64_t before = static_cast<int64_t>(std::time(nullptr));
    auto resp = do_login("testuser", "badpwd");
    const int64_t after = static_cast<int64_t>(std::time(nullptr));
    expect_login_error(resp, drogon::k423Locked, "account_locked");
    auto body = parse_body(resp);
    // 契约 rest §1: locked_until 为绝对 Unix 秒级时间戳 = 锁定时刻 + 锁定时长
    const int64_t locked_until = body["locked_until"].get<int64_t>();
    EXPECT_GE(locked_until, before + config.lockout_duration_sec);
    EXPECT_LE(locked_until, after + config.lockout_duration_sec);

    // Verify the user is now locked in the DB
    auto user = repo_->get_user_by_username("testuser");
    EXPECT_EQ(user->status, "locked");
    EXPECT_GT(user->locked_until, static_cast<int64_t>(std::time(nullptr)));
}

// 7. Successful login resets failed_login_count to 0
TEST_F(AuthControllerTest, LoginSuccessResetsFailedCount) {
    // Build up some failed attempts first
    do_login("testuser", "bad1");
    do_login("testuser", "bad2");
    auto user = repo_->get_user_by_username("testuser");
    EXPECT_EQ(user->failed_login_count, 2);

    // Now log in successfully
    auto resp = do_login("testuser", "password123");
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    auto refreshed = repo_->get_user_by_username("testuser");
    EXPECT_EQ(refreshed->failed_login_count, 0);
}

// 8. Successful login records a history entry (success=true)
TEST_F(AuthControllerTest, LoginSuccessRecordsHistory) {
    auto resp = do_login("testuser", "password123");
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    auto history = repo_->get_login_history(1, 50);
    bool found_success = false;
    for (const auto& e : history.entries) {
        if (e.username == "testuser" && e.success) {
            found_success = true;
            break;
        }
    }
    EXPECT_TRUE(found_success);
}

// 9. Invalid JSON body → 400
TEST_F(AuthControllerTest, LoginBadJsonReturns400) {
    auto resp = do_login_raw("not valid nlohmann::json {{{");
    expect_bad_request(resp);
}

// 9a. 合法 JSON 但非对象 / 字段类型错 → 400（契约 rest §1：bad request 不得 500）
TEST_F(AuthControllerTest, LoginNonObjectOrNonStringBodyReturns400) {
    const std::vector<std::string> cases = {
        "[]",
        "123",
        R"({"username":123,"password":"password123"})",
        R"({"username":"testuser","password":123})",
        R"({"username":"testuser"})",
        R"({"password":"password123"})",
    };
    for (const auto& raw : cases) {
        auto resp = do_login_raw(raw);
        expect_bad_request(resp);
    }
}

// 9b. 空 username / 空 password → 400（契约 rest §2.1 校验）
TEST_F(AuthControllerTest, LoginEmptyFieldsReturn400) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"", "password123"},
        {"testuser", ""},
        {"", ""},
    };
    for (const auto& [username, password] : cases) {
        auto resp = do_login(username, password);
        expect_bad_request(resp);
    }
}

// 9c. IP 限制（契约 rest §2.2: access_mode + 黑白名单）
TEST_F(AuthControllerTest, LoginBlacklistedIpReturns401IpBanned) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    const std::string peer_ip = req->getPeerAddr().toIp();
    ASSERT_TRUE(repo_->add_to_blacklist(peer_ip, "test ban", "manual"));

    auto sec = repo_->get_security_config();
    sec.access_mode = "blacklist";
    ASSERT_TRUE(repo_->set_security_config(sec));

    req->setBody(R"({"username":"testuser","password":"password123"})");
    drogon::HttpResponsePtr captured;
    ctrl_->login(req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    ASSERT_NE(captured.get(), nullptr);
    expect_login_error(captured, drogon::k401Unauthorized, "ip_banned");
}

TEST_F(AuthControllerTest, LoginWhitelistModeRejectsUnlistedIp) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    const std::string peer_ip = req->getPeerAddr().toIp();

    auto sec = repo_->get_security_config();
    sec.access_mode = "whitelist";
    ASSERT_TRUE(repo_->set_security_config(sec));

    req->setBody(R"({"username":"testuser","password":"password123"})");
    drogon::HttpResponsePtr rejected;
    ctrl_->login(req, [&rejected](const drogon::HttpResponsePtr& r) { rejected = r; });
    ASSERT_NE(rejected.get(), nullptr);
    expect_login_error(rejected, drogon::k401Unauthorized, "ip_banned");

    // 白名单放行后正常登录
    ASSERT_TRUE(repo_->add_to_whitelist(peer_ip, "test allow"));
    auto req2 = drogon::HttpRequest::newHttpRequest();
    req2->setMethod(drogon::Post);
    req2->setBody(R"({"username":"testuser","password":"password123"})");
    drogon::HttpResponsePtr allowed;
    ctrl_->login(req2, [&allowed](const drogon::HttpResponsePtr& r) { allowed = r; });
    ASSERT_NE(allowed.get(), nullptr);
    EXPECT_EQ(allowed->getStatusCode(), drogon::k200OK);
}

// 11. is_default_password 分支（前端 stores/auth.ts 依赖该字段触发默认密码告警）
TEST_F(AuthControllerTest, LoginAdminWithUnackedDefaultPasswordFlagsTrue) {
    cfg_->admin_password_is_default = true;
    repo_->create_user("admin", "Administrator", "", "admin_pass", "admin");

    auto resp = do_login("admin", "admin_pass");
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_TRUE(parse_body(resp)["is_default_password"].get<bool>());

    // 确认告警后不再提示
    auto admin = repo_->get_user_by_username("admin");
    ASSERT_TRUE(admin.has_value());
    ASSERT_TRUE(repo_->ack_default_password(admin->id));
    auto resp2 = do_login("admin", "admin_pass");
    ASSERT_EQ(resp2->getStatusCode(), drogon::k200OK);
    EXPECT_FALSE(parse_body(resp2)["is_default_password"].get<bool>());
}

TEST_F(AuthControllerTest, LoginAdminWithCustomPasswordFlagsFalse) {
    // 配置已改非默认密码 -> 即便 admin 未 ack 也不告警
    cfg_->admin_password_is_default = false;
    repo_->create_user("admin", "Administrator", "", "admin_pass", "admin");
    auto resp = do_login("admin", "admin_pass");
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_FALSE(parse_body(resp)["is_default_password"].get<bool>());
}

// 10. The token in the response can be verified with jwt_verify
TEST_F(AuthControllerTest, LoginSuccessReturnsValidJWT) {
    auto resp = do_login("testuser", "password123");
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    const std::string token = body["token"].get<std::string>();
    ASSERT_FALSE(token.empty());

    std::string user_id;
    const bool ok = jwt_verify(token, cfg_->jwt_secret, user_id);
    EXPECT_TRUE(ok);
    EXPECT_EQ(user_id, "testuser");

    // A token signed with a different secret should not verify
    std::string user_id2;
    const bool ok2 = jwt_verify(token, "wrong_secret", user_id2);
    EXPECT_FALSE(ok2);
}

}  // anonymous namespace
}  // namespace dztrader::webui
