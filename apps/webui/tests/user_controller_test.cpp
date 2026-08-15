#include <gtest/gtest.h>
#include "user_controller.h"
#include "repository.h"

#include <nlohmann/json.hpp>
#include <memory>

namespace dztrader::webui {
namespace {


class UserControllerTest : public ::testing::Test {
protected:
    std::shared_ptr<Repository> repo_;
    WebuiConfig cfg_;
    std::shared_ptr<UserCtrl> ctrl_;
    int64_t admin_id_ = 0;
    int64_t user_id_ = 0;

    void SetUp() override {
        repo_ = std::make_shared<Repository>(":memory:");
        admin_id_ = repo_->create_user("admin", "Administrator", "admin@test.com", "pass", "admin");
        user_id_ = repo_->create_user("regular", "Regular User", "reg@test.com", "pass", "user");
        ctrl_ = std::make_shared<UserCtrl>(repo_, cfg_);
    }

    /// Build a request carrying an admin JWT identity.
    drogon::HttpRequestPtr admin_req() {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("admin"));
        return req;
    }

    /// Build a request carrying a non-admin JWT identity.
    drogon::HttpRequestPtr user_req() {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("regular"));
        return req;
    }

    /// Invoke a controller method and capture the response synchronously.
    template <typename Fn, typename... Args>
    drogon::HttpResponsePtr invoke(Fn fn, const drogon::HttpRequestPtr& req, Args&&... args) {
        drogon::HttpResponsePtr captured;
        (ctrl_.get()->*fn)(req,
                          [&captured](const drogon::HttpResponsePtr& r) { captured = r; },
                          std::forward<Args>(args)...);
        return captured;
    }

    /// Parse a response body as JSON.
    static nlohmann::json parse_body(const drogon::HttpResponsePtr& resp) {
        return nlohmann::json::parse(resp->getBody());
    }
};

// ---- create ----

TEST_F(UserControllerTest, CreateAsAdminReturns201) {
    auto req = admin_req();
    req->setBody(R"({"username":"newuser","display_name":"New","email":"n@t.com","password":"pw","role":"user"})");
    auto resp = invoke(&UserCtrl::create, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k201Created);
    auto body = parse_body(resp);
    EXPECT_EQ(body["username"].get<std::string>(), "newuser");
    EXPECT_EQ(body["role"].get<std::string>(), "user");
    EXPECT_EQ(body["status"].get<std::string>(), "offline");
}

TEST_F(UserControllerTest, CreateAsNonAdminReturns403) {
    auto req = user_req();
    req->setBody(R"({"username":"newuser","password":"pw"})");
    auto resp = invoke(&UserCtrl::create, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(UserControllerTest, CreateDuplicateReturns409) {
    auto req = admin_req();
    req->setBody(R"({"username":"regular","password":"pw"})");
    auto resp = invoke(&UserCtrl::create, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k409Conflict);
}

TEST_F(UserControllerTest, CreateMissingFieldsReturns400) {
    auto req = admin_req();
    req->setBody(R"({"username":"nopass"})");
    auto resp = invoke(&UserCtrl::create, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

// ---- list ----

TEST_F(UserControllerTest, ListReturnsAllUsers) {
    auto req = admin_req();
    auto resp = invoke(&UserCtrl::list, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["total"].get<int>(), 2);
    EXPECT_EQ(body["users"].size(), 2);
}

TEST_F(UserControllerTest, ListWithRoleFilter) {
    auto req = admin_req();
    req->setParameter("role", "admin");
    auto resp = invoke(&UserCtrl::list, req);
    auto body = parse_body(resp);
    EXPECT_EQ(body["total"].get<int>(), 1);
    EXPECT_EQ(body["users"][0]["username"].get<std::string>(), "admin");
}

TEST_F(UserControllerTest, ListWithSearch) {
    auto req = admin_req();
    req->setParameter("search", "reg");
    auto resp = invoke(&UserCtrl::list, req);
    auto body = parse_body(resp);
    EXPECT_EQ(body["total"].get<int>(), 1);
    EXPECT_EQ(body["users"][0]["username"].get<std::string>(), "regular");
}

// ---- get ----

TEST_F(UserControllerTest, GetExistingUser) {
    auto req = admin_req();
    auto resp = invoke(&UserCtrl::get, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["id"].get<int64_t>(), user_id_);
    EXPECT_EQ(body["username"].get<std::string>(), "regular");
}

TEST_F(UserControllerTest, GetNonexistentReturns404) {
    auto req = admin_req();
    auto resp = invoke(&UserCtrl::get, req, 99999);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

// ---- update ----

TEST_F(UserControllerTest, UpdateUser) {
    auto req = admin_req();
    req->setBody(R"({"display_name":"Updated","email":"up@t.com","role":"admin"})");
    auto resp = invoke(&UserCtrl::update, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["display_name"].get<std::string>(), "Updated");
    EXPECT_EQ(body["email"].get<std::string>(), "up@t.com");
    EXPECT_EQ(body["role"].get<std::string>(), "admin");
}

TEST_F(UserControllerTest, UpdateNonexistentReturns404) {
    auto req = admin_req();
    req->setBody(R"({"display_name":"x"})");
    auto resp = invoke(&UserCtrl::update, req, 99999);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

// ---- remove ----

TEST_F(UserControllerTest, RemoveUserReturns204) {
    auto req = admin_req();
    auto resp = invoke(&UserCtrl::remove, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k204NoContent);
    // Verify gone
    EXPECT_FALSE(repo_->get_user_by_id(user_id_).has_value());
}

TEST_F(UserControllerTest, RemoveNonexistentReturns404) {
    auto req = admin_req();
    auto resp = invoke(&UserCtrl::remove, req, 99999);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

// ---- update_status ----

TEST_F(UserControllerTest, DisableUser) {
    auto req = admin_req();
    req->setBody(R"({"status":"disabled"})");
    auto resp = invoke(&UserCtrl::update_status, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["status"].get<std::string>(), "disabled");
}

TEST_F(UserControllerTest, EnableUser) {
    repo_->update_user_status(user_id_, "disabled");
    auto req = admin_req();
    req->setBody(R"({"status":"enabled"})");
    auto resp = invoke(&UserCtrl::update_status, req, user_id_);
    auto body = parse_body(resp);
    EXPECT_EQ(body["status"].get<std::string>(), "offline");
}

TEST_F(UserControllerTest, LockUser) {
    auto req = admin_req();
    req->setBody(R"({"status":"locked"})");
    auto resp = invoke(&UserCtrl::update_status, req, user_id_);
    auto body = parse_body(resp);
    EXPECT_EQ(body["status"].get<std::string>(), "locked");
    EXPECT_GT(body["locked_until"].get<int64_t>(), 0);
}

TEST_F(UserControllerTest, UnlockUser) {
    repo_->lock_user(user_id_, 9999999999);
    repo_->increment_failed_login("regular");
    repo_->increment_failed_login("regular");
    auto req = admin_req();
    req->setBody(R"({"status":"unlocked"})");
    auto resp = invoke(&UserCtrl::update_status, req, user_id_);
    auto body = parse_body(resp);
    EXPECT_EQ(body["status"].get<std::string>(), "offline");
    EXPECT_EQ(body["locked_until"].get<int64_t>(), 0);
    auto u = repo_->get_user_by_id(user_id_);
    EXPECT_EQ(u->failed_login_count, 0);
}

TEST_F(UserControllerTest, InvalidStatusReturns400) {
    auto req = admin_req();
    req->setBody(R"({"status":"bogus"})");
    auto resp = invoke(&UserCtrl::update_status, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(UserControllerTest, UpdateStatusNonexistentReturns404) {
    auto req = admin_req();
    req->setBody(R"({"status":"locked"})");
    auto resp = invoke(&UserCtrl::update_status, req, 99999);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

TEST_F(UserControllerTest, GetPermissionsNonexistentReturns404) {
    auto req = admin_req();
    auto resp = invoke(&UserCtrl::get_permissions, req, 99999);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

TEST_F(UserControllerTest, UpdatePermissionsNonexistentReturns404) {
    auto req = admin_req();
    req->setBody(R"({"permissions":[]})");
    auto resp = invoke(&UserCtrl::update_permissions, req, 99999);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

// ---- reset_password ----

TEST_F(UserControllerTest, ResetPassword) {
    auto req = admin_req();
    req->setBody(R"({"new_password":"newsecret"})");
    auto resp = invoke(&UserCtrl::reset_password, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
}

TEST_F(UserControllerTest, ResetPasswordEmptyReturns400) {
    auto req = admin_req();
    req->setBody(R"({"new_password":""})");
    auto resp = invoke(&UserCtrl::reset_password, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

// 改为非默认密码时不应重置 default_password_acknowledged
TEST_F(UserControllerTest, ResetPasswordNonDefaultDoesNotResetAck) {
    // 1. 先确认默认密码告警
    ASSERT_TRUE(repo_->ack_default_password(user_id_));
    auto u = repo_->get_user_by_id(user_id_);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->default_password_acknowledged, 1);

    // 2. 调用 reset_password 改为强密码
    ASSERT_TRUE(repo_->reset_password(user_id_, "StrongPass123!"));

    // 3. 验证 default_password_acknowledged 仍为 1(未重置)
    u = repo_->get_user_by_id(user_id_);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->default_password_acknowledged, 1);
}

// ---- permissions ----

TEST_F(UserControllerTest, GetPermissionsEmpty) {
    auto req = admin_req();
    auto resp = invoke(&UserCtrl::get_permissions, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_TRUE(body.is_array());
    EXPECT_EQ(body.size(), 0);
}

TEST_F(UserControllerTest, UpdateAndGetPermissions) {
    auto req = admin_req();
    req->setBody(R"({"permissions":[{"type":"account","id":"CTP-001","granted":true},{"type":"strategy","id":"MACD","granted":false}]})");
    auto resp = invoke(&UserCtrl::update_permissions, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body.size(), 2);

    // Now get permissions
    auto get_req = admin_req();
    auto get_resp = invoke(&UserCtrl::get_permissions, get_req, user_id_);
    auto get_body = parse_body(get_resp);
    EXPECT_EQ(get_body.size(), 2);
}

TEST_F(UserControllerTest, UpdatePermissionsNonAdminReturns403) {
    auto req = user_req();
    req->setBody(R"({"permissions":[]})");
    auto resp = invoke(&UserCtrl::update_permissions, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

// ---- JSON excludes password_hash ----

TEST_F(UserControllerTest, UserJsonExcludesPasswordHash) {
    auto req = admin_req();
    auto resp = invoke(&UserCtrl::get, req, user_id_);
    auto body = parse_body(resp);
    EXPECT_FALSE(body.contains("password_hash"));
}

}  // anonymous namespace
}  // namespace dztrader::webui
