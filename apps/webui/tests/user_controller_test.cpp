#include <gtest/gtest.h>
#include "user_controller.h"
#include "repository.h"
#include "ws_controller.h"
#include "fake_data_change_notifier.h"

#include <nlohmann/json.hpp>
#include <memory>

namespace dztrader::webui {
namespace {

class UserControllerTest : public ::testing::Test {
protected:
    std::shared_ptr<Repository> repo_;
    WebuiConfig cfg_;
    std::shared_ptr<FakeDataChangeNotifier> notifier_;
    std::shared_ptr<UserCtrl> ctrl_;
    int64_t admin_id_ = 0;
    int64_t user_id_ = 0;

    void SetUp() override {
        repo_ = std::make_shared<Repository>(":memory:");
        notifier_ = std::make_shared<FakeDataChangeNotifier>();
        admin_id_ = repo_->create_user("admin", "Administrator", "admin@test.com", "pass", "admin");
        user_id_ = repo_->create_user("regular", "Regular User", "reg@test.com", "pass", "user");
        ctrl_ = std::make_shared<UserCtrl>(repo_, cfg_, *notifier_);
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
        (ctrl_.get()->*fn)(
            req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; },
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
    req->setBody(
        R"({"username":"newuser","display_name":"New","email":"n@t.com","password":"pw","role":"user"})");
    auto resp = invoke(&UserCtrl::create, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k201Created);
    auto body = parse_body(resp);
    EXPECT_EQ(body["username"].get<std::string>(), "newuser");
    EXPECT_EQ(body["role"].get<std::string>(), "user");
    EXPECT_EQ(body["status"].get<std::string>(), "offline");
    // 契约 rest §1: 变更 DB 成功后广播 data_changed{users}
    ASSERT_EQ(notifier_->scopes.size(), 1u);
    EXPECT_EQ(notifier_->scopes[0], "users");
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
    EXPECT_EQ(parse_body(resp)["error"].get<std::string>(), "not found");
}

// ---- me（角色降级后前端刷新缓存，与后端踢降级用户 WS 连接配套）----

TEST_F(UserControllerTest, MeReturnsOwnInfoForNonAdmin) {
    // 任意已认证用户（非 admin）可访问自身信息，无需 admin
    auto req = user_req();  // attribute user_id = "regular"
    auto resp = invoke(&UserCtrl::me, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["username"].get<std::string>(), "regular");
    EXPECT_EQ(body["role"].get<std::string>(), "user");
}

TEST_F(UserControllerTest, MeReturnsLatestRoleAfterDemotion) {
    // 模拟角色降级：regular 从 user 升为 admin，前端 me 能取到最新角色
    auto admin_req_ = admin_req();
    admin_req_->setBody(R"({"display_name":"Regular User","email":"reg@test.com","role":"admin"})");
    auto resp = invoke(&UserCtrl::update, admin_req_, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    // 以 regular 身份再查 me：应返回最新 role=admin（DB 真相源）
    auto req = user_req();
    auto mine = invoke(&UserCtrl::me, req);
    auto body = parse_body(mine);
    EXPECT_EQ(body["role"].get<std::string>(), "admin");
}

TEST_F(UserControllerTest, MeWithoutAuthContextReturns403) {
    // 无 user_id attribute（未注入 JWT 身份）→ 403
    auto req = drogon::HttpRequest::newHttpRequest();
    auto resp = invoke(&UserCtrl::me, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
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
    ASSERT_EQ(notifier_->scopes.size(), 1u);
    EXPECT_EQ(notifier_->scopes[0], "users");
}

TEST_F(UserControllerTest, UpdateNonexistentReturns404) {
    auto req = admin_req();
    req->setBody(R"({"display_name":"x"})");
    auto resp = invoke(&UserCtrl::update, req, 99999);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
    EXPECT_EQ(parse_body(resp)["error"].get<std::string>(), "not found");
}

// ---- 角色降级踢连接（契约 webui-ws §2.3：降低权限后须强制断开该用户已建立的 WS 连接）----

TEST_F(UserControllerTest, UpdateDemotingAdminToUserKicksUser) {
    // 目标为 admin（username "admin"）；降级为 user 后应触发踢人
    auto req = admin_req();
    req->setBody(R"({"display_name":"Administrator","email":"admin@test.com","role":"user"})");
    auto resp = invoke(&UserCtrl::update, req, admin_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    ASSERT_EQ(notifier_->kicked.size(), 1u);
    EXPECT_EQ(notifier_->kicked[0], "admin");
}

TEST_F(UserControllerTest, UpdateKeepingNonAdminRoleDoesNotKick) {
    // 普通用户改 display_name 但保持 user 角色：权限未变，不应踢
    auto req = admin_req();
    req->setBody(R"({"display_name":"Renamed","email":"reg@test.com","role":"user"})");
    auto resp = invoke(&UserCtrl::update, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_TRUE(notifier_->kicked.empty());
}

TEST_F(UserControllerTest, UpdateKeepingAdminRoleDoesNotKick) {
    // 目标为 admin 且保持 admin 角色：权限未降级，不应误踢
    auto req = admin_req();
    req->setBody(R"({"display_name":"Administrator","email":"admin@test.com","role":"admin"})");
    auto resp = invoke(&UserCtrl::update, req, admin_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_TRUE(notifier_->kicked.empty());
}

// ---- remove ----

TEST_F(UserControllerTest, RemoveUserReturns204) {
    auto req = admin_req();
    auto resp = invoke(&UserCtrl::remove, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k204NoContent);
    // Verify gone
    EXPECT_FALSE(repo_->get_user_by_id(user_id_).has_value());
    // 契约 rest §2.2: 删后强制断开其 WS 连接并广播 users
    ASSERT_EQ(notifier_->kicked.size(), 1u);
    EXPECT_EQ(notifier_->kicked[0], "regular");
    ASSERT_EQ(notifier_->scopes.size(), 1u);
    EXPECT_EQ(notifier_->scopes[0], "users");
}

TEST_F(UserControllerTest, RemoveSelfReturns403) {
    auto resp = invoke(&UserCtrl::remove, admin_req(), admin_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
    EXPECT_EQ(parse_body(resp)["error"].get<std::string>(), "cannot delete yourself");
    EXPECT_TRUE(repo_->get_user_by_id(admin_id_).has_value());
}

TEST_F(UserControllerTest, RemoveOtherAdminReturns403) {
    const int64_t second_admin = repo_->create_user("admin2", "Admin 2", "", "pw", "admin");
    auto resp = invoke(&UserCtrl::remove, admin_req(), second_admin);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
    EXPECT_EQ(parse_body(resp)["error"].get<std::string>(), "cannot delete admin user");
    EXPECT_TRUE(repo_->get_user_by_id(second_admin).has_value());
    EXPECT_TRUE(notifier_->kicked.empty());
}

TEST_F(UserControllerTest, RemoveNonexistentReturns404) {
    auto req = admin_req();
    auto resp = invoke(&UserCtrl::remove, req, 99999);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
    EXPECT_EQ(parse_body(resp)["error"].get<std::string>(), "not found");
}

// ---- update_status ----

TEST_F(UserControllerTest, DisableUser) {
    auto req = admin_req();
    req->setBody(R"({"status":"disabled"})");
    auto resp = invoke(&UserCtrl::update_status, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["status"].get<std::string>(), "disabled");
    // 契约 rest §2.2: 禁用时强制断开其 WS 连接
    ASSERT_EQ(notifier_->kicked.size(), 1u);
    EXPECT_EQ(notifier_->kicked[0], "regular");
    ASSERT_EQ(notifier_->scopes.size(), 1u);
    EXPECT_EQ(notifier_->scopes[0], "users");
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
    EXPECT_EQ(parse_body(resp)["error"].get<std::string>(), "not found");
}

TEST_F(UserControllerTest, UpdateStatusAdminDisableOrLockReturns403) {
    // 契约防误操作: 禁用/锁定管理员 403（状态不变）
    for (const char* body : {R"({"status":"disabled"})", R"({"status":"locked"})"}) {
        auto req = admin_req();
        req->setBody(body);
        auto resp = invoke(&UserCtrl::update_status, req, admin_id_);
        EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden) << body;
        EXPECT_EQ(parse_body(resp)["error"].get<std::string>(), "cannot disable or lock admin user")
            << body;
    }
    auto admin = repo_->get_user_by_id(admin_id_);
    ASSERT_TRUE(admin.has_value());
    EXPECT_EQ(admin->status, "offline");
    EXPECT_TRUE(notifier_->kicked.empty());
}

TEST_F(UserControllerTest, NonAdminForbiddenEndpoints) {
    // list / get(other) / update / update_status / reset_password 均要求 admin
    auto list = invoke(&UserCtrl::list, user_req());
    EXPECT_EQ(list->getStatusCode(), drogon::k403Forbidden);

    auto get_other = invoke(&UserCtrl::get, user_req(), admin_id_);
    EXPECT_EQ(get_other->getStatusCode(), drogon::k403Forbidden);

    auto upd = user_req();
    upd->setBody(R"({"display_name":"x"})");
    EXPECT_EQ(invoke(&UserCtrl::update, upd, user_id_)->getStatusCode(), drogon::k403Forbidden);

    auto st = user_req();
    st->setBody(R"({"status":"disabled"})");
    EXPECT_EQ(invoke(&UserCtrl::update_status, st, user_id_)->getStatusCode(),
              drogon::k403Forbidden);

    auto pw = user_req();
    pw->setBody(R"({"new_password":"x"})");
    EXPECT_EQ(invoke(&UserCtrl::reset_password, pw, user_id_)->getStatusCode(),
              drogon::k403Forbidden);
}

// ---- reset_password ----

TEST_F(UserControllerTest, ResetPassword) {
    auto req = admin_req();
    req->setBody(R"({"new_password":"newsecret"})");
    auto resp = invoke(&UserCtrl::reset_password, req, user_id_);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    // 断言新密码生效、旧密码失效（控制器未真正改密但返回 200 时必红）
    auto hash = repo_->get_password_hash("regular");
    ASSERT_TRUE(hash.has_value());
    EXPECT_TRUE(verify_password("newsecret", *hash));
    EXPECT_FALSE(verify_password("pass", *hash));
}

TEST_F(UserControllerTest, ResetPasswordNonexistentReturns404) {
    auto req = admin_req();
    req->setBody(R"({"new_password":"x"})");
    auto resp = invoke(&UserCtrl::reset_password, req, 99999);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
    EXPECT_EQ(parse_body(resp)["error"].get<std::string>(), "not found");
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

    // 2. 经控制器改为强密码（必须走 UserCtrl 而非直接调仓库）
    auto req = admin_req();
    req->setBody(R"({"new_password":"StrongPass123!"})");
    auto resp = invoke(&UserCtrl::reset_password, req, user_id_);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    // 3. 验证 default_password_acknowledged 仍为 1(未重置)
    u = repo_->get_user_by_id(user_id_);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->default_password_acknowledged, 1);
    auto hash = repo_->get_password_hash("regular");
    ASSERT_TRUE(hash.has_value());
    EXPECT_TRUE(verify_password("StrongPass123!", *hash));
}

// 改回默认密码时应重置 ack（会再次触发默认密码告警）
TEST_F(UserControllerTest, ResetPasswordToDefaultResetsAck) {
    ASSERT_TRUE(repo_->ack_default_password(user_id_));
    auto req = admin_req();
    req->setBody(std::string(R"({"new_password":")") + kDefaultAdminPassword + R"("})");
    auto resp = invoke(&UserCtrl::reset_password, req, user_id_);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    auto u = repo_->get_user_by_id(user_id_);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->default_password_acknowledged, 0);
    auto hash = repo_->get_password_hash("regular");
    ASSERT_TRUE(hash.has_value());
    EXPECT_TRUE(verify_password(kDefaultAdminPassword, *hash));
}

// ---- 非法 JSON 统一 400（禁止解析异常逃逸）----

TEST_F(UserControllerTest, MutatingEndpointsRejectInvalidJson) {
    auto create = admin_req();
    create->setBody("{not json");
    EXPECT_EQ(invoke(&UserCtrl::create, create)->getStatusCode(), drogon::k400BadRequest);

    auto update = admin_req();
    update->setBody("{not json");
    EXPECT_EQ(invoke(&UserCtrl::update, update, user_id_)->getStatusCode(), drogon::k400BadRequest);

    auto status = admin_req();
    status->setBody("{not json");
    EXPECT_EQ(invoke(&UserCtrl::update_status, status, user_id_)->getStatusCode(),
              drogon::k400BadRequest);

    auto reset = admin_req();
    reset->setBody("{not json");
    EXPECT_EQ(invoke(&UserCtrl::reset_password, reset, user_id_)->getStatusCode(),
              drogon::k400BadRequest);
}

// ---- POST /api/auth/change-password（契约 rest §2.1：任意已认证用户）----

TEST_F(UserControllerTest, ChangePasswordWithoutAuthContextReturns401) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setBody(R"({"new_password":"newpw"})");
    auto resp = invoke(&UserCtrl::change_password, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k401Unauthorized);
}

TEST_F(UserControllerTest, ChangePasswordUnknownUserReturns404) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->getAttributes()->insert("user_id", std::string("ghost"));
    req->setBody(R"({"new_password":"newpw"})");
    auto resp = invoke(&UserCtrl::change_password, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
    EXPECT_EQ(parse_body(resp)["error"].get<std::string>(), "not found");
}

TEST_F(UserControllerTest, ChangePasswordInvalidJsonReturns400) {
    auto req = user_req();
    req->setBody("{not json");
    auto resp = invoke(&UserCtrl::change_password, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(UserControllerTest, ChangePasswordEmptyReturns400) {
    auto req = user_req();
    req->setBody(R"({"new_password":""})");
    auto resp = invoke(&UserCtrl::change_password, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(UserControllerTest, ChangePasswordSucceedsAndTakesEffect) {
    auto req = user_req();
    req->setBody(R"({"new_password":"MyNewPass!"})");
    auto resp = invoke(&UserCtrl::change_password, req);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    // 新密码生效、旧密码失效
    auto hash = repo_->get_password_hash("regular");
    ASSERT_TRUE(hash.has_value());
    EXPECT_TRUE(verify_password("MyNewPass!", *hash));
    EXPECT_FALSE(verify_password("pass", *hash));
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
