/**
 * @file td_account_controller_test.cpp
 * @brief 账户注册表读端点与账户配置写端点（契约 rest §2.9.7 / §2.9.9）。
 *
 * 真 handler 直调 + 捕获响应（与 `market_source_controller_test` 同型），真 SQLite 临时库
 * （真 platform.db schema），真 SHM 通道做帧 payload 断言（`ReadyShmFixture` 模式）。
 * 断言 HTTP 语义与契约锚点：状态码、`code` 机器码、帧 `{op, params}` 形状、失败时不写帧。
 */

#include "td_account_controller.h"
#include "fake_data_change_notifier.h"
#include "fake_td_account_registry_store.h"
#include "td_account_registry.h"
#include "test_temp_dir.h"
#include "test_util.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
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

/// 真 SHM 通道 fixture：捕获 dzweb 写入的定向帧（帧断言用）。
/// 通道名按 pid+随机+实例序号唯一化（ctest -j 下不同测试进程互不干扰）。
struct ReadyShmFixture {
    std::string channel_name;
    std::filesystem::path shm_dir;
    std::shared_ptr<shm::ChannelMeta> meta;
    std::shared_ptr<shm::MultiWriter> writer;
    std::optional<shm::Reader> reader;
    static constexpr uint64_t MB = 1024ull * 1024;

    ReadyShmFixture() {
        static std::atomic<uint64_t> instance_seq{0};
        channel_name = "dz_test_tdacctctrl_" + test::unique_suffix() + "_" +
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
    ~ReadyShmFixture() {
        reader.reset();
        writer.reset();
        meta.reset();
        dztrader::shm::test::cleanup_channel(channel_name, shm_dir);
    }
    ReadyShmFixture(const ReadyShmFixture&) = delete;
    ReadyShmFixture& operator=(const ReadyShmFixture&) = delete;

    struct Captured {
        bool found = false;
        std::string instance_id;
        nlohmann::json payload;
    };

    /// 排空 reader，返回第一个匹配 type + instance_id 的定向帧 payload。
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
};

class TdAccountControllerTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::shared_ptr<Repository> repo_;
    std::shared_ptr<TdAccountRegistry> registry_;
    std::shared_ptr<ProcessMirror> process_mirror_;
    std::shared_ptr<ShmWriter> shm_writer_;
    std::shared_ptr<TdAccountCtrl> ctrl_;
    nlohmann::json primed_gateways_ = nlohmann::json::object();

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_webui_td_account_controller_test");
        std::filesystem::create_directories(dir_);
        // 真 platform.db（Repository ctor 跑 run_schema）——注册表持久层用真 schema。
        repo_ = std::make_shared<Repository>(dir_ / "platform.db");
        repo_->create_user("admin", "Administrator", "admin@test.com", "pass", "admin");
        repo_->create_user("regular", "Regular User", "reg@test.com", "pass", "user");
        registry_ = std::make_shared<TdAccountRegistry>(
            std::make_unique<RepositoryTdAccountRegistryStore>(*repo_));
        process_mirror_ = std::make_shared<ProcessMirror>();
        // 未就绪 writer（临时目录无 dz_shm_event 通道）→ is_ready() false
        shm_writer_ = std::make_shared<ShmWriter>(std::filesystem::temp_directory_path());
        ctrl_ = std::make_shared<TdAccountCtrl>(repo_, registry_, process_mirror_, shm_writer_);
    }

    void TearDown() override {
        ctrl_.reset();
        shm_writer_.reset();
        process_mirror_.reset();
        registry_.reset();
        repo_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
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
        ctrl_ = std::make_shared<TdAccountCtrl>(repo_, registry_, process_mirror_, shm_writer_);
    }

    /// 把一个交易网关置为「已注册 + Running」（预检 2 + 3 通过）。
    /// **累积**：进程配置帧是全量映射（契约 process），多次调用不能互相抹掉——
    /// 那会与"两个网关同时存在"的真实场景不符。
    void prime_gateway(const std::string& gateway) {
        primed_gateways_[gateway] = nlohmann::json{
            {"args", nlohmann::json::array()},
            {"env", nlohmann::json::object()},
            {"restart",
             nlohmann::json{{"enabled", false}, {"max_attempts", 0}, {"backoff_sec", 5}}}};
        process_mirror_->update_process_configs(primed_gateways_);
        dztrader::platform::ProcessStatus status;
        status.name = gateway;
        status.state = dztrader::platform::ChildState::Running;
        process_mirror_->update_status(gateway, status);
    }

    /// 模拟该网关从进程配置中消失（网关被移除，契约 process）。
    void unregister_gateways() {
        primed_gateways_ = nlohmann::json::object();
        process_mirror_->update_process_configs(primed_gateways_);
    }

    /// 把账户直接写进注册表（等价于收到一帧配置回报），不经过 HTTP。
    /// **整组**写入（契约 td-config §4：配置帧始终全量）——多次调用会互相替换，
    /// 需要多个账户时一次传入。
    void seed_account(const std::string& gateway,
                      const std::string& account_id,
                      bool enabled = true) {
        seed_accounts(gateway, {{account_id, enabled}});
    }

    void seed_accounts(const std::string& gateway,
                       std::vector<std::pair<std::string, bool>> accounts) {
        std::vector<TdAccountRegistryEntry> entries;
        entries.reserve(accounts.size());
        for (auto& [id, enabled] : accounts) {
            entries.push_back(
                TdAccountRegistryEntry{.account_id = id, .gateway = gateway, .enabled = enabled});
        }
        registry_->reconcile_gateway(gateway, entries);
    }

    /// 一条 AddAccount 的合法请求体。
    static std::string add_body(const std::string& gateway, const std::string& account_id) {
        return nlohmann::json{
            {"gateway", gateway},
            {"account",
             {{"account_id", account_id},
              {"enabled", true},
              {"risk_control_enabled", false},
              {"currency_id", "CNY"},
              {"ext", {{"broker_id", "9999"}, {"user_id", "00001"}, {"password", "p"}}}}}}
            .dump();
    }
};

// ============================================================================
// GET /api/td/accounts（契约 rest §2.9.9）
// ============================================================================

TEST_F(TdAccountControllerTest, ListDegradesWhenNoSnapshotEverReceived) {
    // 未收到任何配置帧、platform.db 也未恢复出条目 → available:false + reason，**不是**空数组。
    auto resp = invoke(&TdAccountCtrl::list, user_req());
    ASSERT_NE(resp.get(), nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = parse_body(resp);
    EXPECT_FALSE(body.at("available").get<bool>());
    EXPECT_EQ(body.at("reason").get<std::string>(), "no_gateway_snapshot");
    EXPECT_FALSE(body.contains("accounts")) << "degraded body has only available+reason";
}

TEST_F(TdAccountControllerTest, ListIsAvailableForAuthenticatedNonAdmin) {
    // 权限：任意已认证用户（不是 admin，契约 rest §2.9.9）。
    registry_->note_config_frame_received();
    auto resp = invoke(&TdAccountCtrl::list, user_req());
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = parse_body(resp);
    EXPECT_TRUE(body.at("available").get<bool>());
    EXPECT_TRUE(body.at("accounts").is_array());
    EXPECT_TRUE(body.at("accounts").empty());
}

TEST_F(TdAccountControllerTest, ListReturnsRegistryEntriesSortedWithEnabled) {
    seed_accounts("dztd_ctp", {{"acc_b", true}, {"acc_a", false}});

    auto resp = invoke(&TdAccountCtrl::list, user_req());
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = parse_body(resp);
    ASSERT_TRUE(body.at("available").get<bool>());
    ASSERT_EQ(body.at("accounts").size(), 2U);
    // 契约 rest §2.9.9：按 account_id 升序
    EXPECT_EQ(body.at("accounts")[0].at("account_id").get<std::string>(), "acc_a");
    EXPECT_EQ(body.at("accounts")[0].at("gateway").get<std::string>(), "dztd_ctp");
    EXPECT_FALSE(body.at("accounts")[0].at("enabled").get<bool>());
    EXPECT_EQ(body.at("accounts")[1].at("account_id").get<std::string>(), "acc_b");
    EXPECT_TRUE(body.at("accounts")[1].at("enabled").get<bool>());
}

TEST_F(TdAccountControllerTest, ListIsAvailableAfterRestartLoadFromPlatformDb) {
    seed_account("dztd_ctp", "acc_a");
    // 模拟重启：新注册表实例从同一个 platform.db 恢复。
    registry_ = std::make_shared<TdAccountRegistry>(
        std::make_unique<RepositoryTdAccountRegistryStore>(*repo_));
    ASSERT_EQ(registry_->load(), 1U);
    ctrl_ = std::make_shared<TdAccountCtrl>(repo_, registry_, process_mirror_, shm_writer_);

    auto resp = invoke(&TdAccountCtrl::list, user_req());
    const auto body = parse_body(resp);
    EXPECT_TRUE(body.at("available").get<bool>()) << "restored rows make the list trustworthy";
    ASSERT_EQ(body.at("accounts").size(), 1U);
    EXPECT_TRUE(body.at("accounts")[0].at("enabled").get<bool>())
        << "known degradation (rest §2.9.9): enabled is not persisted, defaults to true";
}

// ============================================================================
// POST /api/td/accounts（契约 rest §2.9.7）
// ============================================================================

TEST_F(TdAccountControllerTest, CreateAsNonAdminReturns403WithAdminRequiredCode) {
    auto resp = invoke(&TdAccountCtrl::create, user_req(add_body("dztd_ctp", "acc_a")));
    ASSERT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
    const auto body = parse_body(resp);
    EXPECT_EQ(body.at("error").get<std::string>(), "admin required");
    EXPECT_EQ(body.at("code").get<std::string>(), "admin_required");
}

TEST_F(TdAccountControllerTest, CreateWithUnregisteredGatewayReturns400InvalidGateway) {
    // 形如 dztd_* 但不在进程配置里 → 预检 2 失败（不是 409，也不是 404）。
    auto resp = invoke(&TdAccountCtrl::create, admin_req(add_body("dztd_ctp", "acc_a")));
    ASSERT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    const auto body = parse_body(resp);
    EXPECT_EQ(body.at("code").get<std::string>(), "invalid_gateway");
}

TEST_F(TdAccountControllerTest, CreateWithNonTdGatewayReturns400InvalidGateway) {
    prime_gateway("dzmd_ctp");
    auto resp = invoke(&TdAccountCtrl::create, admin_req(add_body("dzmd_ctp", "acc_a")));
    ASSERT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "invalid_gateway");
}

TEST_F(TdAccountControllerTest, CreateWithStoppedGatewayReturns409GatewayNotRunning) {
    prime_gateway("dztd_ctp");
    dztrader::platform::ProcessStatus status;
    status.name = "dztd_ctp";
    status.state = dztrader::platform::ChildState::Stopped;
    process_mirror_->update_status("dztd_ctp", status);

    auto resp = invoke(&TdAccountCtrl::create, admin_req(add_body("dztd_ctp", "acc_a")));
    ASSERT_EQ(resp->getStatusCode(), drogon::k409Conflict);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "gateway_not_running");
}

TEST_F(TdAccountControllerTest, CreateWithDuplicateAccountReturns409AccountExists) {
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");

    auto resp = invoke(&TdAccountCtrl::create, admin_req(add_body("dztd_ctp", "acc_a")));
    ASSERT_EQ(resp->getStatusCode(), drogon::k409Conflict);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "account_exists");
}

TEST_F(TdAccountControllerTest, CreateRejectsAccountOwnedByAnotherGateway) {
    // 契约 td-config §5：account_id 全局唯一（跨网关）——已禁用/离线账户同样占位。
    prime_gateway("dztd_ctp");
    prime_gateway("dztd_ctp_2");
    seed_account("dztd_ctp_2", "acc_a");

    auto resp = invoke(&TdAccountCtrl::create, admin_req(add_body("dztd_ctp", "acc_a")));
    ASSERT_EQ(resp->getStatusCode(), drogon::k409Conflict);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "account_exists");
}

TEST_F(TdAccountControllerTest, CreateRejectsMissingAccountId) {
    prime_gateway("dztd_ctp");
    const std::string body =
        nlohmann::json{{"gateway", "dztd_ctp"}, {"account", {{"enabled", true}}}}.dump();
    auto resp = invoke(&TdAccountCtrl::create, admin_req(body));
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(TdAccountControllerTest, CreateRejectsAccountIdLongerThan31Bytes) {
    prime_gateway("dztd_ctp");
    const std::string too_long(32, 'x');  // DzAccountId 容量 32，可用 31
    auto resp = invoke(&TdAccountCtrl::create, admin_req(add_body("dztd_ctp", too_long)));
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(TdAccountControllerTest, CreateAcceptsAccountIdExactly31Bytes) {
    ReadyShmFixture shm;
    prime_gateway("dztd_ctp");
    use_ready_shm(shm);
    const std::string max_ok(31, 'x');

    auto resp = invoke(&TdAccountCtrl::create, admin_req(add_body("dztd_ctp", max_ok)));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
}

TEST_F(TdAccountControllerTest, CreateRejectsNonObjectOrMissingFields) {
    prime_gateway("dztd_ctp");
    for (const char* body :
         {"[]", "{}", R"({"gateway":"dztd_ctp"})", R"({"account":{"account_id":"a"}})"}) {
        auto resp = invoke(&TdAccountCtrl::create, admin_req(body));
        EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest) << "body=" << body;
    }
}

TEST_F(TdAccountControllerTest, CreateReturns503WhenWriterNotReady) {
    prime_gateway("dztd_ctp");
    auto resp = invoke(&TdAccountCtrl::create, admin_req(add_body("dztd_ctp", "acc_a")));
    ASSERT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
    // 503 = 请求未被受理（基础设施不可用），不带 §2.9.7 的预检 code。
    EXPECT_FALSE(parse_body(resp).contains("code"));
}

TEST_F(TdAccountControllerTest, CreateDispatchesAddAccountFrame) {
    ReadyShmFixture shm;
    prime_gateway("dztd_ctp");
    use_ready_shm(shm);

    auto resp = invoke(&TdAccountCtrl::create, admin_req(add_body("dztd_ctp", "acc_a")));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_TRUE(parse_body(resp).at("ok").get<bool>());

    const auto cap = shm.capture_inst_frame(DZ_FRAME_TD_REQ_MODIFY_CONFIG, "dztd_ctp");
    ASSERT_TRUE(cap.found) << "expect TD_REQ_MODIFY_CONFIG frame to dztd_ctp";
    EXPECT_EQ(cap.payload.at("op").get<std::string>(), "AddAccount");
    // params = 账户配置对象原样透传（dzweb 不解析 ext，契约 rest §2.9.7 预检 6）
    const auto& params = cap.payload.at("params");
    EXPECT_EQ(params.at("account_id").get<std::string>(), "acc_a");
    EXPECT_EQ(params.at("enabled").get<bool>(), true);
    EXPECT_EQ(params.at("currency_id").get<std::string>(), "CNY");
    EXPECT_EQ(params.at("ext").at("broker_id").get<std::string>(), "9999");
}

// ============================================================================
// PUT /api/td/accounts/{account_id}（契约 rest §2.9.7）
// ============================================================================

TEST_F(TdAccountControllerTest, UpdateAsNonAdminReturns403) {
    auto resp = invoke(&TdAccountCtrl::update, user_req("{}"), std::string("acc_a"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "admin_required");
}

TEST_F(TdAccountControllerTest, UpdateUnknownAccountReturns404AccountNotFound) {
    prime_gateway("dztd_ctp");
    auto resp = invoke(&TdAccountCtrl::update, admin_req("{}"), std::string("nope"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k404NotFound);
    const auto body = parse_body(resp);
    EXPECT_EQ(body.at("error").get<std::string>(), "account not found");
    EXPECT_EQ(body.at("code").get<std::string>(), "account_not_found");
}

TEST_F(TdAccountControllerTest, UpdateResolvesGatewayFromRegistry) {
    // PUT 的 gateway 只能从注册表解析（契约 rest §2.9.7：路径不含 gateway）。
    ReadyShmFixture shm;
    prime_gateway("dztd_ctp_2");
    use_ready_shm(shm);
    seed_account("dztd_ctp_2", "acc_a");

    auto resp =
        invoke(&TdAccountCtrl::update, admin_req(R"({"currency_id":"USD"})"), std::string("acc_a"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto cap = shm.capture_inst_frame(DZ_FRAME_TD_REQ_MODIFY_CONFIG, "dztd_ctp_2");
    ASSERT_TRUE(cap.found) << "frame must go to the gateway resolved from the registry";
    EXPECT_EQ(cap.payload.at("op").get<std::string>(), "UpdateAccount");
}

TEST_F(TdAccountControllerTest, UpdateReturns400WhenGatewayRemoved) {
    // 注册表还留着账户（投影滞后于进程移除），但网关已不在进程配置里 → 预检 2 命中
    // `invalid_gateway`(400) 而非 `gateway_not_running`(409)：这是 PUT/DELETE 预检顺序
    // （admin → 注册表解析 → dztd_* → Running）的必然结果，不是缺陷。
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");
    unregister_gateways();  // 网关已移除
    auto resp = invoke(&TdAccountCtrl::update, admin_req("{}"), std::string("acc_a"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "invalid_gateway");
}

TEST_F(TdAccountControllerTest, UpdateRejectsAccountIdInBody) {
    // 契约 rest §2.9.7：路径是 account_id 的唯一来源，body 出现即 400。
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");
    auto resp =
        invoke(&TdAccountCtrl::update, admin_req(R"({"account_id":"acc_b","currency_id":"USD"})"),
               std::string("acc_a"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(TdAccountControllerTest, UpdateRejectsNonObjectBody) {
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");
    auto resp = invoke(&TdAccountCtrl::update, admin_req("[1,2]"), std::string("acc_a"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(TdAccountControllerTest, UpdateInjectsAccountIdAndDispatchesEmptyObject) {
    // 契约 rest §2.9.7：`{}` → `{"op":"UpdateAccount","params":{"account_id":<路径>}}`。
    ReadyShmFixture shm;
    prime_gateway("dztd_ctp");
    use_ready_shm(shm);
    seed_account("dztd_ctp", "acc_a");

    auto resp = invoke(&TdAccountCtrl::update, admin_req("{}"), std::string("acc_a"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto cap = shm.capture_inst_frame(DZ_FRAME_TD_REQ_MODIFY_CONFIG, "dztd_ctp");
    ASSERT_TRUE(cap.found);
    EXPECT_EQ(cap.payload.at("op").get<std::string>(), "UpdateAccount");
    const nlohmann::json expected = {{"account_id", "acc_a"}};
    EXPECT_EQ(cap.payload.at("params"), expected);
}

TEST_F(TdAccountControllerTest, UpdatePassesThroughFieldsAndExtWithoutValidation) {
    // 预检 6：dzweb 不校验 ext（柜台特有字段由目标网关校验），原样透传。
    ReadyShmFixture shm;
    prime_gateway("dztd_ctp");
    use_ready_shm(shm);
    seed_account("dztd_ctp", "acc_a");

    const std::string body = R"({"enabled":false,"ext":{"password":"****","unknown_field":1}})";
    auto resp = invoke(&TdAccountCtrl::update, admin_req(body), std::string("acc_a"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto cap = shm.capture_inst_frame(DZ_FRAME_TD_REQ_MODIFY_CONFIG, "dztd_ctp");
    ASSERT_TRUE(cap.found);
    const auto& params = cap.payload.at("params");
    EXPECT_EQ(params.at("account_id").get<std::string>(), "acc_a");
    EXPECT_EQ(params.at("enabled").get<bool>(), false);
    EXPECT_EQ(params.at("ext").at("unknown_field").get<int>(), 1);
}

// ============================================================================
// DELETE /api/td/accounts/{account_id}（契约 rest §2.9.7）
// ============================================================================

TEST_F(TdAccountControllerTest, DeleteAsNonAdminReturns403) {
    auto resp = invoke(&TdAccountCtrl::remove, user_req(), std::string("acc_a"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "admin_required");
}

TEST_F(TdAccountControllerTest, DeleteUnknownAccountReturns404AccountNotFound) {
    prime_gateway("dztd_ctp");
    auto resp = invoke(&TdAccountCtrl::remove, admin_req(), std::string("nope"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k404NotFound);
    EXPECT_EQ(parse_body(resp).at("code").get<std::string>(), "account_not_found");
}

TEST_F(TdAccountControllerTest, DeleteDispatchesRemoveAccountFrame) {
    ReadyShmFixture shm;
    prime_gateway("dztd_ctp");
    use_ready_shm(shm);
    seed_account("dztd_ctp", "acc_a");

    auto resp = invoke(&TdAccountCtrl::remove, admin_req(), std::string("acc_a"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_TRUE(parse_body(resp).at("ok").get<bool>());

    const auto cap = shm.capture_inst_frame(DZ_FRAME_TD_REQ_MODIFY_CONFIG, "dztd_ctp");
    ASSERT_TRUE(cap.found);
    EXPECT_EQ(cap.payload.at("op").get<std::string>(), "RemoveAccount");
    const nlohmann::json expected = {{"account_id", "acc_a"}};
    EXPECT_EQ(cap.payload.at("params"), expected);
}

TEST_F(TdAccountControllerTest, DeleteDoesNotRemoveRegistryEntryItself) {
    // 注册表是**投影**：删除以 RTN_TD_CONFIG 到达为准（reconcile 掉该账户），
    // 写端点不在本地改注册表——否则失败回滚会让 UI 显示错误的账户集合。
    ReadyShmFixture shm;
    prime_gateway("dztd_ctp");
    use_ready_shm(shm);
    seed_account("dztd_ctp", "acc_a");

    auto resp = invoke(&TdAccountCtrl::remove, admin_req(), std::string("acc_a"));
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_TRUE(registry_->contains("acc_a"))
        << "registry is frame-driven, not write-endpoint driven";
}

TEST_F(TdAccountControllerTest, DeleteReturns503WhenWriterNotReady) {
    prime_gateway("dztd_ctp");
    seed_account("dztd_ctp", "acc_a");
    auto resp = invoke(&TdAccountCtrl::remove, admin_req(), std::string("acc_a"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
}

TEST_F(TdAccountControllerTest, PrecheckFailureWritesNoFrame) {
    // 契约 rest §2.9.7：预检失败时不写帧（失败不能变成对网关的误操作）。
    ReadyShmFixture shm;
    prime_gateway("dztd_ctp");
    use_ready_shm(shm);
    seed_account("dztd_ctp", "acc_a");

    EXPECT_EQ(
        invoke(&TdAccountCtrl::create, admin_req(add_body("dztd_ctp", "acc_a")))->getStatusCode(),
        drogon::k409Conflict);
    EXPECT_EQ(invoke(&TdAccountCtrl::update, admin_req("{}"), std::string("nope"))->getStatusCode(),
              drogon::k404NotFound);

    const auto cap = shm.capture_inst_frame(DZ_FRAME_TD_REQ_MODIFY_CONFIG, "dztd_ctp");
    EXPECT_FALSE(cap.found) << "rejected requests must not write any frame";
}

}  // namespace
}  // namespace dztrader::webui
