#include <gtest/gtest.h>
#include "market_source_controller.h"
#include "repository.h"
#include "process_mirror.h"
#include "ws_controller.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <dztrader/data_type.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>

namespace dztrader::webui {
namespace {


class MarketSourceControllerTest : public ::testing::Test {
protected:
    std::shared_ptr<Repository> repo_;
    std::shared_ptr<ShmWriter> shm_writer_;
    std::shared_ptr<ProcessMirror> process_mirror_;
    std::shared_ptr<MarketSourceCtrl> ctrl_;
    int64_t admin_id_ = 0;
    int64_t user_id_ = 0;

    void SetUp() override {
        repo_ = std::make_shared<Repository>(":memory:");
        admin_id_ = repo_->create_user("admin", "Administrator", "admin@test.com", "pass", "admin");
        user_id_ = repo_->create_user("regular", "Regular User", "reg@test.com", "pass", "user");
        // ShmWriter will fail to open the dz_shm_event channel — is_ready() returns false.
        // This is expected: the controller treats SHM writes as best-effort.
        shm_writer_ = std::make_shared<ShmWriter>(std::filesystem::temp_directory_path());
        // ProcessMirror: 真实实例, 测试中镜像始终为空 (无 dzmd_ctp 上报)
        // 触发 is_process_running_in_mirror 返回 false, write 路径返回 503
        process_mirror_ = std::make_shared<ProcessMirror>();
        ctrl_ = std::make_shared<MarketSourceCtrl>(repo_, shm_writer_, process_mirror_);
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

    /// Helper to create a market source via the controller and return its id.
    int64_t create_source(const std::string& name = "ctp_main") {
        auto req = admin_req();
        req->setBody(R"({"source_type":"ctp","source_name":")" + name +
                     R"(","display_name":"CTP Main"})");
        auto resp = invoke(&MarketSourceCtrl::create, req);
        EXPECT_EQ(resp->getStatusCode(), drogon::k201Created);
        return parse_body(resp)["id"].get<int64_t>();
    }
};

/// Helper: 填充 process_mirror_ 的 status 使 is_process_running_in_mirror 返回 true.
/// Phase 2b 起 dispatch_op 不再读镜像 detail/config (只检查进程 running 状态),
/// 故仅需 update_status; detail 填充由具体测试自行准备 (如 get_config 脱敏测试).
void prime_mirror_for_dispatch(ProcessMirror& mirror, const std::string& process_name) {
    dztrader::platform::ProcessStatus s;
    s.name = process_name;
    s.state = dztrader::platform::ChildState::Running;
    mirror.update_status(process_name, s);
}

// ---- create + get ----

TEST_F(MarketSourceControllerTest, CreateAndGetMarketSource) {
    const int64_t id = create_source("ctp_main");

    // GET detail: Wave 2C 后不再返回 credentials 字段 (凭证存于子进程配置文件)
    // 排程/自动登录走契约 auto-login (auto_login 帧镜像): detail 无 schedules 字段,
    // auto_login 为占位 false (镜像未就绪时)
    auto get_req = admin_req();
    auto get_resp = invoke(&MarketSourceCtrl::get, get_req, id);
    EXPECT_EQ(get_resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(get_resp);
    EXPECT_EQ(body["id"].get<int64_t>(), id);
    EXPECT_EQ(body["source_type"].get<std::string>(), "ctp");
    EXPECT_EQ(body["source_name"].get<std::string>(), "ctp_main");
    EXPECT_EQ(body["display_name"].get<std::string>(), "CTP Main");
    // source_name="ctp_main" 不以 dzmd_/dztd_ 开头, ui_card 为空字符串 (契约 md-config)
    EXPECT_EQ(body["ui_card"].get<std::string>(), "");
    EXPECT_FALSE(body.contains("credentials"));
    EXPECT_FALSE(body.contains("schedules"));  // 契约 auto-login: 排程已迁移, 不再在 detail 返回
    EXPECT_EQ(body["auto_login"].get<bool>(), false);
}

TEST_F(MarketSourceControllerTest, CreateAsNonAdminReturns403) {
    auto req = user_req();
    req->setBody(R"({"source_type":"ctp","source_name":"x","display_name":"X"})");
    auto resp = invoke(&MarketSourceCtrl::create, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, CreateMissingFieldsReturns400) {
    auto req = admin_req();
    req->setBody(R"({"source_type":"ctp"})");
    auto resp = invoke(&MarketSourceCtrl::create, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

// ---- list ----

TEST_F(MarketSourceControllerTest, ListMarketSources) {
    // DB 真相源 (契约 rest §3): list 返回 DB 中 is_added=1 的 dzmd_* 行,
    // 运行状态由 WS process_status 表达, 与镜像是否上报无关
    // 1. DB 记录的 source_name 必须以 dzmd_ 开头
    create_source("dzmd_ctp_a");
    create_source("dzmd_ctp_b");
    // 2. 填充镜像 (模拟 PROCESS_STATUS 帧到达)
    dztrader::platform::ProcessStatus s_a;
    s_a.name = "dzmd_ctp_a";
    s_a.state = dztrader::platform::ChildState::Running;
    process_mirror_->update_status("dzmd_ctp_a", s_a);
    dztrader::platform::ProcessStatus s_b;
    s_b.name = "dzmd_ctp_b";
    s_b.state = dztrader::platform::ChildState::Running;
    process_mirror_->update_status("dzmd_ctp_b", s_b);
    // 3. 调用 list, 应返回 2 项
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::list, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_TRUE(body.is_array());
    EXPECT_EQ(body.size(), 2u);
    // List items must NOT include credentials/schedules (just the basic shape).
    EXPECT_FALSE(body[0].contains("credentials"));
    EXPECT_FALSE(body[0].contains("schedules"));
    // 契约 md-config: ui_card 由 source_name 经 extract_ui_card 计算
    // dzmd_ctp_a / dzmd_ctp_b → tail="ctp_a"/"ctp_b" → 第一个 _ 之前为 "ctp"
    for (const auto& item : body) {
        EXPECT_EQ(item["ui_card"].get<std::string>(), "ctp")
            << "ui_card should be 'ctp' for dzmd_ctp_* process names";
    }
}

// ---- list: DB 真相源（契约 rest §3），不依赖镜像 Running ----

TEST_F(MarketSourceControllerTest, ListReturnsDbRowsRegardlessOfProcessState) {
    create_source("dzmd_list1");  // source_name 必须为 dzmd_* 前缀
    create_source("dzmd_list2");

    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::list, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    // 镜像为空（进程未运行）也返回 DB 行--修复前按镜像 running 过滤会返回空
    EXPECT_EQ(body.size(), 2u);
}

TEST_F(MarketSourceControllerTest, ListExcludesNotAddedRows) {
    const int64_t id = create_source("dzmd_list3");
    EXPECT_TRUE(repo_->set_market_source_added(id, false));

    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::list, req);
    auto body = parse_body(resp);
    EXPECT_EQ(body.size(), 0u);
}

TEST_F(MarketSourceControllerTest, ListAsNonAdminReturns200) {
    // Read endpoints only require JWT.
    create_source("ctp_a");
    auto req = user_req();
    auto resp = invoke(&MarketSourceCtrl::list, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
}

// ---- get nonexistent ----

TEST_F(MarketSourceControllerTest, GetNonexistentMarketSourceReturns404) {
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::get, req, int64_t{99999});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

// ---- update ----

TEST_F(MarketSourceControllerTest, UpdateMarketSource) {
    const int64_t id = create_source("ctp_upd");
    auto req = admin_req();
    req->setBody(R"({"display_name":"Updated"})");
    auto resp = invoke(&MarketSourceCtrl::update, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["display_name"].get<std::string>(), "Updated");
}

TEST_F(MarketSourceControllerTest, UpdateAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_upd2");
    auto req = user_req();
    req->setBody(R"({"display_name":"X"})");
    auto resp = invoke(&MarketSourceCtrl::update, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, UpdateNonexistentReturns404) {
    auto req = admin_req();
    req->setBody(R"({"display_name":"X"})");
    auto resp = invoke(&MarketSourceCtrl::update, req, int64_t{99999});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

// ---- delete ----

TEST_F(MarketSourceControllerTest, DeleteReturns503WhenShmWriteFails) {
    // 契约 rest §1: 写入事件通道失败 = 端点唯一职责失败 -> 503（不再是 200 {ok:false}）
    // 测试环境 writer 未就绪（temp 目录无通道），write_process_control 返回 false
    const int64_t id = create_source("ctp_del");
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::remove, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
    // 写入失败不得标记移除：DB 行保持 is_added=1
    EXPECT_TRUE(repo_->get_market_source(id).has_value());
    EXPECT_TRUE(repo_->get_market_source(id)->is_added);
}

TEST_F(MarketSourceControllerTest, DeleteAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_del2");
    auto req = user_req();
    auto resp = invoke(&MarketSourceCtrl::remove, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, DeleteNonexistentReturns404) {
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::remove, req, int64_t{99999});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

// ---- login (Wave 2C: SHM md_connect only, no DB write) ----

TEST_F(MarketSourceControllerTest, LoginRejectsShmWhenProcessNotRunning) {
    // Wave 2C: login 不再持久化 credentials (凭证存于子进程配置文件 broker entry)
    // 仅检查 dzmd_ctp 是否在镜像中 running, 是则发 md_connect, 否则返回 503
    // 测试环境中 dzmd_ctp 未运行, 返回 503 (无法发送 md_connect)
    const int64_t id = create_source("ctp_login");

    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::login, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

TEST_F(MarketSourceControllerTest, LoginAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_login2");
    auto req = user_req();
    auto resp = invoke(&MarketSourceCtrl::login, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, LoginNonexistentReturns404) {
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::login, req, int64_t{99999});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

// ---- logout ----

TEST_F(MarketSourceControllerTest, LogoutRejectsWhenProcessNotRunning) {
    // 镜像驱动架构: logout 需要 dzmd_ctp 在镜像中 running 才能 SHM 下发 md_disconnect
    // 测试环境中 dzmd_ctp 未运行, 返回 503
    const int64_t id = create_source("ctp_logout");
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::logout, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

TEST_F(MarketSourceControllerTest, LogoutAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_logout2");
    auto req = user_req();
    auto resp = invoke(&MarketSourceCtrl::logout, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, LogoutNonexistentReturns404) {
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::logout, req, int64_t{99999});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

// ---- auto-login toggle ----
// (403/404 paths + dispatch path coverage via prime_mirror_for_dispatch)

TEST_F(MarketSourceControllerTest, ToggleAutoLoginAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_auto2");
    auto req = user_req();
    req->setBody(R"({"enabled":true})");
    auto resp = invoke(&MarketSourceCtrl::set_auto_login, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, ToggleAutoLoginNonexistentReturns404) {
    auto req = admin_req();
    req->setBody(R"({"enabled":true})");
    auto resp = invoke(&MarketSourceCtrl::set_auto_login, req, int64_t{99999});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

TEST_F(MarketSourceControllerTest, ToggleAutoLoginRejectsNonBooleanEnabled) {
    // nlohmann::json::value("enabled", false) would throw type_error if "enabled"
    // is present but not a boolean. Controller must explicitly type-check and
    // return 400 instead of letting the exception escape to drogon (500).
    const int64_t id = create_source("ctp_auto3");
    auto req = admin_req();
    req->setBody(R"({"enabled":"true"})");  // string instead of bool
    auto resp = invoke(&MarketSourceCtrl::set_auto_login, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(MarketSourceControllerTest, ToggleAutoLoginRejectsNumericEnabled) {
    // Same as above but with numeric "enabled" — must also return 400.
    const int64_t id = create_source("ctp_auto4");
    auto req = admin_req();
    req->setBody(R"({"enabled":1})");
    auto resp = invoke(&MarketSourceCtrl::set_auto_login, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(MarketSourceControllerTest, ToggleAutoLoginRejectsWhenProcessNotRunning) {
    // Mirror is empty (no dzmd_ctp status): dispatch_op must reject
    // with 503 before attempting SHM write.
    const int64_t id = create_source("ctp_auto5");
    auto req = admin_req();
    req->setBody(R"({"enabled":true})");
    auto resp = invoke(&MarketSourceCtrl::set_auto_login, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

TEST_F(MarketSourceControllerTest, ToggleAutoLoginReturns503WhenWriterNotReady) {
    // mirror primed 但 writer 未就绪: guard_process_dispatch 的 is_ready 检查 -> 503
    // （此前 200 "dispatched" 是写失败伪装成功--契约 rest §1 修复项）
    const int64_t id = create_source("ctp_auto6");
    prime_mirror_for_dispatch(*process_mirror_, "dzmd_ctp");

    auto req = admin_req();
    req->setBody(R"({"enabled":true})");
    auto resp = invoke(&MarketSourceCtrl::set_auto_login, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

// ---- auto-login / schedule (契约 auto-login: 全量 {enabled, schedules} 直发 SET_AUTO_LOGIN) ----

TEST_F(MarketSourceControllerTest, SetAutoLoginAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_sched2");
    auto req = user_req();
    req->setBody(R"({"enabled":true,"schedules":[{"login_time":"09:00","logout_time":"15:30"}]})");
    auto resp = invoke(&MarketSourceCtrl::set_auto_login, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, SetAutoLoginNonexistentSourceReturns404) {
    auto req = admin_req();
    req->setBody(R"({"enabled":true,"schedules":[]})");
    auto resp = invoke(&MarketSourceCtrl::set_auto_login, req, int64_t{99999});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

TEST_F(MarketSourceControllerTest, SetAutoLoginRejectsWhenProcessNotRunning) {
    // 镜像驱动架构: SET_AUTO_LOGIN 定向到 dzmd_ctp, 测试环境中进程未运行,
    // is_process_running_in_mirror 返回 false → 503
    const int64_t id = create_source("ctp_sched3");
    auto req = admin_req();
    req->setBody(R"({"enabled":true,"schedules":[]})");
    auto resp = invoke(&MarketSourceCtrl::set_auto_login, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

TEST_F(MarketSourceControllerTest, SetAutoLoginInvalidBodyReturns400) {
    const int64_t id = create_source("ctp_sched4");
    auto req = admin_req();
    req->setBody(R"({"enabled":"not_bool"})");
    auto resp = invoke(&MarketSourceCtrl::set_auto_login, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    auto req2 = admin_req();
    req2->setBody(R"({"schedules":"not_array"})");
    auto resp2 = invoke(&MarketSourceCtrl::set_auto_login, req2, id);
    EXPECT_EQ(resp2->getStatusCode(), drogon::k400BadRequest);
}

// ---- broker CRUD (Wave 2C: SHM dispatch, no DB persistence) ----
//
// 镜像驱动架构: broker CRUD 走 dispatch_op + write_md_set_config (op-based, Phase 2b)
// 测试中镜像为空 (无 dzmd_ctp 上报), is_process_running_in_mirror 返回 false,
// 触发 503 路径. happy path 测试通过填充镜像 status 验证 dispatched 响应.

// ---- add_broker ----

TEST_F(MarketSourceControllerTest, AddBrokerAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_add1");
    auto req = user_req();
    req->setBody(R"({"name":"b1","broker_id":"B1","user_id":"u","password":"p","product_info":"pi"})");
    auto resp = invoke(&MarketSourceCtrl::add_broker, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, AddBrokerMissingNameReturns400) {
    const int64_t id = create_source("ctp_add2");
    auto req = admin_req();
    req->setBody(R"({"broker_id":"B1"})");
    auto resp = invoke(&MarketSourceCtrl::add_broker, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(MarketSourceControllerTest, AddBrokerNonexistentSourceReturns404) {
    auto req = admin_req();
    req->setBody(R"({"name":"b1"})");
    auto resp = invoke(&MarketSourceCtrl::add_broker, req, int64_t{99999});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

TEST_F(MarketSourceControllerTest, AddBrokerRejectsWhenProcessNotRunning) {
    const int64_t id = create_source("ctp_add3");
    auto req = admin_req();
    req->setBody(R"({"name":"b1","broker_id":"B1","user_id":"u","password":"p","product_info":"pi"})");
    auto resp = invoke(&MarketSourceCtrl::add_broker, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

TEST_F(MarketSourceControllerTest, AddBrokerReturns503WhenWriterNotReady) {
    // 契约 rest §1（修订）: guard_process_dispatch 的 is_ready 检查在 write_md_set_config
    // 之前--镜像就绪但 writer 未就绪时 503，不再 200 "dispatched"（写失败伪装成功）
    const int64_t id = create_source("ctp_add4");
    prime_mirror_for_dispatch(*process_mirror_, "dzmd_ctp");

    auto req = admin_req();
    req->setBody(R"({"name":"b1","broker_id":"B1","user_id":"u","password":"p","product_info":"pi"})");
    auto resp = invoke(&MarketSourceCtrl::add_broker, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

// ---- remove_broker ----

TEST_F(MarketSourceControllerTest, RemoveBrokerAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_rm1");
    auto req = user_req();
    auto resp = invoke(&MarketSourceCtrl::remove_broker, req, id, std::string{"b1"});
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, RemoveBrokerNonexistentSourceReturns404) {
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::remove_broker, req, int64_t{99999}, std::string{"b1"});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

TEST_F(MarketSourceControllerTest, RemoveBrokerRejectsWhenProcessNotRunning) {
    const int64_t id = create_source("ctp_rm2");
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::remove_broker, req, id, std::string{"b1"});
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

// ---- update_broker ----

TEST_F(MarketSourceControllerTest, UpdateBrokerAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_upd1");
    auto req = user_req();
    req->setBody(R"({"name":"b1","broker_id":"B1"})");
    auto resp = invoke(&MarketSourceCtrl::update_broker, req, id, std::string{"b1"});
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, UpdateBrokerNameMismatchReturns400) {
    const int64_t id = create_source("ctp_upd2");
    auto req = admin_req();
    req->setBody(R"({"name":"other","broker_id":"B1"})");
    auto resp = invoke(&MarketSourceCtrl::update_broker, req, id, std::string{"b1"});
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(MarketSourceControllerTest, UpdateBrokerNonexistentSourceReturns404) {
    auto req = admin_req();
    req->setBody(R"({"name":"b1"})");
    auto resp = invoke(&MarketSourceCtrl::update_broker, req, int64_t{99999}, std::string{"b1"});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

TEST_F(MarketSourceControllerTest, UpdateBrokerRejectsWhenProcessNotRunning) {
    const int64_t id = create_source("ctp_upd3");
    auto req = admin_req();
    req->setBody(R"({"name":"b1","broker_id":"B1"})");
    auto resp = invoke(&MarketSourceCtrl::update_broker, req, id, std::string{"b1"});
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

// ---- update_broker_frontends ----

TEST_F(MarketSourceControllerTest, UpdateBrokerFrontendsAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_fe1");
    auto req = user_req();
    req->setBody(R"({"frontends":[]})");
    auto resp = invoke(&MarketSourceCtrl::update_broker_frontends, req, id, std::string{"b1"});
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, UpdateBrokerFrontendsMissingArrayReturns400) {
    const int64_t id = create_source("ctp_fe2");
    auto req = admin_req();
    req->setBody(R"({"address":"tcp://x"})");
    auto resp = invoke(&MarketSourceCtrl::update_broker_frontends, req, id, std::string{"b1"});
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(MarketSourceControllerTest, UpdateBrokerFrontendsRejectsWhenProcessNotRunning) {
    const int64_t id = create_source("ctp_fe3");
    auto req = admin_req();
    req->setBody(R"({"frontends":[]})");
    auto resp = invoke(&MarketSourceCtrl::update_broker_frontends, req, id, std::string{"b1"});
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

// ---- set_current_broker ----

TEST_F(MarketSourceControllerTest, SetCurrentBrokerAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_cur1");
    auto req = user_req();
    req->setBody(R"({"name":"b1"})");
    auto resp = invoke(&MarketSourceCtrl::set_current_broker, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, SetCurrentBrokerNonexistentSourceReturns404) {
    auto req = admin_req();
    req->setBody(R"({"name":"b1"})");
    auto resp = invoke(&MarketSourceCtrl::set_current_broker, req, int64_t{99999});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

TEST_F(MarketSourceControllerTest, SetCurrentBrokerRejectsWhenProcessNotRunning) {
    const int64_t id = create_source("ctp_cur2");
    auto req = admin_req();
    req->setBody(R"({"name":"b1"})");
    auto resp = invoke(&MarketSourceCtrl::set_current_broker, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

TEST_F(MarketSourceControllerTest, SetCurrentBrokerEmptyNameAllowed) {
    // name="" 表示清空选中 (合法), 仍需进程在跑才能下发
    const int64_t id = create_source("ctp_cur3");
    auto req = admin_req();
    req->setBody(R"({"name":""})");
    auto resp = invoke(&MarketSourceCtrl::set_current_broker, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

// ---- list_available: added = DB 存在且 is_added=1（契约 rest §2.3 修订）----
// 扫描目标用测试 exe 目录真实存在的 dzmd_ctp_md_state_test.exe stem
TEST_F(MarketSourceControllerTest, AvailableAddedFollowsDbLifecycle) {
    constexpr const char* k_scan_target = "dzmd_ctp_md_state_test";
    // DB 入库（is_added=1）-> added=true
    const int64_t id = create_source(k_scan_target);
    {
        auto req = admin_req();
        auto resp = invoke(&MarketSourceCtrl::list_available, req);
        auto body = parse_body(resp);
        bool found = false;
        for (const auto& item : body) {
            if (item.value("name", "") == k_scan_target) {
                found = true;
                EXPECT_TRUE(item.value("added", false)) << "db row is_added=1 -> added=true";
                EXPECT_EQ(item.value("ui_card", std::string{"<missing>"}), "ctp");
            }
        }
        EXPECT_TRUE(found) << "test requires dzmd_ctp_md_state_test.exe in test exe dir";
    }
    // remove 标记（is_added=0）-> added=false（与进程镜像状态无关）
    EXPECT_TRUE(repo_->set_market_source_added(id, false));
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::list_available, req);
    auto body = parse_body(resp);
    bool found = false;
    for (const auto& item : body) {
        if (item.value("name", "") == k_scan_target) {
            found = true;
            EXPECT_FALSE(item.value("added", true)) << "db row is_added=0 -> added=false";
            EXPECT_EQ(item.value("ui_card", std::string{"<missing>"}), "ctp");
        }
    }
    EXPECT_TRUE(found) << "test requires dzmd_ctp_md_state_test.exe in test exe dir";
}

TEST_F(MarketSourceControllerTest, AvailableUnmanagedExeNotAdded) {
    // 未入库的 exe（无论镜像状态）-> added=false
    constexpr const char* k_scan_target = "dzmd_ctp_md_state_test";
    dztrader::platform::ProcessStatus s;
    s.name = k_scan_target;
    s.state = dztrader::platform::ChildState::Running;
    process_mirror_->update_status(k_scan_target, s);  // 镜像 Running 但 DB 无行

    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::list_available, req);
    auto body = parse_body(resp);
    bool found = false;
    for (const auto& item : body) {
        if (item.value("name", "") == k_scan_target) {
            found = true;
            EXPECT_FALSE(item.value("added", true))
                << "running mirror but no db row -> added=false (is_added 语义)";
        }
    }
    EXPECT_TRUE(found) << "test requires dzmd_ctp_md_state_test.exe in test exe dir";
}

// ---- get_config password redaction (Wave 5A) ----
//
// Wave 5A: 修复 password 通过 HTTP get_config 泄露
// - top-level password → "****"
// - brokers[].password → "****"
// - mirror 中的原始 config 不被脱敏 (保持子进程上报原值, get_config 出口脱敏)

TEST_F(MarketSourceControllerTest, GetConfigRedactsTopLevelAndBrokerPasswords) {
    const int64_t id = create_source("ctp_cfg1");
    // 填充镜像: 配置含 top-level password + brokers[].password 明文
    dztrader::platform::ProcessStatus s;
    s.name = "dzmd_ctp";
    s.state = dztrader::platform::ChildState::Running;
    process_mirror_->update_status("dzmd_ctp", s);

    const nlohmann::json config = nlohmann::json{
        {"brokers", nlohmann::json::array({
                        nlohmann::json{
                            {"name", "b1"},
                            {"broker_id", "B1"},
                            {"user_id", "u1"},
                            {"password", "broker_secret"},
                            {"product_info", "pi"},
                            {"frontends", nlohmann::json::array()},
                        },
                    })},
        {"current_broker_name", "b1"},
        {"password", "top_secret"},
    };
    process_mirror_->update_config("dzmd_ctp", config);

    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::get_config, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["config"]["password"].get<std::string>(), "****");
    EXPECT_EQ(body["config"]["brokers"][0]["password"].get<std::string>(), "****");
    // 非密码字段保持原值
    EXPECT_EQ(body["config"]["brokers"][0]["broker_id"].get<std::string>(), "B1");
    EXPECT_EQ(body["config"]["brokers"][0]["name"].get<std::string>(), "b1");

    // 回归检查: mirror 中的 config 仍是明文 (未被脱敏破坏, 子进程上报原值保持完整)
    auto mirror_config = process_mirror_->get_config("dzmd_ctp");
    ASSERT_TRUE(mirror_config.has_value());
    EXPECT_EQ((*mirror_config)["password"].get<std::string>(), "top_secret");
    EXPECT_EQ((*mirror_config)["brokers"][0]["password"].get<std::string>(), "broker_secret");
}

TEST_F(MarketSourceControllerTest, GetConfigAsNonAdminReturns403) {
    const int64_t id = create_source("ctp_cfg2");
    auto req = user_req();
    auto resp = invoke(&MarketSourceCtrl::get_config, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(MarketSourceControllerTest, GetConfigNonexistentSourceReturns404) {
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::get_config, req, int64_t{99999});
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

TEST_F(MarketSourceControllerTest, GetConfigRejectsWhenMirrorNotReady) {
    const int64_t id = create_source("ctp_cfg3");
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::get_config, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

// ---- start / stop: writer 未就绪 -> 503（契约 rest §1）----

TEST_F(MarketSourceControllerTest, StartReturns503WhenShmWriteFails) {
    const int64_t id = create_source("ctp_start_w");
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::start, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

TEST_F(MarketSourceControllerTest, StopReturns503WhenShmWriteFails) {
    const int64_t id = create_source("ctp_stop_w");
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::stop, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

// ---- login/logout: 镜像 Running 但 writer 未就绪 -> 503 ----

TEST_F(MarketSourceControllerTest, LoginReturns503WhenWriterNotReady) {
    const int64_t id = create_source("ctp_login_w");
    prime_mirror_for_dispatch(*process_mirror_, "dzmd_ctp");
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::login, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

TEST_F(MarketSourceControllerTest, LogoutReturns503WhenWriterNotReady) {
    const int64_t id = create_source("ctp_logout_w");
    prime_mirror_for_dispatch(*process_mirror_, "dzmd_ctp");
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::logout, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

// ---- data_changed{market_sources} 广播（契约 rest §2.3 修订：列表 DB 真相源的多客户端同步）----

TEST_F(MarketSourceControllerTest, CreateBroadcastsMarketSourcesScope) {
    std::vector<std::string> scopes;
    dztrader::webui::g_broadcast_data_changed =
        [&scopes](const std::string& s) { scopes.push_back(s); };
    auto req = admin_req();
    req->setBody(R"({"source_type":"ctp","source_name":"ctp_bc1","display_name":"X"})");
    auto resp = invoke(&MarketSourceCtrl::create, req);
    dztrader::webui::g_broadcast_data_changed = nullptr;  // 复位全局指针，防污染其他用例
    EXPECT_EQ(resp->getStatusCode(), drogon::k201Created);
    ASSERT_EQ(scopes.size(), 1u);
    EXPECT_EQ(scopes[0], "market_sources");
}

TEST_F(MarketSourceControllerTest, UpdateBroadcastsMarketSourcesScope) {
    const int64_t id = create_source("ctp_bc2");
    std::vector<std::string> scopes;
    dztrader::webui::g_broadcast_data_changed =
        [&scopes](const std::string& s) { scopes.push_back(s); };
    auto req = admin_req();
    req->setBody(R"({"display_name":"新名字"})");
    auto resp = invoke(&MarketSourceCtrl::update, req, id);
    dztrader::webui::g_broadcast_data_changed = nullptr;
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    ASSERT_EQ(scopes.size(), 1u);
    EXPECT_EQ(scopes[0], "market_sources");
}

TEST_F(MarketSourceControllerTest, RemoveNoBroadcastWhenWriteFails) {
    // 503 路径（写帧失败）不得广播——列表条目未变化
    const int64_t id = create_source("ctp_bc3");
    std::vector<std::string> scopes;
    dztrader::webui::g_broadcast_data_changed =
        [&scopes](const std::string& s) { scopes.push_back(s); };
    auto req = admin_req();
    auto resp = invoke(&MarketSourceCtrl::remove, req, id);
    dztrader::webui::g_broadcast_data_changed = nullptr;
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
    EXPECT_TRUE(scopes.empty());
}

// ---- shm-config: 空对象 {} = no-op 透传（契约 shm §SET，I7）----

/// 构造独立事件通道，返回就绪 ShmWriter 与捕获 Reader。
/// 使 guard_process_dispatch 的 is_ready 通过，并能读到写入的 SET_MD_SHM_CONFIG 帧。
struct ReadyShmFixture {
    std::string channel_name;
    std::filesystem::path shm_dir;
    std::shared_ptr<shm::ChannelMeta> meta;
    std::shared_ptr<shm::MultiWriter> writer;
    std::optional<shm::Reader> reader;
    static constexpr uint64_t MB = 1024ull * 1024;

    ReadyShmFixture() {
        channel_name = "dz_test_shmcfg_" + std::to_string(reinterpret_cast<uintptr_t>(this));
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
    ~ReadyShmFixture() {
        reader.reset();
        writer.reset();
        meta.reset();
        std::filesystem::remove_all(shm_dir);
    }
    // 排空 reader，返回最后一帧是否与目标 instance 匹配的 SET_MD_SHM_CONFIG 及 payload
    struct Captured { bool found = false; nlohmann::json payload; };
    Captured capture_shm_config(const std::string& instance) {
        Captured out;
        while (const auto* raw = reader->next_frame()) {
            const shm::FrameView view(raw);
            if (view.type() != DZ_FRAME_SET_MD_SHM_CONFIG) {
                continue;
            }
            if (std::string(view.ext_inst_id()) != instance) {
                continue;
            }
            const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
            out.payload = nlohmann::json::parse(data, data + view.ext_inst_payload_size());
            out.found = true;
            break;
        }
        return out;
    }
};

TEST_F(MarketSourceControllerTest, SetShmConfigEmptyObjectIsNoop_Dispatched200) {
    // 契约 shm §SET: 空对象 {} = 无操作 no-op, 仍回 RTN（透传给网关，由网关回当前值）。
    // 此前的实现把空 body 判为 400，与契约矛盾；修复后应放行透传。
    // 镜像就绪 + writer 就绪时，set_shm_config 应返回 200 dispatched。
    const int64_t id = create_source("ctp_shmcfg_empty");
    prime_mirror_for_dispatch(*process_mirror_, "dzmd_ctp");

    ReadyShmFixture shm;
    shm_writer_ = std::make_shared<ShmWriter>(shm.writer);
    ctrl_ = std::make_shared<MarketSourceCtrl>(repo_, shm_writer_, process_mirror_);

    auto req = admin_req();
    req->setBody(R"({})");
    auto resp = invoke(&MarketSourceCtrl::set_shm_config, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    // 网关收到 payload 为 {} 的 SET_MD_SHM_CONFIG 帧
    auto cap = shm.capture_shm_config("dzmd_ctp");
    ASSERT_TRUE(cap.found) << "应写入 SET_MD_SHM_CONFIG 帧 (instance=dzmd_ctp)";
    EXPECT_TRUE(cap.payload.is_object());
    EXPECT_TRUE(cap.payload.empty());
}

TEST_F(MarketSourceControllerTest, SetShmConfigNonObjectBodyReturns400) {
    // 仅非 object 形态（数组/标量/null）才 400（契约 shm §SET）
    const int64_t id = create_source("ctp_shmcfg_arr");
    auto req = admin_req();
    req->setBody(R"([1,2])");
    auto resp = invoke(&MarketSourceCtrl::set_shm_config, req, id);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

}  // anonymous namespace
}  // namespace dztrader::webui
