/**
 * @file td_account_registry_test.cpp
 * @brief 交易账户注册表（契约 td-config §5 / rest §2.9.9，ADR 0056）。
 *
 * 断言外部可观察行为：reconcile 整组替换语义、跨网关重复的 CRITICAL + `NOTIFY_UI` 告警
 * （不阻断）、重启加载、写失败不阻塞、网关移除清理、`no_gateway_snapshot` 降级判定、
 * `account_name` 持久化与旧库加列迁移。**用真 platform.db schema + 真 SQLite 临时库**覆盖
 * 持久层本身（fake store 只用于注入写失败，见 `fake_td_account_registry_store.h`）。
 */

#include "td_account_registry.h"
#include "fake_td_account_registry_store.h"
#include "ready_shm_fixture.h"
#include "test_temp_dir.h"

#include "db_init.h"
#include <dztrader/core/core_data_type.h>
#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace dztrader::webui {
namespace {

std::vector<TdAccountRegistryEntry> accounts(std::vector<std::pair<std::string, bool>> pairs) {
    std::vector<TdAccountRegistryEntry> result;
    result.reserve(pairs.size());
    for (auto& [id, enabled] : pairs) {
        result.push_back(TdAccountRegistryEntry{
            .account_id = id, .gateway = "", .account_name = "", .enabled = enabled});
    }
    return result;
}

/// 带展示名的条目（account_id, account_name, enabled）。
std::vector<TdAccountRegistryEntry> named_accounts(
    std::vector<std::tuple<std::string, std::string, bool>> triples) {
    std::vector<TdAccountRegistryEntry> result;
    result.reserve(triples.size());
    for (auto& [id, name, enabled] : triples) {
        result.push_back(TdAccountRegistryEntry{
            .account_id = id, .gateway = "", .account_name = name, .enabled = enabled});
    }
    return result;
}

// ============================================================================
// reconcile 语义（契约 td-config §5：帧为权威的整组替换）
// ============================================================================

TEST(TdAccountRegistryTest, ReconcileBuildsEntriesFromFrame) {
    TdAccountRegistry registry;
    const bool changed =
        registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}, {"acc_b", false}}));

    EXPECT_TRUE(changed);
    ASSERT_EQ(registry.size(), 2U);
    EXPECT_EQ(registry.find("acc_a")->gateway, "dztd_ctp");
    EXPECT_TRUE(registry.find("acc_a")->enabled);
    EXPECT_FALSE(registry.find("acc_b")->enabled);
    EXPECT_FALSE(registry.find("nope").has_value());
}

TEST(TdAccountRegistryTest, ReconcileIsWholeGroupReplacePerGateway) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}, {"acc_b", true}}));
    // 第二帧只含 acc_a：acc_b 已从该网关移除 → 注册表里必须消失（不是"合并"）。
    const bool changed = registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));

    EXPECT_TRUE(changed);
    EXPECT_EQ(registry.size(), 1U);
    EXPECT_TRUE(registry.contains("acc_a"));
    EXPECT_FALSE(registry.contains("acc_b"));
}

TEST(TdAccountRegistryTest, ReconcileDoesNotTouchOtherGateways) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    registry.reconcile_gateway("dztd_ctp_2", accounts({{"acc_x", true}}));

    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}, {"acc_b", true}}));

    EXPECT_TRUE(registry.contains("acc_a"));
    EXPECT_TRUE(registry.contains("acc_b"));
    EXPECT_TRUE(registry.contains("acc_x")) << "other gateway's account must survive";
    EXPECT_EQ(registry.find("acc_x")->gateway, "dztd_ctp_2");
}

TEST(TdAccountRegistryTest, ReconcileReportsNoChangeWhenIdentical) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    EXPECT_FALSE(registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}})))
        << "identical frame must not report a change (drives data_changed{td})";
    // enabled 翻转是变化（读端点的 enabled 列变了）。
    EXPECT_TRUE(registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", false}})));
}

TEST(TdAccountRegistryTest, ReconcileCarriesAccountName) {
    // 契约 td-config §5：注册表是账户显示名的唯一来源（ADR 0056 决策 9）。
    TdAccountRegistry registry;
    registry.reconcile_gateway(
        "dztd_ctp", named_accounts({{"acc_a", "我的主账户", true}, {"acc_b", "", true}}));

    ASSERT_TRUE(registry.find("acc_a").has_value());
    EXPECT_EQ(registry.find("acc_a")->account_name, "我的主账户");
    EXPECT_EQ(registry.find("acc_b")->account_name, "") << "empty = not set (fallback to id)";
}

TEST(TdAccountRegistryTest, AccountNameOnlyChangeIsNotADataChangedTrigger) {
    // 契约 webui-ws §2.3 的 data_changed{td} = 账户集合 / enabled 变化（交易数据基线失效）；
    // 改名不动交易数据，前端由配置帧更新名字 → 不得触发基线重拉。
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", named_accounts({{"acc_a", "旧名字", true}}));
    EXPECT_FALSE(
        registry.reconcile_gateway("dztd_ctp", named_accounts({{"acc_a", "新名字", true}})));
    EXPECT_EQ(registry.find("acc_a")->account_name, "新名字");
}

TEST(TdAccountRegistryTest, ReconcileSelfHealsCrossGatewayDuplicate) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    // 同一 account_id 出现在另一网关的帧里：帧为权威 → 改判归属（契约 td-config §5 全局唯一）。
    registry.reconcile_gateway("dztd_ctp_2", accounts({{"acc_a", true}}));

    ASSERT_TRUE(registry.find("acc_a").has_value());
    EXPECT_EQ(registry.find("acc_a")->gateway, "dztd_ctp_2");
    EXPECT_EQ(registry.size(), 1U) << "account_id is a global key, not per-gateway";
}

TEST(TdAccountRegistryTest, CrossGatewayDuplicateEmitsNotifyUiPopupAndDoesNotBlock) {
    // AC7 / ADR 0056：同一 account_id 出现在两个网关 → CRITICAL + NOTIFY_UI 弹窗，
    // 不阻断、不自动登出（帧为权威，归属改判，reconcile 照常返回变化）。
    dztrader::test::ReadyShmFixture shm;
    TdAccountRegistry registry(nullptr, shm.writer.get());
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));

    const bool changed = registry.reconcile_gateway("dztd_ctp_2", accounts({{"acc_a", true}}));

    EXPECT_TRUE(changed) << "duplicate must not block the frame-driven reconcile";
    EXPECT_EQ(registry.find("acc_a")->gateway, "dztd_ctp_2");
    const auto popup = shm.capture_ext_frame(DZ_FRAME_NOTIFY_UI);
    ASSERT_TRUE(popup.found) << "user must be told about the duplicate (NOTIFY_UI)";
    EXPECT_EQ(popup.payload.at("level").get<std::string>(), "error");
    EXPECT_TRUE(popup.payload.at("popup").get<bool>()) << "契约 notify-ui: popup must interrupt";
    EXPECT_NE(popup.payload.at("message").get<std::string>().find("acc_a"), std::string::npos);
    EXPECT_FALSE(popup.payload.at("source").get<std::string>().empty());
}

TEST(TdAccountRegistryTest, NoNotifyUiWhenSameGatewayReconciles) {
    dztrader::test::ReadyShmFixture shm;
    TdAccountRegistry registry(nullptr, shm.writer.get());
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));

    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", false}}));

    EXPECT_FALSE(shm.capture_ext_frame(DZ_FRAME_NOTIFY_UI).found)
        << "same-gateway reconcile (even enabled flip) is normal operation, not a duplicate";
}

TEST(TdAccountRegistryTest, ReconcileSkipsEmptyAccountId) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"", true}, {"acc_a", true}}));
    EXPECT_EQ(registry.size(), 1U);
    EXPECT_TRUE(registry.contains("acc_a"));
}

TEST(TdAccountRegistryTest, ReconcileWithEmptyGatewayIsIgnored) {
    TdAccountRegistry registry;
    EXPECT_FALSE(registry.reconcile_gateway("", accounts({{"acc_a", true}})));
    EXPECT_TRUE(registry.entries().empty());
}

TEST(TdAccountRegistryTest, EntriesAreSortedByAccountId) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"zz", true}, {"aa", true}, {"mm", true}}));
    std::vector<std::string> ids;
    for (const auto& entry : registry.entries()) {
        ids.push_back(entry.account_id);
    }
    EXPECT_EQ(ids, (std::vector<std::string>{"aa", "mm", "zz"}));
}

// ============================================================================
// 按网关判空（契约 rest §2.10 预检 5：DELETE 的 gateway_has_accounts guard）
// ============================================================================

TEST(TdAccountRegistryTest, GatewayHasAccountsMatchesOnlyThatGateway) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    registry.reconcile_gateway("dztd_ctp_2", accounts({{"acc_x", true}}));

    EXPECT_TRUE(registry.gateway_has_accounts("dztd_ctp"));
    EXPECT_TRUE(registry.gateway_has_accounts("dztd_ctp_2"));
    EXPECT_FALSE(registry.gateway_has_accounts("dztd_xtp"))
        << "gateway without accounts must not block removal (契约 rest §2.10 预检 5)";
}

TEST(TdAccountRegistryTest, GatewayHasAccountsCountsDisabledAccounts) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", false}}));

    // 已禁用账户同样是「有账户」：配置仍持久化在接口文件里，带账户移除会复活
    // （ADR 0053 决策 3），guard 不得区分 enabled。
    EXPECT_TRUE(registry.gateway_has_accounts("dztd_ctp"));
}

TEST(TdAccountRegistryTest, GatewayHasAccountsEmptyAfterWholeGroupRemoval) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}, {"acc_b", true}}));
    registry.reconcile_gateway("dztd_ctp", {});

    EXPECT_FALSE(registry.gateway_has_accounts("dztd_ctp"));
}

TEST(TdAccountRegistryTest, GatewayHasAccountsEmptyAfterPrune) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    registry.prune_gateways_not_in({});

    EXPECT_FALSE(registry.gateway_has_accounts("dztd_ctp"));
}

TEST(TdAccountRegistryTest, GatewayHasAccountsEmptyGatewayIsFalse) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));

    EXPECT_FALSE(registry.gateway_has_accounts(""));
}

// ============================================================================
// 可用性判定（契约 rest §2.9.9 的 available:false / no_gateway_snapshot）
// ============================================================================

TEST(TdAccountRegistryTest, UnavailableUntilFirstFrameOrLoadedRows) {
    TdAccountRegistry registry;
    EXPECT_FALSE(registry.available());
    EXPECT_FALSE(registry.has_snapshot());

    registry.note_config_frame_received();
    EXPECT_TRUE(registry.available())
        << "a frame with zero accounts is still a trustworthy snapshot";
    EXPECT_TRUE(registry.has_snapshot());
    EXPECT_TRUE(registry.entries().empty());
}

TEST(TdAccountRegistryTest, LoadedRowsMakeRegistryAvailable) {
    auto store = std::make_unique<FakeTdAccountRegistryStore>();
    store->stored_rows = {{.account_id = "acc_a", .gateway = "dztd_ctp", .account_name = ""}};
    TdAccountRegistry registry(std::move(store));

    EXPECT_EQ(registry.load(), 1U);
    EXPECT_TRUE(registry.available()) << "restored rows make 'no accounts' distinguishable";
    ASSERT_TRUE(registry.find("acc_a").has_value());
    // 契约 rest §2.9.9 已知降级：持久层没有 enabled，恢复的条目回 true。
    EXPECT_TRUE(registry.find("acc_a")->enabled);
}

TEST(TdAccountRegistryTest, LoadFailureStartsEmptyAndLogsWithoutThrowing) {
    auto store = std::make_unique<FakeTdAccountRegistryStore>();
    store->fail_load = true;
    TdAccountRegistry registry(std::move(store));

    EXPECT_EQ(registry.load(), 0U) << "load failure must not throw (契约 td-config §5)";
    EXPECT_TRUE(registry.entries().empty());
    EXPECT_FALSE(registry.available());
}

// ============================================================================
// 持久化（契约 td-config §5：尽力而为）
// ============================================================================

TEST(TdAccountRegistryTest, ReconcilePersistsWholeGroupPerGateway) {
    auto store = std::make_unique<FakeTdAccountRegistryStore>();
    auto* raw = store.get();
    TdAccountRegistry registry(std::move(store));

    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}, {"acc_b", true}}));

    EXPECT_EQ(raw->persisted("dztd_ctp"), (std::vector<std::string>{"acc_a", "acc_b"}))
        << "whole-group replace, not append";
}

TEST(TdAccountRegistryTest, PersistFailureDoesNotBlockInMemoryUpdate) {
    auto store = std::make_unique<FakeTdAccountRegistryStore>();
    store->fail_replace = true;
    TdAccountRegistry registry(std::move(store));

    // 契约 td-config §5：DB 写失败只记 ERROR，内存优先、不阻塞账户操作。
    EXPECT_TRUE(registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}})));
    EXPECT_TRUE(registry.contains("acc_a")) << "in-memory state must win over a failed write";
}

TEST(TdAccountRegistryTest, PersistThrowingDoesNotPropagate) {
    auto store = std::make_unique<FakeTdAccountRegistryStore>();
    auto* raw = store.get();
    store->fail_replace = true;
    TdAccountRegistry registry(std::move(store));

    EXPECT_NO_THROW(registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}})));
    // 不只在"没抛"上断言：失败路径必须**确实被走到**，且内存状态照常生效。
    // （否则 reconcile 根本没执行、内存没更新时本用例也会恒过。）
    EXPECT_TRUE(raw->replaced_called("dztd_ctp"))
        << "reconcile must attempt the write even though it is going to fail";
    EXPECT_TRUE(registry.contains("acc_a"))
        << "in-memory state must survive a failed (throwing/false) persist";
}

// ============================================================================
// 网关移除清理（契约 td-config §5）
// ============================================================================

TEST(TdAccountRegistryTest, PruneRemovesEntriesOfDepartedGateway) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    registry.reconcile_gateway("dztd_ctp_2", accounts({{"acc_x", true}}));

    const bool changed = registry.prune_gateways_not_in({"dztd_ctp_2"});

    EXPECT_TRUE(changed);
    EXPECT_FALSE(registry.contains("acc_a"));
    EXPECT_TRUE(registry.contains("acc_x"));
}

TEST(TdAccountRegistryTest, PruneClearsPersistentRowsToo) {
    auto store = std::make_unique<FakeTdAccountRegistryStore>();
    auto* raw = store.get();
    TdAccountRegistry registry(std::move(store));
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    ASSERT_EQ(raw->persisted("dztd_ctp"), (std::vector<std::string>{"acc_a"}));

    registry.prune_gateways_not_in({});

    // 持久层不清 → 重启后又被加载回来（契约 td-config §5 的"整体清理"含持久层）。
    EXPECT_TRUE(raw->persisted("dztd_ctp").empty());
}

TEST(TdAccountRegistryTest, PruneDeleteFailureDoesNotThrow) {
    auto store = std::make_unique<FakeTdAccountRegistryStore>();
    store->fail_delete = true;
    TdAccountRegistry registry(std::move(store));
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));

    EXPECT_NO_THROW(registry.prune_gateways_not_in({}));
    EXPECT_FALSE(registry.contains("acc_a")) << "in-memory prune must still happen";
}

TEST(TdAccountRegistryTest, PruneReportsNoChangeWhenNothingDeparted) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    EXPECT_FALSE(registry.prune_gateways_not_in({"dztd_ctp"}));
}

// ============================================================================
// 真 platform.db 持久层（真 schema、真 SQLite 临时库）
// ============================================================================

class TdAccountRegistryDbTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::unique_ptr<Repository> repo_;
    std::unique_ptr<TdAccountRegistry> registry_;

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_webui_td_account_registry_test");
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        registry_.reset();
        repo_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// prepare 一个真 platform.db（Repository ctor 会跑 run_schema）。
    void open_db(const std::string& name = "platform.db") {
        repo_ = std::make_unique<Repository>(dir_ / name);
        registry_ = std::make_unique<TdAccountRegistry>(
            std::make_unique<RepositoryTdAccountRegistryStore>(*repo_));
    }

    /// 关掉库再开同名库（模拟 dzweb 重启：内存丢失，只剩 platform.db）。
    void reopen_db() {
        registry_.reset();
        repo_.reset();
        open_db();
    }
};

TEST_F(TdAccountRegistryDbTest, RestartRestoresAccountsFromPlatformDb) {
    open_db();
    registry_->reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}, {"acc_b", false}}));

    reopen_db();
    EXPECT_EQ(registry_->load(), 2U);
    ASSERT_TRUE(registry_->find("acc_a").has_value());
    EXPECT_EQ(registry_->find("acc_a")->gateway, "dztd_ctp");
    // 已知降级（契约 rest §2.9.9）：enabled 未持久化 → 重启后回 true，不误标为禁用。
    EXPECT_TRUE(registry_->find("acc_b")->enabled);
}

TEST_F(TdAccountRegistryDbTest, ReconcileReplacesPersistedGroupAcrossRestart) {
    open_db();
    registry_->reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}, {"acc_b", true}}));
    registry_->reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));

    reopen_db();
    EXPECT_EQ(registry_->load(), 1U) << "removed account must not come back after restart";
    EXPECT_TRUE(registry_->contains("acc_a"));
    EXPECT_FALSE(registry_->contains("acc_b"));
}

TEST_F(TdAccountRegistryDbTest, PruneClearsPersistedRowsAcrossRestart) {
    open_db();
    registry_->reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    registry_->prune_gateways_not_in({});

    reopen_db();
    EXPECT_EQ(registry_->load(), 0U);
}

TEST_F(TdAccountRegistryDbTest, RestartRestoresAccountNamesFromPlatformDb) {
    // 契约 td-config §5 / rest §2.9.9：account_name 持久化，重启窗口内仍可展示（ADR 0056）。
    open_db();
    registry_->reconcile_gateway("dztd_ctp", named_accounts({{"acc_a", "我的主账户", true}}));

    reopen_db();
    EXPECT_EQ(registry_->load(), 1U);
    ASSERT_TRUE(registry_->find("acc_a").has_value());
    EXPECT_EQ(registry_->find("acc_a")->account_name, "我的主账户");
}

TEST_F(TdAccountRegistryDbTest, PersistedSchemaHasThreeColumns) {
    // 契约 rest §2.9.9：platform.db 存 account_id / gateway / account_name 三列，
    // enabled 不落库。用真 schema 断言列集合——列变更会让这条变红，提醒同步契约与迁移。
    open_db();
    registry_->reconcile_gateway("dztd_ctp", named_accounts({{"acc_a", "主账户", false}}));

    const auto rows = repo_->list_td_account_registry();
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].account_id, "acc_a");
    EXPECT_EQ(rows[0].gateway, "dztd_ctp");
    EXPECT_EQ(rows[0].account_name, "主账户");
}

TEST_F(TdAccountRegistryDbTest, LegacyDatabaseGetsAccountNameColumnAndEmptyFallback) {
    // AC1：旧库（两列）启动自动补 account_name 列，旧行读为空串；不要求用户重建库。
    const auto path = dir_ / "legacy.db";
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
        char* err = nullptr;
        ASSERT_EQ(sqlite3_exec(raw,
                               "CREATE TABLE td_account_registry ("
                               "account_id TEXT PRIMARY KEY, gateway TEXT NOT NULL);"
                               "INSERT INTO td_account_registry (account_id, gateway) "
                               "VALUES ('acc_old', 'dztd_ctp');",
                               nullptr, nullptr, &err),
                  SQLITE_OK)
            << (err ? err : "");
        sqlite3_free(err);
        sqlite3_close(raw);
    }

    // Repository ctor → run_schema：CREATE TABLE IF NOT EXISTS 不会补列，必须靠 ALTER 迁移。
    repo_ = std::make_unique<Repository>(path);
    registry_ = std::make_unique<TdAccountRegistry>(
        std::make_unique<RepositoryTdAccountRegistryStore>(*repo_));
    EXPECT_EQ(registry_->load(), 1U) << "legacy row must survive the migration";
    ASSERT_TRUE(registry_->find("acc_old").has_value());
    EXPECT_EQ(registry_->find("acc_old")->account_name, "")
        << "rest §2.9.9: legacy rows restore with empty name (frontend falls back to id)";

    const auto rows = repo_->list_td_account_registry();
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].account_name, "");
}

TEST_F(TdAccountRegistryDbTest, TwoGatewaysCoexistInDb) {
    open_db();
    registry_->reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    registry_->reconcile_gateway("dztd_ctp_2", accounts({{"acc_b", true}}));

    reopen_db();
    EXPECT_EQ(registry_->load(), 2U);
    EXPECT_EQ(registry_->find("acc_a")->gateway, "dztd_ctp");
    EXPECT_EQ(registry_->find("acc_b")->gateway, "dztd_ctp_2");
}

}  // namespace
}  // namespace dztrader::webui
