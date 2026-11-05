/**
 * @file td_account_registry_test.cpp
 * @brief 交易账户注册表（契约 td-config §5 / rest §2.9.9）。
 *
 * 断言外部可观察行为：reconcile 整组替换语义、重启加载、写失败不阻塞、网关移除清理、
 * `no_gateway_snapshot` 降级判定。**用真 platform.db schema + 真 SQLite 临时库**覆盖
 * 持久层本身（fake store 只用于注入写失败，见 `fake_td_account_registry_store.h`）。
 */

#include "td_account_registry.h"
#include "fake_td_account_registry_store.h"
#include "test_temp_dir.h"

#include "db_init.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace dztrader::webui {
namespace {

std::vector<TdAccountRegistryEntry> accounts(std::vector<std::pair<std::string, bool>> pairs) {
    std::vector<TdAccountRegistryEntry> result;
    result.reserve(pairs.size());
    for (auto& [id, enabled] : pairs) {
        result.push_back(
            TdAccountRegistryEntry{.account_id = id, .gateway = "", .enabled = enabled});
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

TEST(TdAccountRegistryTest, ReconcileSelfHealsCrossGatewayDuplicate) {
    TdAccountRegistry registry;
    registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}}));
    // 同一 account_id 出现在另一网关的帧里：帧为权威 → 改判归属（契约 td-config §5 全局唯一）。
    registry.reconcile_gateway("dztd_ctp_2", accounts({{"acc_a", true}}));

    ASSERT_TRUE(registry.find("acc_a").has_value());
    EXPECT_EQ(registry.find("acc_a")->gateway, "dztd_ctp_2");
    EXPECT_EQ(registry.size(), 1U) << "account_id is a global key, not per-gateway";
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
    store->stored_rows = {{.account_id = "acc_a", .gateway = "dztd_ctp"}};
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
    // load 用 throw 模拟异常；replace 用 return false 模拟失败——两者都必须被吞。
    store->fail_replace = true;
    TdAccountRegistry registry(std::move(store));
    EXPECT_NO_THROW(registry.reconcile_gateway("dztd_ctp", accounts({{"acc_a", true}})));
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

TEST_F(TdAccountRegistryDbTest, PersistedSchemaHasOnlyAccountIdAndGateway) {
    // 契约 rest §2.9.9：platform.db 只存 account_id → gateway 两列，enabled 不落库。
    // 用真 schema 断言列集合——加列会让这条变红，提醒改契约与读端点的 enabled 语义。
    open_db();
    registry_->reconcile_gateway("dztd_ctp", accounts({{"acc_a", false}}));

    const auto rows = repo_->list_td_account_registry();
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].account_id, "acc_a");
    EXPECT_EQ(rows[0].gateway, "dztd_ctp");
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
