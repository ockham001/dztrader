/**
 * @file td_gateway_controller_test.cpp
 * @brief 交易接口（`dztd_*`）生命周期端点（契约 rest §2.10、ADR 0053）。
 *
 * 真 handler 直调 + 捕获响应（与 `td_account_controller_test` 同型），真 SQLite 临时库
 * （真 platform.db schema），真 SHM 通道做帧 payload 断言（`ReadyShmFixture` 模式），
 * 扫描根注入临时目录（不依赖测试二进制所在目录里恰好有 `dztd_*` exe）。
 * 断言 HTTP 语义与契约锚点：状态码、`code` 机器码、预检顺序、帧 `{action, target, config}`
 * 形状、预检失败时一帧不写。
 */

#include "td_gateway_controller.h"

#include "ready_shm_fixture.h"
#include "td_account_registry.h"
#include "test_temp_dir.h"
#include "test_util.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/platform/process.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::webui {
namespace {

using test::ReadyShmFixture;  // 共享 SHM 夹具，见 ready_shm_fixture.h

/// 扫描器按平台判可执行文件：Windows 认 `.exe` 后缀，Linux 认 owner_exec 权限位。
#ifdef _WIN32
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

class TdGatewayControllerTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::filesystem::path scan_dir_;
    std::shared_ptr<Repository> repo_;
    std::shared_ptr<TdAccountRegistry> registry_;
    /// 共享镜像（与生产 main.cpp 同构：AutoLoginDomainService 写、ProcessMirror 读）。
    /// 本 fixture 需要直接播种 `auto_login` 域来测 GET/PUT（#530 M3）。
    std::shared_ptr<MirrorStore> mirror_store_;
    std::shared_ptr<ProcessMirror> process_mirror_;
    std::shared_ptr<ShmWriter> shm_writer_;
    std::shared_ptr<TdGatewayCtrl> ctrl_;
    /// 进程配置镜像累积的已注册条目（契约 process：RTN_PROCESS_CONFIG 是全量映射）。
    nlohmann::json primed_configs_ = nlohmann::json::object();

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_webui_td_gateway_controller_test");
        std::filesystem::create_directories(dir_);
        scan_dir_ = dir_ / "app_root";
        std::filesystem::create_directories(scan_dir_);
        // 真 platform.db（Repository ctor 跑 run_schema）——注册表持久层用真 schema。
        repo_ = std::make_shared<Repository>(dir_ / "platform.db");
        repo_->create_user("admin", "Administrator", "admin@test.com", "pass", "admin");
        repo_->create_user("regular", "Regular User", "reg@test.com", "pass", "user");
        registry_ = std::make_shared<TdAccountRegistry>(
            std::make_unique<RepositoryTdAccountRegistryStore>(*repo_));
        mirror_store_ = std::make_shared<MirrorStore>();
        process_mirror_ = std::make_shared<ProcessMirror>(*mirror_store_);
        // 未就绪 writer（临时目录无 dzevent 通道）→ is_ready() false
        shm_writer_ = std::make_shared<ShmWriter>(std::filesystem::temp_directory_path());
        rebuild_ctrl();
    }

    void TearDown() override {
        ctrl_.reset();
        shm_writer_.reset();
        process_mirror_.reset();
        mirror_store_.reset();
        registry_.reset();
        repo_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    void rebuild_ctrl() {
        ctrl_ = std::make_shared<TdGatewayCtrl>(repo_, registry_, process_mirror_, shm_writer_,
                                                scan_dir_);
    }

    drogon::HttpRequestPtr admin_req(const std::string& body = "") {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("admin"));
        if (!body.empty()) {
            req->setBody(body);
        }
        return req;
    }
    drogon::HttpRequestPtr user_req(const std::string& body = "") {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("regular"));
        if (!body.empty()) {
            req->setBody(body);
        }
        return req;
    }

    template <typename Fn, typename... Args>
    drogon::HttpResponsePtr invoke(Fn fn, const drogon::HttpRequestPtr& req, Args&&... args) {
        drogon::HttpResponsePtr captured;
        (ctrl_.get()->*fn)(
            req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; },
            std::forward<Args>(args)...);
        return captured;
    }

    static nlohmann::json parse_body(const drogon::HttpResponsePtr& resp) {
        return nlohmann::json::parse(resp->getBody());
    }

    /// 注入就绪 writer（配合 ReadyShmFixture 做帧断言）。
    void use_ready_shm(ReadyShmFixture& shm) {
        shm_writer_ = std::make_shared<ShmWriter>(shm.writer);
        rebuild_ctrl();
    }

    /// 在注入的扫描根下造一个「可执行文件」桩（契约 process 的扫描判据）。
    void make_exe(const std::string& name) {
        const std::filesystem::path path = scan_dir_ / (name + kExeSuffix);
        std::ofstream(path) << "";
#ifndef _WIN32
        std::filesystem::permissions(path,
                                     std::filesystem::perms::owner_exec |
                                         std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add);
#endif
    }

    /// 把一个交易网关置为「已注册」（进程配置镜像有条目，契约 process）。
    /// **累积**：全量帧语义下多次调用不能互相抹掉（与"多个网关同时存在"的真实场景一致）。
    void prime_gateway(const std::string& name, const std::string& display_name = "") {
        nlohmann::json entry{
            {"args", nlohmann::json::array()},
            {"env", nlohmann::json::object()},
            {"restart",
             nlohmann::json{{"enabled", false}, {"max_attempts", 0}, {"backoff_sec", 5}}}};
        if (!display_name.empty()) {
            entry["display_name"] = display_name;
        }
        primed_configs_[name] = std::move(entry);
        process_mirror_->update_process_configs(primed_configs_);
    }

    /// 填充进程状态（`process_status` 镜像，契约 process）。
    void set_state(const std::string& name, platform::ChildState state) {
        platform::ProcessStatus status;
        status.name = name;
        status.state = state;
        process_mirror_->update_status(name, status);
    }

    /// 把账户直接写进注册表（等价于收到一帧配置回报），供 DELETE guard 用例使用。
    void seed_account(const std::string& gateway,
                      const std::string& account_id,
                      bool enabled = true) {
        registry_->reconcile_gateway(gateway, {TdAccountRegistryEntry{.account_id = account_id,
                                                                      .gateway = gateway,
                                                                      .account_name = "",
                                                                      .enabled = enabled}});
    }

    /// 超过单页（`ReadyShmFixture` page_size = 1MB）的进程名：用于构造「就绪 writer 写失败」
    /// 分支——stop / DELETE 没有 body，只能靠帧 target 撑爆单帧（`write_process_control`
    /// 返回 false）。名字仍合法（`dztd_*` 前缀），注册进镜像后其 Stop/Remove 帧必然超页。
    static std::string oversized_gateway_name() { return "dztd_" + std::string(1536 * 1024, 'x'); }

    /// 错误体断言：状态码 + `error` 人类可读消息 + 机器码 `code`（契约 rest §1）。
    static void expect_code(const drogon::HttpResponsePtr& resp,
                            drogon::HttpStatusCode status,
                            const std::string& message,
                            const std::string& code) {
        ASSERT_NE(resp.get(), nullptr);
        EXPECT_EQ(resp->getStatusCode(), status);
        const auto body = parse_body(resp);
        EXPECT_EQ(body.at("error").get<std::string>(), message);
        ASSERT_TRUE(body.contains("code")) << "precheck failures must carry a machine code";
        EXPECT_EQ(body.at("code").get<std::string>(), code);
    }

    /// 同 expect_code，但只断言机器码（错误文案来自 `AutoLoginConfig::validate`，
    /// 逐字断言会把校验文案锁死）。
    static void expect_code(const drogon::HttpResponsePtr& resp,
                            drogon::HttpStatusCode status,
                            const std::string& code) {
        ASSERT_NE(resp.get(), nullptr);
        EXPECT_EQ(resp->getStatusCode(), status);
        const auto body = parse_body(resp);
        ASSERT_TRUE(body.contains("code")) << "precheck failures must carry a machine code";
        EXPECT_EQ(body.at("code").get<std::string>(), code);
    }

    /// 错误体断言：状态码 + `error` 消息，且**不带** `code`（基础设施失败，契约 rest §2.10）。
    static void expect_no_code(const drogon::HttpResponsePtr& resp,
                               drogon::HttpStatusCode status,
                               const std::string& message) {
        ASSERT_NE(resp.get(), nullptr);
        EXPECT_EQ(resp->getStatusCode(), status);
        const auto body = parse_body(resp);
        EXPECT_EQ(body.at("error").get<std::string>(), message);
        EXPECT_FALSE(body.contains("code")) << "shm/registry failures must not carry a code";
    }
};

// ============================================================================
// GET /api/td/gateways/available（契约 rest §2.10）
// ============================================================================

TEST_F(TdGatewayControllerTest, AvailableIsReadableByNonAdmin) {
    // 权限：任意已认证用户（不是 admin，契约 rest §2.10）。
    make_exe("dztd_ctp");
    auto resp = invoke(&TdGatewayCtrl::list_available, user_req());
    ASSERT_NE(resp.get(), nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = parse_body(resp);
    ASSERT_TRUE(body.is_array());
    ASSERT_EQ(body.size(), 1U);
    EXPECT_EQ(body[0].at("name").get<std::string>(), "dztd_ctp");
}

TEST_F(TdGatewayControllerTest, AvailableFiltersNonTdProcesses) {
    make_exe("dztd_ctp");
    make_exe("dzmd_ctp");
    make_exe("dzweb");
    make_exe("dzsysmon");

    auto resp = invoke(&TdGatewayCtrl::list_available, user_req());
    const auto body = parse_body(resp);
    ASSERT_TRUE(body.is_array());
    ASSERT_EQ(body.size(), 1U) << "only dztd_* may appear in the td gateway list";
    EXPECT_EQ(body[0].at("name").get<std::string>(), "dztd_ctp");
}

TEST_F(TdGatewayControllerTest, AvailableReportsAddedRunningAndDisplayName) {
    make_exe("dztd_ctp");
    make_exe("dztd_xtp");
    prime_gateway("dztd_ctp", "CTP 交易接口");
    set_state("dztd_ctp", platform::ChildState::Running);
    set_state("dztd_xtp", platform::ChildState::Stopped);

    auto resp = invoke(&TdGatewayCtrl::list_available, user_req());
    const auto body = parse_body(resp);
    ASSERT_TRUE(body.is_array());
    ASSERT_EQ(body.size(), 2U);
    // 契约 rest §2.10：按 name 升序（dztd_ctp < dztd_xtp）。
    EXPECT_EQ(body[0].at("name").get<std::string>(), "dztd_ctp");
    EXPECT_EQ(body[0].at("display_name").get<std::string>(), "CTP 交易接口");
    EXPECT_TRUE(body[0].at("added").get<bool>());
    EXPECT_TRUE(body[0].at("running").get<bool>());
    // 未注册：display_name 回退 name，added=false。
    EXPECT_EQ(body[1].at("name").get<std::string>(), "dztd_xtp");
    EXPECT_EQ(body[1].at("display_name").get<std::string>(), "dztd_xtp");
    EXPECT_FALSE(body[1].at("added").get<bool>());
    EXPECT_FALSE(body[1].at("running").get<bool>());
}

TEST_F(TdGatewayControllerTest, AvailableFallsBackToNameWhenDisplayNameMissing) {
    make_exe("dztd_ctp");
    prime_gateway("dztd_ctp");  // 已注册但未配置 display_name

    auto resp = invoke(&TdGatewayCtrl::list_available, user_req());
    const auto body = parse_body(resp);
    ASSERT_EQ(body.size(), 1U);
    EXPECT_TRUE(body[0].at("added").get<bool>());
    EXPECT_EQ(body[0].at("display_name").get<std::string>(), "dztd_ctp");
}

TEST_F(TdGatewayControllerTest, AvailableRunningOnlyForRunningState) {
    make_exe("dztd_ctp");
    prime_gateway("dztd_ctp", "CTP 交易接口");
    set_state("dztd_ctp", platform::ChildState::Crashed);

    auto resp = invoke(&TdGatewayCtrl::list_available, user_req());
    const auto body = parse_body(resp);
    ASSERT_EQ(body.size(), 1U);
    EXPECT_TRUE(body[0].at("added").get<bool>());
    EXPECT_FALSE(body[0].at("running").get<bool>()) << "only Running counts as running";
}

// ============================================================================
// POST /api/td/gateways/{name}/start（契约 rest §2.10）
// ============================================================================

TEST_F(TdGatewayControllerTest, StartAsNonAdminReturns403WithAdminRequiredCode) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    auto resp = invoke(&TdGatewayCtrl::start, user_req(), std::string("dztd_ctp"));
    expect_code(resp, drogon::k403Forbidden, "admin required", "admin_required");
    EXPECT_FALSE(shm.has_any_frame()) << "precheck failure must not write a frame";
}

TEST_F(TdGatewayControllerTest, StartInvalidGatewayNameReturns400) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    auto resp = invoke(&TdGatewayCtrl::start, admin_req(), std::string("dzmd_ctp"));
    expect_code(resp, drogon::k400BadRequest, "invalid gateway", "invalid_gateway");
    EXPECT_FALSE(shm.has_any_frame());
}

TEST_F(TdGatewayControllerTest, StartUnregisteredWithoutExeReturns400) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    // 未注册且扫描根下无同名 exe：master 的动态注册不可能成功 → 400（契约 rest §2.10 预检 3）。
    auto resp = invoke(&TdGatewayCtrl::start, admin_req(), std::string("dztd_ghost"));
    expect_code(resp, drogon::k400BadRequest, "invalid gateway", "invalid_gateway");
    EXPECT_FALSE(shm.has_any_frame());
}

TEST_F(TdGatewayControllerTest, StartUnregisteredWithScannedExeWritesStartFrameWithDisplayName) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    make_exe("dztd_ctp");  // 可扫描到但尚未注册 → 允许（master 动态注册）
    auto req = admin_req(R"({"display_name":"CTP 交易接口"})");

    auto resp = invoke(&TdGatewayCtrl::start, req, std::string("dztd_ctp"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_TRUE(parse_body(resp).at("ok").get<bool>());

    auto cap = shm.capture_ext_frame(DZ_FRAME_REQUEST_PROCESS_CONTROL);
    ASSERT_TRUE(cap.found) << "start must write REQUEST_PROCESS_CONTROL";
    EXPECT_EQ(cap.payload.at("action").get<std::string>(), "Start");
    EXPECT_EQ(cap.payload.at("target").get<std::string>(), "dztd_ctp");
    ASSERT_TRUE(cap.payload.contains("config"))
        << "display_name must be sent as start config patch";
    EXPECT_EQ(cap.payload.at("config").at("display_name").get<std::string>(), "CTP 交易接口");
}

TEST_F(TdGatewayControllerTest, StartRegisteredStoppedWritesStartFrameWithoutConfig) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp", "CTP 交易接口");
    set_state("dztd_ctp", platform::ChildState::Stopped);  // 崩溃/停止后的恢复场景

    auto resp = invoke(&TdGatewayCtrl::start, admin_req(), std::string("dztd_ctp"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    auto cap = shm.capture_ext_frame(DZ_FRAME_REQUEST_PROCESS_CONTROL);
    ASSERT_TRUE(cap.found);
    EXPECT_EQ(cap.payload.at("action").get<std::string>(), "Start");
    EXPECT_EQ(cap.payload.at("target").get<std::string>(), "dztd_ctp");
    EXPECT_FALSE(cap.payload.contains("config")) << "no display_name -> no config patch";
}

TEST_F(TdGatewayControllerTest, StartEmptyDisplayNameDoesNotCarryPatch) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");

    auto resp =
        invoke(&TdGatewayCtrl::start, admin_req(R"({"display_name":""})"), std::string("dztd_ctp"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    auto cap = shm.capture_ext_frame(DZ_FRAME_REQUEST_PROCESS_CONTROL);
    ASSERT_TRUE(cap.found);
    EXPECT_FALSE(cap.payload.contains("config"))
        << "empty display_name means 'not provided', not 'clear config'";
}

TEST_F(TdGatewayControllerTest, StartMalformedBodyReturns400WithoutFrame) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");

    auto resp = invoke(&TdGatewayCtrl::start, admin_req("{not json"), std::string("dztd_ctp"));
    expect_no_code(resp, drogon::k400BadRequest, "bad request");
    EXPECT_FALSE(shm.has_any_frame());
}

TEST_F(TdGatewayControllerTest, StartWriterNotReadyReturns503WithoutCode) {
    prime_gateway("dztd_ctp");
    // 未注入就绪 writer：事件通道不可用 → 503（请求未被受理，不带 code；契约 rest §2.10）。
    auto resp = invoke(&TdGatewayCtrl::start, admin_req(), std::string("dztd_ctp"));
    expect_no_code(resp, drogon::k503ServiceUnavailable, "shm writer not available");
}

TEST_F(TdGatewayControllerTest, StartWriteFailsReturns503WithoutCode) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    // writer 就绪但单帧超过 page_size(1MB)：display_name 撑爆 Start 帧 → 写失败 → 503
    // （区别于 writer 未就绪分支；契约 rest §2.10 预检 6「shm write failed」）。
    auto req = admin_req(R"({"display_name":")" + std::string(1536 * 1024, 'x') + R"("})");
    auto resp = invoke(&TdGatewayCtrl::start, req, std::string("dztd_ctp"));
    expect_no_code(resp, drogon::k503ServiceUnavailable, "shm write failed");
    EXPECT_FALSE(shm.has_any_frame());
}

// ============================================================================
// POST /api/td/gateways/{name}/stop（契约 rest §2.10）
// ============================================================================

TEST_F(TdGatewayControllerTest, StopAsNonAdminReturns403WithAdminRequiredCode) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    auto resp = invoke(&TdGatewayCtrl::stop, user_req(), std::string("dztd_ctp"));
    expect_code(resp, drogon::k403Forbidden, "admin required", "admin_required");
    EXPECT_FALSE(shm.has_any_frame());
}

TEST_F(TdGatewayControllerTest, StopInvalidGatewayNameReturns400) {
    auto resp = invoke(&TdGatewayCtrl::stop, admin_req(), std::string("dzweb"));
    expect_code(resp, drogon::k400BadRequest, "invalid gateway", "invalid_gateway");
}

TEST_F(TdGatewayControllerTest, StopUnregisteredReturns404GatewayNotFound) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    auto resp = invoke(&TdGatewayCtrl::stop, admin_req(), std::string("dztd_ghost"));
    expect_code(resp, drogon::k404NotFound, "gateway not found", "gateway_not_found");
    EXPECT_FALSE(shm.has_any_frame());
}

TEST_F(TdGatewayControllerTest, StopRegisteredWritesStopFrame) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    set_state("dztd_ctp", platform::ChildState::Running);

    auto resp = invoke(&TdGatewayCtrl::stop, admin_req(), std::string("dztd_ctp"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_TRUE(parse_body(resp).at("ok").get<bool>());

    auto cap = shm.capture_ext_frame(DZ_FRAME_REQUEST_PROCESS_CONTROL);
    ASSERT_TRUE(cap.found) << "stop must write REQUEST_PROCESS_CONTROL";
    EXPECT_EQ(cap.payload.at("action").get<std::string>(), "Stop");
    EXPECT_EQ(cap.payload.at("target").get<std::string>(), "dztd_ctp");
    EXPECT_FALSE(cap.payload.contains("config"));
}

TEST_F(TdGatewayControllerTest, StopWriterNotReadyReturns503WithoutCode) {
    prime_gateway("dztd_ctp");
    auto resp = invoke(&TdGatewayCtrl::stop, admin_req(), std::string("dztd_ctp"));
    expect_no_code(resp, drogon::k503ServiceUnavailable, "shm writer not available");
}

TEST_F(TdGatewayControllerTest, StopWriteFailsReturns503WithoutCode) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    const std::string name = oversized_gateway_name();
    prime_gateway(name);  // Stop 帧 target 超页 → write_process_control 返回 false
    auto resp = invoke(&TdGatewayCtrl::stop, admin_req(), name);
    expect_no_code(resp, drogon::k503ServiceUnavailable, "shm write failed");
    EXPECT_FALSE(shm.has_any_frame());
}

// ============================================================================
// DELETE /api/td/gateways/{name}（契约 rest §2.10）
// ============================================================================

TEST_F(TdGatewayControllerTest, RemoveAsNonAdminReturns403WithAdminRequiredCode) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    auto resp = invoke(&TdGatewayCtrl::remove, user_req(), std::string("dztd_ctp"));
    expect_code(resp, drogon::k403Forbidden, "admin required", "admin_required");
    EXPECT_FALSE(shm.has_any_frame());
}

TEST_F(TdGatewayControllerTest, RemoveInvalidGatewayNameReturns400) {
    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), std::string("dzmd_ctp"));
    expect_code(resp, drogon::k400BadRequest, "invalid gateway", "invalid_gateway");
}

TEST_F(TdGatewayControllerTest, RemoveUnregisteredReturns404GatewayNotFound) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), std::string("dztd_ghost"));
    expect_code(resp, drogon::k404NotFound, "gateway not found", "gateway_not_found");
    EXPECT_FALSE(shm.has_any_frame());
}

TEST_F(TdGatewayControllerTest, RemoveWithAccountsReturns409GatewayHasAccounts) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");  // 注册表有该网关账户（契约 rest §2.10 预检 5）

    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), std::string("dztd_ctp"));
    expect_code(resp, drogon::k409Conflict, "gateway has accounts", "gateway_has_accounts");
    EXPECT_FALSE(shm.has_any_frame()) << "guard failure must not write Remove";
}

TEST_F(TdGatewayControllerTest, RemoveWithDisabledAccountStillReturns409) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a", /*enabled=*/false);

    // 已禁用账户同样是「有账户」：配置仍在接口文件里（ADR 0053 决策 3）。
    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), std::string("dztd_ctp"));
    expect_code(resp, drogon::k409Conflict, "gateway has accounts", "gateway_has_accounts");
}

TEST_F(TdGatewayControllerTest, RemoveRegistryUnavailableReturns503WithoutCode) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    // 未收到任何配置帧、platform.db 也无条目 → 注册表降级：无法确认是否为空，
    // 必须 503 且不写帧（契约 rest §2.10 预检 5）。
    ASSERT_FALSE(registry_->available());

    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), std::string("dztd_ctp"));
    expect_no_code(resp, drogon::k503ServiceUnavailable, "account registry unavailable");
    EXPECT_FALSE(shm.has_any_frame()) << "cannot confirm empty -> must not remove";
}

TEST_F(TdGatewayControllerTest, RemoveNoAccountsWritesRemoveFrame) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    registry_->note_config_frame_received();  // 注册表可用且该网关无账户

    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), std::string("dztd_ctp"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_TRUE(parse_body(resp).at("ok").get<bool>());

    auto cap = shm.capture_ext_frame(DZ_FRAME_REQUEST_PROCESS_CONTROL);
    ASSERT_TRUE(cap.found) << "remove must write REQUEST_PROCESS_CONTROL";
    EXPECT_EQ(cap.payload.at("action").get<std::string>(), "Remove");
    EXPECT_EQ(cap.payload.at("target").get<std::string>(), "dztd_ctp");
    EXPECT_FALSE(cap.payload.contains("config"));
}

TEST_F(TdGatewayControllerTest, RemoveWriterNotReadyReturns503WithoutCode) {
    prime_gateway("dztd_ctp");
    registry_->note_config_frame_received();
    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), std::string("dztd_ctp"));
    expect_no_code(resp, drogon::k503ServiceUnavailable, "shm writer not available");
}

TEST_F(TdGatewayControllerTest, RemoveWriteFailsReturns503WithoutCode) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    const std::string name = oversized_gateway_name();
    prime_gateway(name);  // Remove 帧 target 超页 → write_process_control 返回 false
    registry_->note_config_frame_received();
    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), name);
    expect_no_code(resp, drogon::k503ServiceUnavailable, "shm write failed");
    EXPECT_FALSE(shm.has_any_frame());
}

TEST_F(TdGatewayControllerTest, RemoveIgnoresAccountsOfOtherGateway) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    seed_account("dztd_xtp", "acc_x");  // 账户属于另一个交易接口，不是删除目标

    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), std::string("dztd_ctp"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK)
        << "guard must be per-gateway: another gateway's accounts must not block removal";

    auto cap = shm.capture_ext_frame(DZ_FRAME_REQUEST_PROCESS_CONTROL);
    ASSERT_TRUE(cap.found);
    EXPECT_EQ(cap.payload.at("action").get<std::string>(), "Remove");
    EXPECT_EQ(cap.payload.at("target").get<std::string>(), "dztd_ctp");
}

// ============================================================================
// 预检顺序（契约 rest §2.10：编号即语义清单，失败时不写帧）
// ============================================================================

TEST_F(TdGatewayControllerTest, AdminCheckedBeforeGatewayNameValidation) {
    // 非 admin + 非法名：admin(1) 先于 name(2)。
    auto resp = invoke(&TdGatewayCtrl::start, user_req(), std::string("not-a-gateway"));
    expect_code(resp, drogon::k403Forbidden, "admin required", "admin_required");
}

TEST_F(TdGatewayControllerTest, GatewayNameValidationCheckedBeforeRegistrationLookup) {
    // admin + 非法名（也未被注册）：name(2) 先于 not-found(4) → 400 而非 404。
    auto resp = invoke(&TdGatewayCtrl::stop, admin_req(), std::string("dzmd_ctp"));
    expect_code(resp, drogon::k400BadRequest, "invalid gateway", "invalid_gateway");
}

TEST_F(TdGatewayControllerTest, RemoveRegistrationLookupCheckedBeforeRegistryGuard) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    // 未注册 + 注册表降级：not-found(4) 先于 guard(5) → 404 而非 503。
    ASSERT_FALSE(registry_->available());
    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), std::string("dztd_ghost"));
    expect_code(resp, drogon::k404NotFound, "gateway not found", "gateway_not_found");
    EXPECT_FALSE(shm.has_any_frame());
}

TEST_F(TdGatewayControllerTest, RemoveRegistryGuardCheckedBeforeWriterCheck) {
    // 已注册 + 注册表降级 + writer 未就绪：guard(5) 先于写通道(6) → 503 registry。
    prime_gateway("dztd_ctp");
    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), std::string("dztd_ctp"));
    expect_no_code(resp, drogon::k503ServiceUnavailable, "account registry unavailable");
}

TEST_F(TdGatewayControllerTest, RemoveAccountGuardCheckedBeforeWriterCheck) {
    // 已注册 + 有账户 + writer 未就绪：guard(5) 先于写通道(6) → 409（不是 503）。
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");
    auto resp = invoke(&TdGatewayCtrl::remove, admin_req(), std::string("dztd_ctp"));
    expect_code(resp, drogon::k409Conflict, "gateway has accounts", "gateway_has_accounts");
}

// ============================================================================
// GET/PUT /api/td/gateways/{name}/auto-login（契约 rest §2.10 / 契约 auto-login；#530 M3）
// ============================================================================

namespace {

/// 播种镜像 auto_login 域（等价于收到 RTN_AUTO_LOGIN：AutoLoginDomainService 写 MirrorStore）。
void seed_auto_login(MirrorStore& mirror,
                     const std::string& gateway,
                     bool enabled,
                     nlohmann::json schedules) {
    mirror.update(gateway, "auto_login",
                  nlohmann::json{{"enabled", enabled}, {"schedules", std::move(schedules)}});
}

nlohmann::json day_schedules() {
    return nlohmann::json::array({
        nlohmann::json{{"login_time", "08:45"}, {"logout_time", "15:45"}},
    });
}

}  // namespace

TEST_F(TdGatewayControllerTest, AutoLoginGetReadableByNonAdminReturnsMirror) {
    // 权限：任意已认证用户（不是 admin，契约 rest §2.10）；source 标明数据来源。
    seed_auto_login(*mirror_store_, "dztd_ctp", true, day_schedules());
    auto resp = invoke(&TdGatewayCtrl::get_auto_login, user_req(), std::string("dztd_ctp"));
    ASSERT_NE(resp.get(), nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = parse_body(resp);
    EXPECT_EQ(body.at("source").get<std::string>(), "mirror");
    EXPECT_TRUE(body.at("enabled").get<bool>());
    ASSERT_EQ(body.at("schedules").size(), 1U);
    EXPECT_EQ(body.at("schedules")[0].at("login_time").get<std::string>(), "08:45");
}

TEST_F(TdGatewayControllerTest, AutoLoginGetReturnsUnavailableWhenMirrorMissing) {
    // 网关未运行 / 尚未上报：200 + source:"unavailable"，不编造内置默认排程（契约 rest §2.10）。
    auto resp = invoke(&TdGatewayCtrl::get_auto_login, user_req(), std::string("dztd_ctp"));
    ASSERT_NE(resp.get(), nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = parse_body(resp);
    EXPECT_EQ(body.at("source").get<std::string>(), "unavailable");
    EXPECT_FALSE(body.at("enabled").get<bool>());
    EXPECT_TRUE(body.at("schedules").is_array());
    EXPECT_TRUE(body.at("schedules").empty());
}

TEST_F(TdGatewayControllerTest, AutoLoginGetRejectsInvalidGatewayName) {
    auto resp = invoke(&TdGatewayCtrl::get_auto_login, user_req(), std::string("not-a-gateway"));
    expect_code(resp, drogon::k400BadRequest, "invalid gateway", "invalid_gateway");
}

TEST_F(TdGatewayControllerTest, AutoLoginPutRequiresAdmin) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    // 非 admin：403（预检 1 先于其它一切），且一帧不写。
    auto resp = invoke(&TdGatewayCtrl::set_auto_login, user_req(R"({"enabled":false})"),
                       std::string("dztd_ctp"));
    expect_code(resp, drogon::k403Forbidden, "admin required", "admin_required");
    EXPECT_FALSE(shm.has_any_frame());
}

TEST_F(TdGatewayControllerTest, AutoLoginPutRejectsInvalidJsonAndNonObjectBody) {
    prime_gateway("dztd_ctp");
    auto bad_json =
        invoke(&TdGatewayCtrl::set_auto_login, admin_req("{not json"), std::string("dztd_ctp"));
    expect_code(bad_json, drogon::k400BadRequest, "invalid json", "invalid_json");

    auto non_object =
        invoke(&TdGatewayCtrl::set_auto_login, admin_req("[]"), std::string("dztd_ctp"));
    expect_code(non_object, drogon::k400BadRequest, "body must be an object", "invalid_body");
}

TEST_F(TdGatewayControllerTest, AutoLoginPutRejectsNullAnywhere) {
    // 契约 auto-login：任何位置出现 null 均为校验失败（#530 M4）。
    // 顶层/嵌套用例在改造前也会因 merged 缺必填字段被 validate 拒；真正锁定「递归 null
    // 拒绝」的是 unknown_field=null —— 改造前它被 erase 后放行并下发带 null 的 body。
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");

    auto top_level = invoke(&TdGatewayCtrl::set_auto_login, admin_req(R"({"enabled":null})"),
                            std::string("dztd_ctp"));
    expect_code(top_level, drogon::k400BadRequest, "invalid_auto_login");

    auto nested = invoke(&TdGatewayCtrl::set_auto_login,
                         admin_req(R"({"schedules":[{"login_time":"08:45","logout_time":null}]})"),
                         std::string("dztd_ctp"));
    expect_code(nested, drogon::k400BadRequest, "invalid_auto_login");

    auto unknown_field = invoke(&TdGatewayCtrl::set_auto_login,
                                admin_req(R"({"future_field":null})"), std::string("dztd_ctp"));
    expect_code(unknown_field, drogon::k400BadRequest, "invalid_auto_login");
    EXPECT_FALSE(shm.has_any_frame()) << "invalid body must not be dispatched";
}

TEST_F(TdGatewayControllerTest, AutoLoginPutRejectsInvalidGatewayName) {
    // 预检 2：name 必须匹配 dztd_*（admin 已过）。
    auto resp = invoke(&TdGatewayCtrl::set_auto_login, admin_req(R"({"enabled":false})"),
                       std::string("not-a-gateway"));
    expect_code(resp, drogon::k400BadRequest, "invalid gateway", "invalid_gateway");
}

TEST_F(TdGatewayControllerTest, AutoLoginPutRejectsInvalidValues) {
    prime_gateway("dztd_ctp");
    // validate 唯一真相源：login_time 非法格式 → 400 invalid_auto_login。
    auto resp = invoke(&TdGatewayCtrl::set_auto_login,
                       admin_req(R"({"schedules":[{"login_time":"25:00","logout_time":"15:45"}]})"),
                       std::string("dztd_ctp"));
    expect_code(resp, drogon::k400BadRequest, "invalid_auto_login");
}

TEST_F(TdGatewayControllerTest, AutoLoginPutValidationPrecedesRegistrationLookup) {
    // 预检顺序：校验(4) 先于注册检查(5)。非法 body + 未注册 → 400 invalid_auto_login
    // （若顺序反了会先 404，本用例可区分；#530 M3）。
    auto resp = invoke(&TdGatewayCtrl::set_auto_login,
                       admin_req(R"({"schedules":[{"login_time":"25:00","logout_time":"15:45"}]})"),
                       std::string("dztd_ghost"));
    expect_code(resp, drogon::k400BadRequest, "invalid_auto_login");
}

TEST_F(TdGatewayControllerTest, AutoLoginPutUnregisteredGatewayReturns404) {
    // 合法 body + 未注册 → 404 gateway_not_found（不是 503）。
    auto resp = invoke(&TdGatewayCtrl::set_auto_login, admin_req(R"({"enabled":false})"),
                       std::string("dztd_ghost"));
    expect_code(resp, drogon::k404NotFound, "gateway not found", "gateway_not_found");
}

TEST_F(TdGatewayControllerTest, AutoLoginPutWriterNotReadyReturns503WithoutCode) {
    prime_gateway("dztd_ctp");
    auto resp = invoke(&TdGatewayCtrl::set_auto_login, admin_req(R"({"enabled":false})"),
                       std::string("dztd_ctp"));
    expect_no_code(resp, drogon::k503ServiceUnavailable, "shm writer not available");
}

TEST_F(TdGatewayControllerTest, AutoLoginPutDispatchesSetAutoLoginFrame) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    // 增量 body：只发 enabled，缺失字段由网关沿用当前值（契约 auto-login RFC 7386）。
    auto resp = invoke(&TdGatewayCtrl::set_auto_login, admin_req(R"({"enabled":false})"),
                       std::string("dztd_ctp"));
    ASSERT_NE(resp.get(), nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_TRUE(parse_body(resp).at("ok").get<bool>());

    auto cap = shm.capture_inst_frame(DZ_FRAME_SET_AUTO_LOGIN, "dztd_ctp");
    ASSERT_TRUE(cap.found) << "SET_AUTO_LOGIN must be dispatched to the gateway";
    EXPECT_EQ(cap.payload, nlohmann::json({{"enabled", false}}));
}

TEST_F(TdGatewayControllerTest, AutoLoginPutValidatesMergedConfigAgainstMirror) {
    ReadyShmFixture shm;
    use_ready_shm(shm);
    prime_gateway("dztd_ctp");
    // 证明 dzweb 的校验基准是「镜像当前值 + 增量 patch」，不是 patch 本身：
    // 在镜像里注入非法排程（真实 RTN 已经过 AutoLoginDomainService 全量校验，这里只为
    // 构造可区分的行为），只发 {enabled:false} 时 merged 仍非法 → 400 且一帧不写。
    mirror_store_->update(
        "dztd_ctp", "auto_login",
        nlohmann::json{{"enabled", true},
                       {"schedules", nlohmann::json::array({nlohmann::json{
                                         {"login_time", "bad"}, {"logout_time", "15:45"}}})}});
    auto resp = invoke(&TdGatewayCtrl::set_auto_login, admin_req(R"({"enabled":false})"),
                       std::string("dztd_ctp"));
    expect_code(resp, drogon::k400BadRequest, "invalid_auto_login");
    EXPECT_FALSE(shm.has_any_frame());
}

}  // namespace
}  // namespace dztrader::webui
