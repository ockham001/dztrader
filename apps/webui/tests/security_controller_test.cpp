#include <gtest/gtest.h>
#include "security_controller.h"
#include "repository.h"

#include <nlohmann/json.hpp>
#include <memory>

namespace dztrader::webui {
namespace {


class SecurityControllerTest : public ::testing::Test {
protected:
    std::shared_ptr<Repository> repo_;
    std::shared_ptr<SecurityCtrl> ctrl_;
    int64_t admin_id_ = 0;
    int64_t user_id_ = 0;

    void SetUp() override {
        repo_ = std::make_shared<Repository>(":memory:");
        admin_id_ = repo_->create_user("admin", "Administrator", "admin@test.com", "pass", "admin");
        user_id_ = repo_->create_user("regular", "Regular User", "reg@test.com", "pass", "user");
        ctrl_ = std::make_shared<SecurityCtrl>(repo_);
    }

    drogon::HttpRequestPtr admin_req() {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("admin"));
        return req;
    }
    drogon::HttpRequestPtr user_req() {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("regular"));
        return req;
    }
    template <typename Fn, typename... Args>
    drogon::HttpResponsePtr invoke(Fn fn, const drogon::HttpRequestPtr& req, Args&&... args) {
        drogon::HttpResponsePtr captured;
        (ctrl_.get()->*fn)(req,
                          [&captured](const drogon::HttpResponsePtr& r) { captured = r; },
                          std::forward<Args>(args)...);
        return captured;
    }
    static nlohmann::json parse_body(const drogon::HttpResponsePtr& resp) {
        return nlohmann::json::parse(resp->getBody());
    }
};

// ---- get_config ----

TEST_F(SecurityControllerTest, GetConfigReturns200) {
    auto req = admin_req();
    auto resp = invoke(&SecurityCtrl::get_config, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_TRUE(body.contains("login_lockout_enabled"));
    EXPECT_TRUE(body.contains("access_mode"));
    EXPECT_TRUE(body.contains("max_failed_attempts"));
    EXPECT_TRUE(body.contains("lockout_duration_sec"));
}

TEST_F(SecurityControllerTest, GetConfigAsNonAdminReturns403) {
    auto req = user_req();
    auto resp = invoke(&SecurityCtrl::get_config, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

// ---- set_config ----

TEST_F(SecurityControllerTest, SetConfigReturns200) {
    auto req = admin_req();
    req->setBody(R"({"login_lockout_enabled":false,"access_mode":"whitelist","max_failed_attempts":3,"lockout_duration_sec":600})");
    auto resp = invoke(&SecurityCtrl::set_config, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["access_mode"].get<std::string>(), "whitelist");
    EXPECT_EQ(body["max_failed_attempts"].get<int>(), 3);
    EXPECT_EQ(body["lockout_duration_sec"].get<int>(), 600);
    EXPECT_FALSE(body["login_lockout_enabled"].get<bool>());

    // Verify the change persisted.
    auto get_resp = invoke(&SecurityCtrl::get_config, admin_req());
    auto get_body = parse_body(get_resp);
    EXPECT_EQ(get_body["access_mode"].get<std::string>(), "whitelist");
    EXPECT_EQ(get_body["max_failed_attempts"].get<int>(), 3);
}

TEST_F(SecurityControllerTest, SetConfigAsNonAdminReturns403) {
    auto req = user_req();
    req->setBody(R"({"access_mode":"whitelist"})");
    auto resp = invoke(&SecurityCtrl::set_config, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(SecurityControllerTest, SetConfigInvalidAccessModeReturns400) {
    auto req = admin_req();
    req->setBody(R"({"access_mode":"bogus"})");
    auto resp = invoke(&SecurityCtrl::set_config, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(SecurityControllerTest, SetConfigBadJsonReturns400) {
    auto req = admin_req();
    req->setBody("not nlohmann::json");
    auto resp = invoke(&SecurityCtrl::set_config, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

// ---- blacklist lifecycle ----

TEST_F(SecurityControllerTest, BlacklistLifecycle) {
    // Add
    auto add_req = admin_req();
    add_req->setBody(R"({"ip":"1.2.3.4","reason":"suspected attack"})");
    auto add_resp = invoke(&SecurityCtrl::add_blacklist, add_req);
    EXPECT_EQ(add_resp->getStatusCode(), drogon::k201Created);
    auto added = parse_body(add_resp);
    EXPECT_EQ(added["ip"].get<std::string>(), "1.2.3.4");
    EXPECT_EQ(added["reason"].get<std::string>(), "suspected attack");
    EXPECT_EQ(added["source"].get<std::string>(), "manual");
    const int64_t entry_id = added["id"].get<int64_t>();
    ASSERT_GT(entry_id, 0);

    // List
    auto list_resp = invoke(&SecurityCtrl::list_blacklist, admin_req());
    EXPECT_EQ(list_resp->getStatusCode(), drogon::k200OK);
    auto list_body = parse_body(list_resp);
    EXPECT_TRUE(list_body.is_array());
    EXPECT_EQ(list_body.size(), 1u);
    EXPECT_EQ(list_body[0]["ip"].get<std::string>(), "1.2.3.4");

    // Remove
    auto del_resp = invoke(&SecurityCtrl::remove_blacklist, admin_req(), entry_id);
    EXPECT_EQ(del_resp->getStatusCode(), drogon::k204NoContent);

    // Verify gone
    auto list_after = invoke(&SecurityCtrl::list_blacklist, admin_req());
    auto after_body = parse_body(list_after);
    EXPECT_EQ(after_body.size(), 0u);
}

TEST_F(SecurityControllerTest, AddBlacklistDuplicateReturns409) {
    auto req1 = admin_req();
    req1->setBody(R"({"ip":"5.5.5.5","reason":"first"})");
    auto r1 = invoke(&SecurityCtrl::add_blacklist, req1);
    EXPECT_EQ(r1->getStatusCode(), drogon::k201Created);

    auto req2 = admin_req();
    req2->setBody(R"({"ip":"5.5.5.5","reason":"second"})");
    auto r2 = invoke(&SecurityCtrl::add_blacklist, req2);
    EXPECT_EQ(r2->getStatusCode(), drogon::k409Conflict);
}

TEST_F(SecurityControllerTest, AddBlacklistMissingIpReturns400) {
    auto req = admin_req();
    req->setBody(R"({"reason":"no ip"})");
    auto resp = invoke(&SecurityCtrl::add_blacklist, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(SecurityControllerTest, RemoveBlacklistNonexistentReturns404) {
    auto resp = invoke(&SecurityCtrl::remove_blacklist, admin_req(), 99999);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

// ---- whitelist lifecycle ----

TEST_F(SecurityControllerTest, WhitelistLifecycle) {
    auto add_req = admin_req();
    add_req->setBody(R"({"ip":"10.0.0.1","reason":"trusted"})");
    auto add_resp = invoke(&SecurityCtrl::add_whitelist, add_req);
    EXPECT_EQ(add_resp->getStatusCode(), drogon::k201Created);
    auto added = parse_body(add_resp);
    EXPECT_EQ(added["ip"].get<std::string>(), "10.0.0.1");
    const int64_t entry_id = added["id"].get<int64_t>();
    ASSERT_GT(entry_id, 0);

    auto list_resp = invoke(&SecurityCtrl::list_whitelist, admin_req());
    EXPECT_EQ(list_resp->getStatusCode(), drogon::k200OK);
    auto list_body = parse_body(list_resp);
    EXPECT_EQ(list_body.size(), 1u);

    auto del_resp = invoke(&SecurityCtrl::remove_whitelist, admin_req(), entry_id);
    EXPECT_EQ(del_resp->getStatusCode(), drogon::k204NoContent);

    auto list_after = invoke(&SecurityCtrl::list_whitelist, admin_req());
    auto after_body = parse_body(list_after);
    EXPECT_EQ(after_body.size(), 0u);
}

TEST_F(SecurityControllerTest, AddWhitelistDuplicateReturns409) {
    auto req1 = admin_req();
    req1->setBody(R"({"ip":"10.0.0.2","reason":"a"})");
    invoke(&SecurityCtrl::add_whitelist, req1);

    auto req2 = admin_req();
    req2->setBody(R"({"ip":"10.0.0.2","reason":"b"})");
    auto r2 = invoke(&SecurityCtrl::add_whitelist, req2);
    EXPECT_EQ(r2->getStatusCode(), drogon::k409Conflict);
}

TEST_F(SecurityControllerTest, RemoveWhitelistNonexistentReturns404) {
    auto resp = invoke(&SecurityCtrl::remove_whitelist, admin_req(), 99999);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

// ---- login history ----

TEST_F(SecurityControllerTest, LoginHistoryReturnsPaginated) {
    // Add some history entries.
    for (int i = 0; i < 5; ++i) {
        repo_->add_login_history("admin", "1.2.3." + std::to_string(i), "Mozilla", true);
    }
    for (int i = 0; i < 3; ++i) {
        repo_->add_login_history("regular", "5.6.7." + std::to_string(i), "curl", false);
    }

    // Page 1, page_size 4 → 4 entries, total 8
    auto req = admin_req();
    req->setParameter("page", "1");
    req->setParameter("page_size", "4");
    auto resp = invoke(&SecurityCtrl::login_history, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["total"].get<int>(), 8);
    EXPECT_EQ(body["entries"].size(), 4u);
    EXPECT_TRUE(body["entries"][0].contains("username"));
    EXPECT_TRUE(body["entries"][0].contains("success"));

    // Page 2, page_size 4 → 4 entries, total 8
    auto req2 = admin_req();
    req2->setParameter("page", "2");
    req2->setParameter("page_size", "4");
    auto resp2 = invoke(&SecurityCtrl::login_history, req2);
    auto body2 = parse_body(resp2);
    EXPECT_EQ(body2["total"].get<int>(), 8);
    EXPECT_EQ(body2["entries"].size(), 4u);

    // Page 3, page_size 4 → 0 entries (only 8 total)
    auto req3 = admin_req();
    req3->setParameter("page", "3");
    req3->setParameter("page_size", "4");
    auto resp3 = invoke(&SecurityCtrl::login_history, req3);
    auto body3 = parse_body(resp3);
    EXPECT_EQ(body3["total"].get<int>(), 8);
    EXPECT_EQ(body3["entries"].size(), 0u);
}

TEST_F(SecurityControllerTest, LoginHistoryAsNonAdminReturns403) {
    auto req = user_req();
    auto resp = invoke(&SecurityCtrl::login_history, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

// ---- All mutating endpoints as non-admin return 403 ----

TEST_F(SecurityControllerTest, AllEndpointsAsNonAdminReturn403) {
    auto req = user_req();

    auto set_cfg = invoke(&SecurityCtrl::set_config, req);
    EXPECT_EQ(set_cfg->getStatusCode(), drogon::k403Forbidden);

    auto list_bl = invoke(&SecurityCtrl::list_blacklist, req);
    EXPECT_EQ(list_bl->getStatusCode(), drogon::k403Forbidden);

    req->setBody(R"({"ip":"1.2.3.4"})");
    auto add_bl = invoke(&SecurityCtrl::add_blacklist, req);
    EXPECT_EQ(add_bl->getStatusCode(), drogon::k403Forbidden);

    auto del_bl = invoke(&SecurityCtrl::remove_blacklist, req, int64_t{1});
    EXPECT_EQ(del_bl->getStatusCode(), drogon::k403Forbidden);

    auto list_wl = invoke(&SecurityCtrl::list_whitelist, req);
    EXPECT_EQ(list_wl->getStatusCode(), drogon::k403Forbidden);

    auto add_wl = invoke(&SecurityCtrl::add_whitelist, req);
    EXPECT_EQ(add_wl->getStatusCode(), drogon::k403Forbidden);

    auto del_wl = invoke(&SecurityCtrl::remove_whitelist, req, int64_t{1});
    EXPECT_EQ(del_wl->getStatusCode(), drogon::k403Forbidden);

    auto hist = invoke(&SecurityCtrl::login_history, req);
    EXPECT_EQ(hist->getStatusCode(), drogon::k403Forbidden);

    auto get_cfg = invoke(&SecurityCtrl::get_config, req);
    EXPECT_EQ(get_cfg->getStatusCode(), drogon::k403Forbidden);
}

}  // anonymous namespace
}  // namespace dztrader::webui
