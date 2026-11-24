/**
 * @file td_domain_config_status_test.cpp
 * @brief 交易网关配置/细状态领域服务（契约 td-config §4/§5/§8、td-status §3、
 *        webui-ws §2.1/§2.2）。
 *
 * 断言外部可观察行为：镜像 domain 与 key、WS 消息 type / instance_id / payload、
 * 配置帧触发的注册表 reconcile（含 account_name）与 `data_changed{td}` 广播、
 * 账户从配置帧消失时的账户级镜像清理、异常 payload 不破坏注册表。
 */

#include "td_account_mirror_cleanup.h"
#include "td_config_domain_service.h"
#include "td_account_registry.h"
#include "td_status_domain_service.h"
#include "fake_data_change_notifier.h"
#include "mirror_store.h"
#include "ws_broadcaster.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace dztrader::webui {
namespace {

/// 记录广播的 WsBroadcaster 假实现（与 `domain_service_test.cpp` 同型）。
class RecordingBroadcaster : public WsBroadcaster {
public:
    struct Msg {
        std::string type;
        std::string instance_id;
        nlohmann::json data;
    };
    std::vector<Msg> messages;

    void broadcast(const std::string& type,
                   const std::string& instance_id,
                   const nlohmann::json& data) override {
        messages.push_back({.type = type, .instance_id = instance_id, .data = data});
    }
};

/// 一帧契约 td-config §2.4 形态的配置 payload（dp 侧最终形态：通用层 + ext）。
nlohmann::json config_payload(std::vector<std::pair<std::string, bool>> accounts) {
    nlohmann::json list = nlohmann::json::array();
    for (auto& [account_id, enabled] : accounts) {
        list.push_back(nlohmann::json{{"account_id", account_id},
                                      {"account_name", ""},
                                      {"enabled", enabled},
                                      {"risk_control_enabled", false},
                                      {"currency_id", "CNY"},
                                      {"ext",
                                       {{"broker_id", "9999"},
                                        {"password", "****"},
                                        {"product_info", ""},
                                        {"frontends", nlohmann::json::array()},
                                        {"auth_code", ""},
                                        {"app_id", ""},
                                        {"flow_dir", ""}}}});
    }
    return nlohmann::json{
        {"qry_min_interval_ms", 1100}, {"ext", {{"enable_lock_mode", true}}}, {"accounts", list}};
}

class TdConfigDomainServiceTest : public ::testing::Test {
protected:
    MirrorStore mirror_;
    RecordingBroadcaster ws_;
    FakeDataChangeNotifier notifier_;
    std::unique_ptr<TdAccountRegistry> registry_ = std::make_unique<TdAccountRegistry>();
    std::unique_ptr<TdAccountMirrorCleanup> cleanup_;
    std::unique_ptr<TdConfigDomainService> service_;

    void SetUp() override {
        cleanup_ = std::make_unique<TdAccountMirrorCleanup>(mirror_, *registry_);
        service_ =
            std::make_unique<TdConfigDomainService>(mirror_, *registry_, ws_, notifier_, *cleanup_);
    }

    void TearDown() override {
        service_.reset();
        cleanup_.reset();
    }
};

// ============================================================================
// 镜像 + WS 广播（契约 td-config §8 / webui-ws §2.1/§2.2）
// ============================================================================

TEST_F(TdConfigDomainServiceTest, WritesMirrorUnderGatewayKeyAndBroadcasts) {
    const auto payload = config_payload({{"acc_a", true}});
    service_->on_rtn_td_config("dztd_ctp", payload);

    // 镜像 domain `td_config`，key = 网关进程名（契约 webui-ws §2.1）
    const auto instance = mirror_.instance("dztd_ctp");
    ASSERT_TRUE(instance.contains("td_config"));
    EXPECT_EQ(instance.at("td_config"), payload) << "payload is passed through verbatim";

    ASSERT_EQ(ws_.messages.size(), 1U);
    const auto& msg = ws_.messages[0];
    EXPECT_EQ(msg.type, "td_rtn_config");
    EXPECT_EQ(msg.instance_id, "dztd_ctp") << "webui-ws §2.2: instance_id = gateway process name";
    EXPECT_EQ(msg.data, payload);
}

TEST_F(TdConfigDomainServiceTest, ConfigPayloadIsNotRewritten) {
    // dzweb 不解析/不改写柜台特有字段：前端自己按 interface_type 取字段（契约 td-config §8）。
    auto payload = config_payload({{"acc_a", true}});
    payload["accounts"][0]["ext"]["unknown_field"] = 42;
    service_->on_rtn_td_config("dztd_ctp", payload);

    const nlohmann::json broadcast = ws_.messages.at(0).data;
    EXPECT_EQ(broadcast.at("accounts").at(0).at("ext").at("unknown_field").get<int>(), 42);
    EXPECT_EQ(mirror_.instance("dztd_ctp").at("td_config"), broadcast);
}

TEST_F(TdConfigDomainServiceTest, LaterFrameOverwritesMirrorEntirely) {
    // 契约 td-config §4：始终全量，后到覆盖先到（不是 merge）。
    service_->on_rtn_td_config("dztd_ctp", config_payload({{"acc_a", true}, {"acc_b", true}}));
    service_->on_rtn_td_config("dztd_ctp", config_payload({{"acc_a", true}}));

    const nlohmann::json accounts = mirror_.instance("dztd_ctp").at("td_config").at("accounts");
    ASSERT_EQ(accounts.size(), 1U);
    EXPECT_EQ(accounts[0].at("account_id").get<std::string>(), "acc_a");
}

TEST_F(TdConfigDomainServiceTest, DifferentGatewaysKeepSeparateMirrorDomains) {
    service_->on_rtn_td_config("dztd_ctp", config_payload({{"acc_a", true}}));
    service_->on_rtn_td_config("dztd_ctp_2", config_payload({{"acc_b", true}}));

    EXPECT_EQ(mirror_.instance("dztd_ctp").at("td_config").at("accounts").size(), 1U);
    EXPECT_EQ(mirror_.instance("dztd_ctp_2").at("td_config").at("accounts").size(), 1U);
    EXPECT_EQ(ws_.messages.at(1).instance_id, "dztd_ctp_2");
}

// ============================================================================
// 注册表 reconcile（契约 td-config §5）
// ============================================================================

TEST_F(TdConfigDomainServiceTest, ReconcilesRegistryAndBroadcastsDataChanged) {
    auto payload = config_payload({{"acc_a", true}, {"acc_b", false}});
    payload["accounts"][0]["account_name"] = "我的主账户";
    service_->on_rtn_td_config("dztd_ctp", payload);

    EXPECT_EQ(registry_->size(), 2U);
    const auto acc_a = registry_->find("acc_a");
    ASSERT_TRUE(acc_a.has_value()) << "acc_a must be reconciled into the registry";
    EXPECT_EQ(acc_a->gateway, "dztd_ctp");
    EXPECT_EQ(acc_a->account_name, "我的主账户")
        << "account_name is part of the registry (ADR 0056)";
    EXPECT_TRUE(acc_a->enabled);
    const auto acc_b = registry_->find("acc_b");
    ASSERT_TRUE(acc_b.has_value()) << "acc_b must be reconciled into the registry";
    EXPECT_EQ(acc_b->account_name, "");
    EXPECT_FALSE(acc_b->enabled);
    // 账户集合增减 → data_changed{td}（契约 webui-ws §2.3 / rest §2.9.7 可观察效果）
    ASSERT_EQ(notifier_.scopes.size(), 1U);
    EXPECT_EQ(notifier_.scopes[0], "td");
}

TEST_F(TdConfigDomainServiceTest, MissingAccountNameDefaultsToEmpty) {
    // 契约 td-config §2.2：account_name 选填，缺失 = 未设置（空串，前端回落 account_id）。
    nlohmann::json payload = config_payload({});
    payload["accounts"] = nlohmann::json::array({nlohmann::json{{"account_id", "acc_a"}}});
    service_->on_rtn_td_config("dztd_ctp", payload);

    ASSERT_TRUE(registry_->find("acc_a").has_value());
    EXPECT_EQ(registry_->find("acc_a")->account_name, "");
}

TEST_F(TdConfigDomainServiceTest, AccountNameOnlyChangeDoesNotForceTransactionReload) {
    // 改名不动交易数据基线（契约 webui-ws §2.3 的 data_changed{td} 口径）。
    auto payload = config_payload({{"acc_a", true}});
    service_->on_rtn_td_config("dztd_ctp", payload);
    notifier_.scopes.clear();

    payload["accounts"][0]["account_name"] = "新名字";
    service_->on_rtn_td_config("dztd_ctp", payload);

    EXPECT_EQ(registry_->find("acc_a")->account_name, "新名字");
    EXPECT_TRUE(notifier_.scopes.empty());
}

TEST_F(TdConfigDomainServiceTest, AccountRemovedByConfigFrameClearsAccountLevelMirror) {
    // AC6 / ADR 0056 决策 8：账户从配置帧消失 → 清其 td_status / progress 镜像。
    service_->on_rtn_td_config("dztd_ctp", config_payload({{"acc_a", true}, {"acc_b", true}}));
    mirror_.update("acc_a", "td_status", nlohmann::json{{"state", "Ready"}});
    mirror_.update("acc_a", "progress", nlohmann::json{{"current", 10}});
    mirror_.update("acc_b", "td_status", nlohmann::json{{"state", "Ready"}});
    mirror_.update("acc_b", "progress", nlohmann::json{{"current", 10}});
    notifier_.scopes.clear();

    // 第二帧只含 acc_a：acc_b 已从配置消失。
    service_->on_rtn_td_config("dztd_ctp", config_payload({{"acc_a", true}}));

    const auto removed = mirror_.instance("acc_b");
    EXPECT_FALSE(removed.contains("td_status")) << "vanished account must lose its status mirror";
    EXPECT_FALSE(removed.contains("progress")) << "vanished account must lose its progress mirror";
    const auto kept = mirror_.instance("acc_a");
    EXPECT_TRUE(kept.contains("td_status")) << "still-configured account keeps its mirror";
    EXPECT_TRUE(kept.contains("progress"));
    // 账户集合变化仍触发交易数据基线重拉。
    ASSERT_EQ(notifier_.scopes.size(), 1U);
    EXPECT_EQ(notifier_.scopes[0], "td");
}

TEST_F(TdConfigDomainServiceTest, AccountReassignedToAnotherGatewayKeepsMirror) {
    // 跨网关改判归属（手改配置造成）时账户**仍在**注册表 → 不清理其账户级镜像（只是归属变了）。
    service_->on_rtn_td_config("dztd_ctp", config_payload({{"acc_a", true}}));
    mirror_.update("acc_a", "td_status", nlohmann::json{{"state", "Ready"}});

    service_->on_rtn_td_config("dztd_ctp_2", config_payload({{"acc_a", true}}));

    ASSERT_TRUE(registry_->find("acc_a").has_value());
    EXPECT_EQ(registry_->find("acc_a")->gateway, "dztd_ctp_2");
    EXPECT_TRUE(mirror_.instance("acc_a").contains("td_status"));
}

TEST_F(TdConfigDomainServiceTest, RegistryIgnoresExtEntirely) {
    // 注册表只读通用层（account_id / gateway / enabled），不读 ext。
    auto payload = config_payload({{"acc_a", true}});
    payload["accounts"][0].erase("ext");
    service_->on_rtn_td_config("dztd_ctp", payload);

    ASSERT_TRUE(registry_->find("acc_a").has_value());
    EXPECT_EQ(registry_->find("acc_a")->gateway, "dztd_ctp");
}

TEST_F(TdConfigDomainServiceTest, MissingEnabledDefaultsToTrue) {
    // 契约 td-config §2.2：enabled 默认 true——不把账户误标为禁用。
    nlohmann::json payload = config_payload({});
    payload["accounts"] = nlohmann::json::array({nlohmann::json{{"account_id", "acc_a"}}});
    service_->on_rtn_td_config("dztd_ctp", payload);

    ASSERT_TRUE(registry_->find("acc_a").has_value());
    EXPECT_TRUE(registry_->find("acc_a")->enabled);
}

TEST_F(TdConfigDomainServiceTest, NoDataChangedWhenAccountSetUnchanged) {
    service_->on_rtn_td_config("dztd_ctp", config_payload({{"acc_a", true}}));
    notifier_.scopes.clear();

    service_->on_rtn_td_config("dztd_ctp", config_payload({{"acc_a", true}}));

    EXPECT_TRUE(notifier_.scopes.empty()) << "unchanged account set must not force UI reloads";
}

TEST_F(TdConfigDomainServiceTest, EnabledFlipBroadcastsDataChanged) {
    service_->on_rtn_td_config("dztd_ctp", config_payload({{"acc_a", true}}));
    notifier_.scopes.clear();

    service_->on_rtn_td_config("dztd_ctp", config_payload({{"acc_a", false}}));

    ASSERT_EQ(notifier_.scopes.size(), 1U)
        << "enabled is part of the read-endpoint response, so its change is observable";
    EXPECT_EQ(notifier_.scopes[0], "td");
}

TEST_F(TdConfigDomainServiceTest, SnapshotFlagSetEvenWhenAccountsMissing) {
    // snapshot 标志先于 reconcile 置位（契约 rest §2.9.9 的降级判定）。
    service_->on_rtn_td_config("dztd_ctp", nlohmann::json{{"qry_min_interval_ms", 1100}});

    EXPECT_TRUE(registry_->has_snapshot());
    EXPECT_TRUE(registry_->available());
}

TEST_F(TdConfigDomainServiceTest, MalformedAccountsDoesNotWipeRegistry) {
    // 缺 accounts 数组 → 不 reconcile（用空集合 reconcile 会把该网关账户从 UI 抹掉）。
    service_->on_rtn_td_config("dztd_ctp", config_payload({{"acc_a", true}}));
    notifier_.scopes.clear();

    service_->on_rtn_td_config("dztd_ctp", nlohmann::json{{"accounts", "not an array"}});
    service_->on_rtn_td_config("dztd_ctp", nlohmann::json::array());

    EXPECT_TRUE(registry_->contains("acc_a")) << "a broken frame must not erase known accounts";
    EXPECT_TRUE(notifier_.scopes.empty());
}

TEST_F(TdConfigDomainServiceTest, BadAccountElementsAreSkippedIndividually) {
    nlohmann::json payload = config_payload({});
    payload["accounts"] = nlohmann::json::array({
        nlohmann::json{{"account_id", ""}},        // 空 id → 跳过
        nlohmann::json{{"enabled", true}},         // 缺 id → 跳过
        nlohmann::json{{"account_id", 42}},        // 类型不符 → 跳过
        "not an object",                           // 非对象 → 跳过
        nlohmann::json{{"account_id", "acc_ok"}},  // 有效
    });
    service_->on_rtn_td_config("dztd_ctp", payload);

    EXPECT_EQ(registry_->size(), 1U);
    EXPECT_TRUE(registry_->contains("acc_ok"));
}

TEST_F(TdConfigDomainServiceTest, StillBroadcastsWhenAccountsUnusable) {
    // 即使注册表没更新，配置镜像与 WS 推送照常（契约 td-config §4 无条件）。
    service_->on_rtn_td_config("dztd_ctp", nlohmann::json{{"qry_min_interval_ms", 1100}});

    ASSERT_EQ(ws_.messages.size(), 1U);
    EXPECT_EQ(ws_.messages[0].type, "td_rtn_config");
    EXPECT_TRUE(mirror_.instance("dztd_ctp").contains("td_config"));
}

// ============================================================================
// 细状态领域服务（契约 td-status §3 / webui-ws §2.2）
// ============================================================================

TEST(TdStatusDomainServiceTest, WritesMirrorUnderAccountIdKeyAndBroadcasts) {
    MirrorStore mirror;
    RecordingBroadcaster ws;
    TdStatusDomainService service(mirror, ws);

    const nlohmann::json status = {
        {"account_id", "00001"},
        {"state", "Ready"},
        {"date", "20260808"},
        {"login_time", "2026-08-08 08:45:32"},
        {"progress_desc", "ready"},
        {"progress_min", 0},
        {"progress_max", 10},
        {"progress_current", 10},
        {"ext", {{"api_version", "v6.7.2"}, {"sys_version", "v6.7.2_20240105"}}},
    };
    service.on_rtn_td_status("00001", status);

    // 镜像 domain `td_status`，key = 账户 ID（契约 webui-ws §2.1 / 总则 §5：无复合键）。
    const auto instance = mirror.instance("00001");
    ASSERT_TRUE(instance.contains("td_status"));
    EXPECT_EQ(instance.at("td_status"), status);

    ASSERT_EQ(ws.messages.size(), 1U);
    EXPECT_EQ(ws.messages[0].type, "td_rtn_status");
    EXPECT_EQ(ws.messages[0].instance_id, "00001")
        << "webui-ws §2.2: instance_id = account id (no gateway prefix)";
}

TEST(TdStatusDomainServiceTest, LaterFrameOverwritesEntireStatus) {
    // 契约 td-status §3：始终全量，前端直接整体覆盖。
    MirrorStore mirror;
    RecordingBroadcaster ws;
    TdStatusDomainService service(mirror, ws);

    service.on_rtn_td_status("acc_a",
                             nlohmann::json{{"account_id", "acc_a"}, {"date", "20260808"}});
    service.on_rtn_td_status("acc_a", nlohmann::json{{"account_id", "acc_a"}, {"state", "Idle"}});

    const nlohmann::json stored = mirror.instance("acc_a").at("td_status");
    EXPECT_EQ(stored.at("state").get<std::string>(), "Idle");
    EXPECT_FALSE(stored.contains("date")) << "full overwrite: absent fields are gone, not retained";
}

TEST(TdStatusDomainServiceTest, PerAccountKeysDoNotCollide) {
    MirrorStore mirror;
    RecordingBroadcaster ws;
    TdStatusDomainService service(mirror, ws);

    service.on_rtn_td_status("acc_a", nlohmann::json{{"account_id", "acc_a"}});
    service.on_rtn_td_status("acc_b", nlohmann::json{{"account_id", "acc_b"}});

    EXPECT_TRUE(mirror.instance("acc_a").contains("td_status"));
    EXPECT_TRUE(mirror.instance("acc_b").contains("td_status"));
    EXPECT_NE(ws.messages[0].instance_id, ws.messages[1].instance_id);
}

TEST(TdStatusDomainServiceTest, EmptyInstanceIdIsDropped) {
    // instance_id 由发送方保证（契约 td-status §4）：缺失时无处挂镜像，不写不广播。
    MirrorStore mirror;
    RecordingBroadcaster ws;
    TdStatusDomainService service(mirror, ws);

    service.on_rtn_td_status("", nlohmann::json{{"account_id", "acc_a"}});

    EXPECT_TRUE(ws.messages.empty());
    EXPECT_TRUE(mirror.snapshot().empty());
}

}  // namespace
}  // namespace dztrader::webui
