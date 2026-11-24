/**
 * @file td_account_mirror_cleanup_test.cpp
 * @brief 账户级镜像清理（ADR 0056 决策 8；契约 general §9、td-status「镜像」、progress「镜像」）。
 *
 * 断言外部可观察行为：网关停止/崩溃/被移除与账户从配置帧消失时，`td_status` / `progress`
 * 镜像条目**按账户 ID** 消失、其他账户与其他 domain 不受影响、注册表降级时放弃清理
 * （宁残留不猜）。广播不属本组件（调用方用 `data_changed{td}`），故不在此断言。
 */

#include "td_account_mirror_cleanup.h"

#include "mirror_store.h"
#include "td_account_registry.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace dztrader::webui {
namespace {

/// 契约 process 的 `RTN_PROCESS_STATUS` payload 形状。
nlohmann::json process_status(const std::string& name, const std::string& state) {
    return nlohmann::json{{"name", name}, {"state", state}, {"pid", 0}};
}

/// 给某账户挂上两个账户级镜像域（td_status + progress）。
void seed_account_mirror(MirrorStore& mirror, const std::string& account_id) {
    mirror.update(account_id, "td_status", nlohmann::json{{"state", "Ready"}});
    mirror.update(account_id, "progress", nlohmann::json{{"current", 10}});
}

class TdAccountMirrorCleanupTest : public ::testing::Test {
protected:
    MirrorStore mirror_;
    TdAccountRegistry registry_;
    TdAccountMirrorCleanup cleanup_{mirror_, registry_};

    void reconcile(const std::string& gateway, const std::vector<std::string>& account_ids) {
        std::vector<TdAccountRegistryEntry> entries;
        entries.reserve(account_ids.size());
        for (const auto& id : account_ids) {
            entries.push_back(TdAccountRegistryEntry{
                .account_id = id, .gateway = gateway, .account_name = "", .enabled = true});
        }
        registry_.reconcile_gateway(gateway, entries);
    }

    bool has_account_mirror(const std::string& account_id) const {
        const auto instance = mirror_.instance(account_id);
        return instance.contains("td_status") || instance.contains("progress");
    }
};

// ============================================================================
// 网关停止 / 崩溃（进程状态帧）
// ============================================================================

TEST_F(TdAccountMirrorCleanupTest, StoppedGatewayClearsItsAccountsMirrors) {
    reconcile("dztd_ctp", {"acc_a", "acc_b"});
    seed_account_mirror(mirror_, "acc_a");
    seed_account_mirror(mirror_, "acc_b");

    const size_t cleared = cleanup_.on_process_status(process_status("dztd_ctp", "Stopped"));

    EXPECT_EQ(cleared, 2U);
    EXPECT_FALSE(has_account_mirror("acc_a"));
    EXPECT_FALSE(has_account_mirror("acc_b"));
}

TEST_F(TdAccountMirrorCleanupTest, CrashedGatewayClearsItsAccountsMirrors) {
    // 崩溃路径与正常停止同权：崩溃前残留的 "Ready" 会让前端显示在线假象。
    reconcile("dztd_ctp", {"acc_a"});
    seed_account_mirror(mirror_, "acc_a");

    EXPECT_EQ(cleanup_.on_process_status(process_status("dztd_ctp", "Crashed")), 1U);
    EXPECT_FALSE(has_account_mirror("acc_a"));
}

TEST_F(TdAccountMirrorCleanupTest, OtherGatewaysAndProcessesAreUntouched) {
    reconcile("dztd_ctp", {"acc_a"});
    reconcile("dztd_ctp_2", {"acc_x"});
    seed_account_mirror(mirror_, "acc_a");
    seed_account_mirror(mirror_, "acc_x");

    EXPECT_EQ(cleanup_.on_process_status(process_status("dztd_ctp", "Stopped")), 1U);
    EXPECT_FALSE(has_account_mirror("acc_a"));
    EXPECT_TRUE(has_account_mirror("acc_x")) << "other gateway's accounts must survive";
}

TEST_F(TdAccountMirrorCleanupTest, RunningAndStoppingDoNotClear) {
    reconcile("dztd_ctp", {"acc_a"});
    seed_account_mirror(mirror_, "acc_a");

    EXPECT_EQ(cleanup_.on_process_status(process_status("dztd_ctp", "Running")), 0U);
    EXPECT_EQ(cleanup_.on_process_status(process_status("dztd_ctp", "Stopping")), 0U);
    EXPECT_TRUE(has_account_mirror("acc_a")) << "still-running gateway keeps its account mirrors";
}

TEST_F(TdAccountMirrorCleanupTest, NonTdProcessStoppedIsIgnored) {
    // 账户级镜像只挂交易网关账户：行情进程停止不触发账户级清理（行情进程的 progress
    // 由 ProcessMirror::mark_stale 按进程名清理，那是另一条路径）。
    reconcile("dztd_ctp", {"acc_a"});
    seed_account_mirror(mirror_, "acc_a");
    mirror_.update("dzmd_ctp", "progress", nlohmann::json{{"current", 1}});

    EXPECT_EQ(cleanup_.on_process_status(process_status("dzmd_ctp", "Stopped")), 0U);
    EXPECT_TRUE(has_account_mirror("acc_a"));
    EXPECT_TRUE(mirror_.instance("dzmd_ctp").contains("progress"));
}

TEST_F(TdAccountMirrorCleanupTest, MalformedPayloadIsIgnored) {
    reconcile("dztd_ctp", {"acc_a"});
    seed_account_mirror(mirror_, "acc_a");

    EXPECT_EQ(cleanup_.on_process_status(nlohmann::json::array()), 0U);
    EXPECT_EQ(cleanup_.on_process_status(nlohmann::json{{"name", "dztd_ctp"}}), 0U);
    EXPECT_TRUE(has_account_mirror("acc_a"));
}

TEST_F(TdAccountMirrorCleanupTest, DegradedRegistrySkipsCleanup) {
    // AC5 / 契约 general §9：注册表降级（尚无可信快照）时**放弃清理**——宁残留不猜。
    seed_account_mirror(mirror_, "acc_a");
    ASSERT_FALSE(registry_.available());

    EXPECT_EQ(cleanup_.on_process_status(process_status("dztd_ctp", "Stopped")), 0U);
    EXPECT_TRUE(has_account_mirror("acc_a")) << "degraded registry must not guess (leave residue)";
}

TEST_F(TdAccountMirrorCleanupTest, RepeatedCleanupIsIdempotent) {
    reconcile("dztd_ctp", {"acc_a"});
    seed_account_mirror(mirror_, "acc_a");

    EXPECT_EQ(cleanup_.on_process_status(process_status("dztd_ctp", "Stopped")), 1U);
    EXPECT_EQ(cleanup_.on_process_status(process_status("dztd_ctp", "Stopped")), 0U);
}

// ============================================================================
// 网关被移除（RTN_PROCESS_CONFIG 条目消失）
// ============================================================================

TEST_F(TdAccountMirrorCleanupTest, DepartedGatewayAccountsAreCleared) {
    reconcile("dztd_ctp", {"acc_a"});
    reconcile("dztd_ctp_2", {"acc_x"});
    seed_account_mirror(mirror_, "acc_a");
    seed_account_mirror(mirror_, "acc_x");

    const size_t cleared = cleanup_.erase_departed_gateways({"dztd_ctp_2"});

    EXPECT_EQ(cleared, 1U);
    EXPECT_FALSE(has_account_mirror("acc_a")) << "removed gateway's accounts lose their mirrors";
    EXPECT_TRUE(has_account_mirror("acc_x"));
}

TEST_F(TdAccountMirrorCleanupTest, DepartedGatewaysCleanupSkipsWhenRegistryDegraded) {
    seed_account_mirror(mirror_, "acc_a");
    ASSERT_FALSE(registry_.available());

    EXPECT_EQ(cleanup_.erase_departed_gateways({}), 0U);
    EXPECT_TRUE(has_account_mirror("acc_a"));
}

// ============================================================================
// 账户从配置帧消失
// ============================================================================

TEST_F(TdAccountMirrorCleanupTest, UnregisteredCandidatesAreClearedAndRegisteredKept) {
    // reconcile 前拿到账户全集，reconcile 后把"已不在注册表"的那些清掉。
    reconcile("dztd_ctp", {"acc_a"});
    seed_account_mirror(mirror_, "acc_a");
    seed_account_mirror(mirror_, "acc_b");

    const size_t cleared = cleanup_.erase_unregistered({"acc_a", "acc_b"});

    EXPECT_EQ(cleared, 1U);
    EXPECT_TRUE(has_account_mirror("acc_a")) << "still-registered account keeps its mirror";
    EXPECT_FALSE(has_account_mirror("acc_b")) << "vanished account loses its mirror";
}

// ============================================================================
// domain 边界：只清 td_status / progress，不碰配置镜像等其他 domain
// ============================================================================

TEST_F(TdAccountMirrorCleanupTest, OnlyAccountLevelDomainsAreTouched) {
    reconcile("dztd_ctp", {"acc_a"});
    seed_account_mirror(mirror_, "acc_a");
    // 同名实例上的其他 domain（防御性：理论上账户 ID 不挂这些域，但清理不得误删）
    mirror_.update("acc_a", "td_config", nlohmann::json{{"qry_min_interval_ms", 1100}});

    EXPECT_EQ(cleanup_.erase_accounts({"acc_a"}), 1U);

    const auto instance = mirror_.instance("acc_a");
    EXPECT_FALSE(instance.contains("td_status"));
    EXPECT_FALSE(instance.contains("progress"));
    EXPECT_TRUE(instance.contains("td_config")) << "config domain belongs to the process level";
}

TEST_F(TdAccountMirrorCleanupTest, AccountsWithoutMirrorEntriesAreNotCounted) {
    reconcile("dztd_ctp", {"acc_a"});
    // 账户在注册表但镜像从未到达（网关刚起、还没推状态）：不算清理。
    EXPECT_EQ(cleanup_.on_process_status(process_status("dztd_ctp", "Stopped")), 0U);
}

}  // namespace
}  // namespace dztrader::webui
