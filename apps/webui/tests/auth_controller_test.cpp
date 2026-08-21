#include <gtest/gtest.h>
#include "auth_controller.h"
#include "repository.h"
#include "jwt.h"
#include "config.h"
#include "fake_data_change_notifier.h"

#include <nlohmann/json.hpp>
#include <memory>

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
}

// 2. Wrong password → 401 invalid_credentials
TEST_F(AuthControllerTest, LoginWrongPasswordReturns401) {
    auto resp = do_login("testuser", "wrongpassword");
    EXPECT_EQ(resp->getStatusCode(), drogon::k401Unauthorized);
    auto body = parse_body(resp);
    EXPECT_EQ(body["error"].get<std::string>(), "invalid_credentials");
}

// 3. User doesn't exist → 401 invalid_credentials
TEST_F(AuthControllerTest, LoginNonexistentUserReturns401) {
    auto resp = do_login("ghost", "whatever");
    EXPECT_EQ(resp->getStatusCode(), drogon::k401Unauthorized);
    auto body = parse_body(resp);
    EXPECT_EQ(body["error"].get<std::string>(), "invalid_credentials");
}

// 4. Disabled user → 401 account_disabled
TEST_F(AuthControllerTest, LoginDisabledUserReturns401) {
    auto user = repo_->get_user_by_username("testuser");
    repo_->update_user_status(user->id, "disabled");
    auto resp = do_login("testuser", "password123");
    EXPECT_EQ(resp->getStatusCode(), drogon::k401Unauthorized);
    auto body = parse_body(resp);
    EXPECT_EQ(body["error"].get<std::string>(), "account_disabled");
}

// 5. Locked user → 423 account_locked with locked_until
TEST_F(AuthControllerTest, LoginLockedUserReturns423) {
    auto user = repo_->get_user_by_username("testuser");
    const int64_t future = static_cast<int64_t>(std::time(nullptr)) + 600;
    repo_->lock_user(user->id, future);
    auto resp = do_login("testuser", "password123");
    EXPECT_EQ(resp->getStatusCode(), drogon::k423Locked);
    auto body = parse_body(resp);
    EXPECT_EQ(body["error"].get<std::string>(), "account_locked");
    EXPECT_TRUE(body.contains("locked_until"));
    const int64_t remaining = body["locked_until"].get<int64_t>();
    EXPECT_GT(remaining, 0);
    EXPECT_LE(remaining, 600);
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
        ASSERT_EQ(resp->getStatusCode(), drogon::k401Unauthorized);
        auto body = parse_body(resp);
        ASSERT_EQ(body["error"].get<std::string>(), "invalid_credentials");
    }

    // The Nth failure should trigger lockout → 423
    auto resp = do_login("testuser", "badpwd");
    EXPECT_EQ(resp->getStatusCode(), drogon::k423Locked);
    auto body = parse_body(resp);
    EXPECT_EQ(body["error"].get<std::string>(), "account_locked");
    EXPECT_EQ(body["locked_until"].get<int64_t>(), config.lockout_duration_sec);

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
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    auto body = parse_body(resp);
    EXPECT_EQ(body["error"].get<std::string>(), "bad request");
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
