// td_api_pure_test: 单元测试 apply_config_op / find_current_account_in
// 这 2 个自由函数实现在 td_api_pure.cpp, 不依赖 TdApi 类和 CTP 头文件,
// 因此测试目标无需链接 dztd_ctp_api。

#include "td/td_config.h"  // TdConfig, TdConfigOpReq, AccountConfig, TdConfigOp

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace dztrader::ctp {

// === 自由函数声明 (与 td_api.h 中声明一致, 仅用于测试编译) ===

/// 纯粹的 op 应用: switch(req.op) 修改 cfg, 无副作用无 I/O。
void apply_config_op(TdConfig& cfg, const TdConfigOpReq& req);

/// 在 accounts 中查找 account_id 对应的账户 (纯函数)。
/// account_id 为空或未找到时返回 nullptr。
const AccountConfig* find_current_account_in(const std::vector<AccountConfig>& accounts,
                                              const std::string& account_id);

/// 在 cfg.accounts 中查找 account_id 对应的账户 (纯函数)。
/// account_id 为空或未找到时返回 nullptr。
const AccountConfig* find_account_in(const TdConfig& cfg, const std::string& account_id);

namespace {

AccountConfig make_account(const std::string& id, const std::string& broker_name = "",
                           const std::string& password = "") {
    AccountConfig a;
    a.account_id = id;
    a.broker.name = broker_name;
    a.broker.password = password;
    return a;
}

TdConfigOpReq make_req(TdConfigOp op, nlohmann::json params) {
    return TdConfigOpReq{.op = op, .params = std::move(params)};
}

// ============================================================================
// apply_config_op: 11 个 op case
// ============================================================================

TEST(ApplyConfigOpTest, AddAccount) {
    TdConfig cfg;
    auto req = make_req(TdConfigOp::AddAccount, {{"account_id", "a1"},
                                                  {"broker", {{"name", "simnow"},
                                                              {"broker_id", "9999"},
                                                              {"user_id", "00001"},
                                                              {"password", "123456"}}},
                                                  {"auth_code", "auth"},
                                                  {"app_id", "app"},
                                                  {"currency_id", "CNY"}});
    apply_config_op(cfg, req);
    ASSERT_EQ(cfg.accounts.size(), 1u);
    EXPECT_EQ(cfg.accounts[0].account_id, "a1");
    EXPECT_EQ(cfg.accounts[0].broker.name, "simnow");
    EXPECT_EQ(cfg.accounts[0].broker.broker_id, "9999");
    EXPECT_EQ(cfg.accounts[0].broker.password, "123456");
    EXPECT_EQ(cfg.accounts[0].auth_code, "auth");
    EXPECT_EQ(cfg.accounts[0].app_id, "app");
    EXPECT_EQ(cfg.accounts[0].currency_id, "CNY");
}

TEST(ApplyConfigOpTest, AddAccountWithMissingFields) {
    TdConfig cfg;
    auto req = make_req(TdConfigOp::AddAccount, nlohmann::json::object());
    apply_config_op(cfg, req);
    ASSERT_EQ(cfg.accounts.size(), 1u);
    EXPECT_TRUE(cfg.accounts[0].account_id.empty());
    EXPECT_TRUE(cfg.accounts[0].currency_id.empty());  // 无默认值 (AddAccount 不填默认)
}

TEST(ApplyConfigOpTest, AddAccountDuplicateThrows) {
    // 关键决策: AddAccount 检查 account_id 不重复, 重复抛 std::invalid_argument
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    auto req = make_req(TdConfigOp::AddAccount, {{"account_id", "a1"}});
    EXPECT_THROW(apply_config_op(cfg, req), std::invalid_argument);
    EXPECT_EQ(cfg.accounts.size(), 1u);  // 未新增
}

TEST(ApplyConfigOpTest, AddAccountEmptyIdSkipsDuplicateCheck) {
    // account_id 为空时跳过查重 (由 validate 在持久化前拦截空 id)
    TdConfig cfg;
    cfg.accounts.push_back(make_account(""));
    auto req = make_req(TdConfigOp::AddAccount, nlohmann::json::object());
    EXPECT_NO_THROW(apply_config_op(cfg, req));
    EXPECT_EQ(cfg.accounts.size(), 2u);
}

TEST(ApplyConfigOpTest, RemoveAccount) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    cfg.accounts.push_back(make_account("a2"));
    cfg.accounts.push_back(make_account("a3"));

    auto req = make_req(TdConfigOp::RemoveAccount, {{"account_id", "a2"}});
    apply_config_op(cfg, req);
    ASSERT_EQ(cfg.accounts.size(), 2u);
    EXPECT_EQ(cfg.accounts[0].account_id, "a1");
    EXPECT_EQ(cfg.accounts[1].account_id, "a3");
}

TEST(ApplyConfigOpTest, RemoveAccountNotFound) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    auto req = make_req(TdConfigOp::RemoveAccount, {{"account_id", "nonexistent"}});
    apply_config_op(cfg, req);
    EXPECT_EQ(cfg.accounts.size(), 1u);  // 不变
}

TEST(ApplyConfigOpTest, UpdateAccount) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1", "old_name", "old_pass"));
    auto req = make_req(TdConfigOp::UpdateAccount, {{"account_id", "a1"},
                                                     {"broker", {{"name", "new_name"},
                                                                 {"broker_id", "9999"},
                                                                 {"user_id", "u1"},
                                                                 {"password", "new_pass"}}}});
    apply_config_op(cfg, req);
    EXPECT_EQ(cfg.accounts[0].broker.name, "new_name");
    EXPECT_EQ(cfg.accounts[0].broker.password, "new_pass");
    EXPECT_EQ(cfg.accounts[0].broker.broker_id, "9999");
}

TEST(ApplyConfigOpTest, UpdateAccountPasswordMaskKeepsOld) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1", "name", "old_pass"));
    // password="****" 表示保留旧值
    auto req = make_req(TdConfigOp::UpdateAccount, {{"account_id", "a1"},
                                                     {"broker", {{"name", "name"},
                                                                 {"password", "****"}}}});
    apply_config_op(cfg, req);
    EXPECT_EQ(cfg.accounts[0].broker.password, "old_pass");  // 保留旧值
}

TEST(ApplyConfigOpTest, UpdateAccountPasswordOnlyKeepsOtherBrokerFields) {
    TdConfig cfg;
    AccountConfig acct;
    acct.account_id = "acc1";
    acct.broker.name = "broker_name";
    acct.broker.broker_id = "broker1";
    acct.broker.user_id = "user1";
    acct.broker.product_info = "prod1";
    acct.broker.password = "old_pass";
    BrokerFrontend fe;
    fe.address = "addr1";
    fe.label = "lbl1";
    fe.enabled = true;
    acct.broker.frontends.push_back(fe);
    cfg.accounts.push_back(acct);

    // 仅提供 password, 其他 broker 字段未提供 (应保留旧值, 不被清空)
    nlohmann::json params;
    params["account_id"] = "acc1";
    nlohmann::json broker;
    broker["password"] = "new_pass";
    params["broker"] = broker;
    auto req = make_req(TdConfigOp::UpdateAccount, std::move(params));
    apply_config_op(cfg, req);

    // password 应更新
    EXPECT_EQ(cfg.accounts[0].broker.password, "new_pass");
    // 其他字段应保留旧值 (字段级 merge, 不被清空为默认值)
    EXPECT_EQ(cfg.accounts[0].broker.name, "broker_name");
    EXPECT_EQ(cfg.accounts[0].broker.broker_id, "broker1");
    EXPECT_EQ(cfg.accounts[0].broker.user_id, "user1");
    EXPECT_EQ(cfg.accounts[0].broker.product_info, "prod1");
    ASSERT_EQ(cfg.accounts[0].broker.frontends.size(), 1u);
    EXPECT_EQ(cfg.accounts[0].broker.frontends[0].address, "addr1");
    EXPECT_EQ(cfg.accounts[0].broker.frontends[0].label, "lbl1");
    EXPECT_EQ(cfg.accounts[0].broker.frontends[0].enabled, true);
}

TEST(ApplyConfigOpTest, UpdateAccountPasswordEmptyKeepsOld) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1", "name", "old_pass"));
    // password="" (空串) 表示不改
    auto req = make_req(TdConfigOp::UpdateAccount, {{"account_id", "a1"},
                                                     {"broker", {{"password", ""}}}});
    apply_config_op(cfg, req);
    EXPECT_EQ(cfg.accounts[0].broker.password, "old_pass");
}

TEST(ApplyConfigOpTest, UpdateAccountNotFoundThrows) {
    // 关键决策: UpdateAccount 不存在抛 std::invalid_argument
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    auto req = make_req(TdConfigOp::UpdateAccount, {{"account_id", "nonexistent"},
                                                     {"broker", {{"name", "x"}}}});
    EXPECT_THROW(apply_config_op(cfg, req), std::invalid_argument);
    EXPECT_EQ(cfg.accounts[0].broker.name, "");  // cfg 未被修改
}

TEST(ApplyConfigOpTest, SetAccountEnabled) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    EXPECT_TRUE(cfg.accounts[0].enabled);  // 默认 true
    auto req = make_req(TdConfigOp::SetAccountEnabled, {{"account_id", "a1"}, {"enabled", false}});
    apply_config_op(cfg, req);
    EXPECT_FALSE(cfg.accounts[0].enabled);
}

TEST(ApplyConfigOpTest, SetAccountRiskControl) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    EXPECT_FALSE(cfg.accounts[0].risk_control_enabled);  // 默认 false
    auto req = make_req(TdConfigOp::SetAccountRiskControl,
                        {{"account_id", "a1"}, {"enabled", true}});
    apply_config_op(cfg, req);
    EXPECT_TRUE(cfg.accounts[0].risk_control_enabled);
}

TEST(ApplyConfigOpTest, SetAccountCurrency) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    auto req = make_req(TdConfigOp::SetAccountCurrency,
                        {{"account_id", "a1"}, {"currency_id", "USD"}});
    apply_config_op(cfg, req);
    EXPECT_EQ(cfg.accounts[0].currency_id, "USD");
}

// 契约 04 迁移完成: AddSchedule/RemoveSchedule/SetAutoLogin op 已移除,
// 排程单一真相源为 SET/RTN_AUTO_LOGIN 帧 (见 td_api_scheduled.cpp / auto_login_config_)

TEST(ApplyConfigOpTest, SetLockMode) {
    TdConfig cfg;
    EXPECT_TRUE(cfg.enable_lock_mode);  // 默认 true
    auto req = make_req(TdConfigOp::SetLockMode, {{"enabled", false}});
    apply_config_op(cfg, req);
    EXPECT_FALSE(cfg.enable_lock_mode);
}

TEST(ApplyConfigOpTest, SetQryIntervals) {
    TdConfig cfg;
    auto req = make_req(TdConfigOp::SetQryIntervals, {
        {"qry_account_interval_s", 10},
        {"qry_position_interval_s", 15},
        {"qry_flush_interval_ms", 2000}
    });
    apply_config_op(cfg, req);
    EXPECT_EQ(cfg.qry_account_interval_s, 10);
    EXPECT_EQ(cfg.qry_position_interval_s, 15);
    EXPECT_EQ(cfg.qry_flush_interval_ms, 2000);
}

TEST(ApplyConfigOpTest, SetQryIntervalsPartialUpdate) {
    TdConfig cfg;
    cfg.qry_account_interval_s = 5;
    cfg.qry_position_interval_s = 5;
    cfg.qry_flush_interval_ms = 1500;
    // 仅更新一个字段, 其他保留
    auto req = make_req(TdConfigOp::SetQryIntervals, {{"qry_account_interval_s", 20}});
    apply_config_op(cfg, req);
    EXPECT_EQ(cfg.qry_account_interval_s, 20);
    EXPECT_EQ(cfg.qry_position_interval_s, 5);  // 保留旧值
    EXPECT_EQ(cfg.qry_flush_interval_ms, 1500);  // 保留旧值
}

TEST(ApplyConfigOpTest, SetQryIntervalsZeroAccountIntervalThrows) {
    // 关键决策: int_val 越界 (<=0) 抛 std::invalid_argument
    TdConfig cfg;
    auto req = make_req(TdConfigOp::SetQryIntervals, {{"qry_account_interval_s", 0}});
    EXPECT_THROW(apply_config_op(cfg, req), std::invalid_argument);
}

TEST(ApplyConfigOpTest, SetQryIntervalsNegativePositionIntervalThrows) {
    TdConfig cfg;
    auto req = make_req(TdConfigOp::SetQryIntervals, {{"qry_position_interval_s", -1}});
    EXPECT_THROW(apply_config_op(cfg, req), std::invalid_argument);
}

TEST(ApplyConfigOpTest, SetQryIntervalsZeroFlushIntervalThrows) {
    TdConfig cfg;
    auto req = make_req(TdConfigOp::SetQryIntervals, {{"qry_flush_interval_ms", 0}});
    EXPECT_THROW(apply_config_op(cfg, req), std::invalid_argument);
}

TEST(ApplyConfigOpTest, SetQryIntervalsPartialValidUpdatePreservesOthers) {
    // 部分更新: 只提供一个合法字段, 不影响其他字段, 不抛异常
    TdConfig cfg;
    cfg.qry_account_interval_s = 5;
    cfg.qry_position_interval_s = 5;
    cfg.qry_flush_interval_ms = 1500;
    auto req = make_req(TdConfigOp::SetQryIntervals, {{"qry_account_interval_s", 10}});
    EXPECT_NO_THROW(apply_config_op(cfg, req));
    EXPECT_EQ(cfg.qry_account_interval_s, 10);
    EXPECT_EQ(cfg.qry_position_interval_s, 5);  // 保留旧值
    EXPECT_EQ(cfg.qry_flush_interval_ms, 1500);  // 保留旧值
}

// ============================================================================
// find_current_account_in
// ============================================================================

TEST(FindCurrentAccountTest, FoundReturnsPointer) {
    std::vector<AccountConfig> accts = {make_account("a1"), make_account("a2"), make_account("a3")};
    auto* found = find_current_account_in(accts, "a2");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->account_id, "a2");
}

TEST(FindCurrentAccountTest, NotFoundReturnsNull) {
    std::vector<AccountConfig> accts = {make_account("a1")};
    EXPECT_EQ(find_current_account_in(accts, "nonexistent"), nullptr);
}

TEST(FindCurrentAccountTest, EmptyIdReturnsNull) {
    std::vector<AccountConfig> accts = {make_account("a1")};
    EXPECT_EQ(find_current_account_in(accts, ""), nullptr);
}

TEST(FindCurrentAccountTest, EmptyListReturnsNull) {
    std::vector<AccountConfig> accts;
    EXPECT_EQ(find_current_account_in(accts, "a1"), nullptr);
}

// ============================================================================
// find_account_in (TdConfig 接受版本, 委托给 find_current_account_in)
// ============================================================================

TEST(FindAccountInTest, FoundReturnsPointer) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    cfg.accounts.push_back(make_account("a2"));
    auto* found = find_account_in(cfg, "a2");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->account_id, "a2");
}

TEST(FindAccountInTest, NotFoundReturnsNull) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    EXPECT_EQ(find_account_in(cfg, "nonexistent"), nullptr);
}

TEST(FindAccountInTest, EmptyIdReturnsNull) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    EXPECT_EQ(find_account_in(cfg, ""), nullptr);
}

TEST(FindAccountInTest, EmptyAccountsReturnsNull) {
    TdConfig cfg;
    EXPECT_EQ(find_account_in(cfg, "a1"), nullptr);
}

// ============================================================================
// is_connection_op
// ============================================================================

TEST(IsConnectionOpTest, ConnectionOps) {
    EXPECT_TRUE(is_connection_op(TdConfigOp::RemoveAccount));
    EXPECT_TRUE(is_connection_op(TdConfigOp::UpdateAccount));
}

TEST(IsConnectionOpTest, NonConnectionOps) {
    EXPECT_FALSE(is_connection_op(TdConfigOp::AddAccount));
    // 契约 04 迁移完成: AddSchedule/RemoveSchedule/SetAutoLogin 已从枚举移除
    EXPECT_FALSE(is_connection_op(TdConfigOp::SetLockMode));
    EXPECT_FALSE(is_connection_op(TdConfigOp::SetQryIntervals));
    EXPECT_FALSE(is_connection_op(TdConfigOp::SetAccountEnabled));
    EXPECT_FALSE(is_connection_op(TdConfigOp::SetAccountRiskControl));
    EXPECT_FALSE(is_connection_op(TdConfigOp::SetAccountCurrency));
}

}  // namespace
}  // namespace dztrader::ctp
