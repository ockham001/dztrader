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

AccountConfig make_account(const std::string& id,
                           const std::string& broker_id = "",
                           const std::string& password = "") {
    AccountConfig a;
    a.account_id = id;
    a.ext.broker_id = broker_id;
    a.ext.password = password;
    return a;
}

TdConfigOpReq make_req(TdConfigOp op, nlohmann::json params) {
    return TdConfigOpReq{.op = op, .params = std::move(params)};
}

// ============================================================================
// apply_config_op: 全部 op case（契约 td-config §2.1）
// ============================================================================

TEST(ApplyConfigOpTest, AddAccount) {
    TdConfig cfg;
    auto req = make_req(TdConfigOp::AddAccount, {{"account_id", "a1"},
                                                 {"enabled", true},
                                                 {"risk_control_enabled", false},
                                                 {"currency_id", "CNY"},
                                                 {"ext",
                                                  {{"broker_id", "9999"},
                                                   {"user_id", "00001"},
                                                   {"password", "123456"},
                                                   {"auth_code", "auth"},
                                                   {"app_id", "app"}}}});
    apply_config_op(cfg, req);
    ASSERT_EQ(cfg.accounts.size(), 1u);
    EXPECT_EQ(cfg.accounts[0].account_id, "a1");
    EXPECT_EQ(cfg.accounts[0].ext.broker_id, "9999");
    EXPECT_EQ(cfg.accounts[0].ext.user_id, "00001");
    EXPECT_EQ(cfg.accounts[0].ext.password, "123456");
    EXPECT_EQ(cfg.accounts[0].ext.auth_code, "auth");
    EXPECT_EQ(cfg.accounts[0].ext.app_id, "app");
    EXPECT_EQ(cfg.accounts[0].currency_id, "CNY");
}

TEST(ApplyConfigOpTest, AddAccountWithMissingFields) {
    TdConfig cfg;
    auto req = make_req(TdConfigOp::AddAccount, nlohmann::json::object());
    apply_config_op(cfg, req);
    ASSERT_EQ(cfg.accounts.size(), 1u);
    EXPECT_TRUE(cfg.accounts[0].account_id.empty());
    // 通用层默认值按契约 td-config §2.2：currency_id 默认 CNY、enabled 默认 true
    EXPECT_EQ(cfg.accounts[0].currency_id, "CNY");
    EXPECT_TRUE(cfg.accounts[0].enabled);
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
    cfg.accounts.push_back(make_account("a1", "old_broker", "old_pass"));
    auto req =
        make_req(TdConfigOp::UpdateAccount,
                 {{"account_id", "a1"},
                  {"ext", {{"broker_id", "9999"}, {"user_id", "u1"}, {"password", "new_pass"}}}});
    apply_config_op(cfg, req);
    EXPECT_EQ(cfg.accounts[0].ext.password, "new_pass");
    EXPECT_EQ(cfg.accounts[0].ext.broker_id, "9999");
    EXPECT_EQ(cfg.accounts[0].ext.user_id, "u1");
}

TEST(ApplyConfigOpTest, UpdateAccountPasswordMaskKeepsOld) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1", "broker1", "old_pass"));
    // password="****" 表示保留旧值（契约 td-config §2.1：脱敏值不得覆盖真实密码）
    auto req = make_req(TdConfigOp::UpdateAccount,
                        {{"account_id", "a1"}, {"ext", {{"password", "****"}}}});
    apply_config_op(cfg, req);
    EXPECT_EQ(cfg.accounts[0].ext.password, "old_pass");  // 保留旧值
}

TEST(ApplyConfigOpTest, UpdateAccountPasswordOnlyKeepsOtherExtFields) {
    TdConfig cfg;
    AccountConfig acct;
    acct.account_id = "acc1";
    acct.ext.broker_id = "broker1";
    acct.ext.user_id = "user1";
    acct.ext.product_info = "prod1";
    acct.ext.auth_code = "auth1";
    acct.ext.app_id = "app1";
    acct.ext.flow_dir = "flow1";
    acct.ext.password = "old_pass";
    BrokerFrontend fe;
    fe.address = "addr1";
    fe.label = "lbl1";
    fe.enabled = true;
    acct.ext.frontends.push_back(fe);
    cfg.accounts.push_back(acct);

    // 仅提供 password, 其他 ext 字段未提供 (应保留旧值, 不被清空)
    auto req = make_req(TdConfigOp::UpdateAccount,
                        {{"account_id", "acc1"}, {"ext", {{"password", "new_pass"}}}});
    apply_config_op(cfg, req);

    EXPECT_EQ(cfg.accounts[0].ext.password, "new_pass");
    // 其他字段应保留旧值 (字段级 merge, 不被清空为默认值)
    EXPECT_EQ(cfg.accounts[0].ext.broker_id, "broker1");
    EXPECT_EQ(cfg.accounts[0].ext.user_id, "user1");
    EXPECT_EQ(cfg.accounts[0].ext.product_info, "prod1");
    EXPECT_EQ(cfg.accounts[0].ext.auth_code, "auth1");
    EXPECT_EQ(cfg.accounts[0].ext.app_id, "app1");
    EXPECT_EQ(cfg.accounts[0].ext.flow_dir, "flow1");
    ASSERT_EQ(cfg.accounts[0].ext.frontends.size(), 1u);
    EXPECT_EQ(cfg.accounts[0].ext.frontends[0].address, "addr1");
    EXPECT_EQ(cfg.accounts[0].ext.frontends[0].label, "lbl1");
    EXPECT_TRUE(cfg.accounts[0].ext.frontends[0].enabled);
}

TEST(ApplyConfigOpTest, UpdateAccountPasswordEmptyKeepsOld) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1", "broker1", "old_pass"));
    // password="" (空串) 表示不改
    auto req =
        make_req(TdConfigOp::UpdateAccount, {{"account_id", "a1"}, {"ext", {{"password", ""}}}});
    apply_config_op(cfg, req);
    EXPECT_EQ(cfg.accounts[0].ext.password, "old_pass");
}

TEST(ApplyConfigOpTest, UpdateAccountUpdatesExtAndCommonFields) {
    TdConfig cfg;
    AccountConfig acct = make_account("a1", "old_broker", "old_pass");
    acct.ext.user_id = "old_user";
    acct.ext.product_info = "old_product";
    acct.ext.auth_code = "old_auth";
    acct.ext.app_id = "old_app";
    acct.ext.flow_dir = "old_flow";
    acct.currency_id = "CNY";
    acct.enabled = true;
    acct.risk_control_enabled = false;
    BrokerFrontend old_fe;
    old_fe.address = "old_addr";
    old_fe.label = "old_label";
    old_fe.enabled = true;
    acct.ext.frontends.push_back(old_fe);
    cfg.accounts.push_back(acct);

    nlohmann::json frontends = nlohmann::json::array();
    frontends.push_back({{"address", "new_addr"}, {"label", "new_label"}, {"enabled", false}});
    auto req = make_req(TdConfigOp::UpdateAccount, {{"account_id", "a1"},
                                                    {"enabled", false},
                                                    {"risk_control_enabled", true},
                                                    {"currency_id", "USD"},
                                                    {"ext",
                                                     {{"broker_id", "new_broker"},
                                                      {"user_id", "new_user"},
                                                      {"product_info", "new_product"},
                                                      {"password", "new_pass"},
                                                      {"auth_code", "new_auth"},
                                                      {"app_id", "new_app"},
                                                      {"flow_dir", "new_flow"},
                                                      {"frontends", frontends}}}});
    apply_config_op(cfg, req);

    EXPECT_EQ(cfg.accounts[0].ext.broker_id, "new_broker");
    EXPECT_EQ(cfg.accounts[0].ext.user_id, "new_user");
    EXPECT_EQ(cfg.accounts[0].ext.product_info, "new_product");
    EXPECT_EQ(cfg.accounts[0].ext.password, "new_pass");
    EXPECT_EQ(cfg.accounts[0].ext.auth_code, "new_auth");
    EXPECT_EQ(cfg.accounts[0].ext.app_id, "new_app");
    EXPECT_EQ(cfg.accounts[0].ext.flow_dir, "new_flow");
    EXPECT_FALSE(cfg.accounts[0].enabled);
    EXPECT_TRUE(cfg.accounts[0].risk_control_enabled);
    EXPECT_EQ(cfg.accounts[0].currency_id, "USD");
    // frontends 提供时整体替换 (旧条目被丢弃)
    ASSERT_EQ(cfg.accounts[0].ext.frontends.size(), 1u);
    EXPECT_EQ(cfg.accounts[0].ext.frontends[0].address, "new_addr");
    EXPECT_EQ(cfg.accounts[0].ext.frontends[0].label, "new_label");
    EXPECT_FALSE(cfg.accounts[0].ext.frontends[0].enabled);
}

TEST(ApplyConfigOpTest, UpdateAccountOmittedFieldsPreserved) {
    TdConfig cfg;
    AccountConfig acct = make_account("a1", "broker1", "old_pass");
    acct.ext.user_id = "user1";
    acct.ext.product_info = "prod1";
    acct.ext.auth_code = "auth";
    acct.ext.app_id = "app";
    acct.ext.flow_dir = "flow";
    acct.currency_id = "USD";
    acct.enabled = false;
    acct.risk_control_enabled = true;
    BrokerFrontend fe;
    fe.address = "addr1";
    fe.label = "lbl1";
    fe.enabled = true;
    acct.ext.frontends.push_back(fe);
    cfg.accounts.push_back(acct);

    // 仅更新 ext.broker_id, 其余字段 (含 frontends 与通用层) 全部缺省
    auto req =
        make_req(TdConfigOp::UpdateAccount, {{"account_id", "a1"}, {"ext", {{"broker_id", "b2"}}}});
    apply_config_op(cfg, req);

    EXPECT_EQ(cfg.accounts[0].ext.broker_id, "b2");
    EXPECT_EQ(cfg.accounts[0].ext.user_id, "user1");
    EXPECT_EQ(cfg.accounts[0].ext.product_info, "prod1");
    EXPECT_EQ(cfg.accounts[0].ext.password, "old_pass");
    EXPECT_EQ(cfg.accounts[0].ext.auth_code, "auth");
    EXPECT_EQ(cfg.accounts[0].ext.app_id, "app");
    EXPECT_EQ(cfg.accounts[0].ext.flow_dir, "flow");
    EXPECT_EQ(cfg.accounts[0].currency_id, "USD");
    EXPECT_FALSE(cfg.accounts[0].enabled);
    EXPECT_TRUE(cfg.accounts[0].risk_control_enabled);
    ASSERT_EQ(cfg.accounts[0].ext.frontends.size(), 1u);
    EXPECT_EQ(cfg.accounts[0].ext.frontends[0].address, "addr1");
}

TEST(ApplyConfigOpTest, UpdateAccountEmptyParamsIsNoOp) {
    // 契约 td-config §2.1 UpdateAccount：允许任意子集；PUT 允许空对象（总则 §8 空 patch = 无操作）
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1", "broker1", "pass1"));
    const auto before = cfg.accounts[0].ext.password;
    auto req = make_req(TdConfigOp::UpdateAccount, {{"account_id", "a1"}});
    EXPECT_NO_THROW(apply_config_op(cfg, req));
    EXPECT_EQ(cfg.accounts[0].ext.password, before);
    EXPECT_EQ(cfg.accounts[0].ext.broker_id, "broker1");
}

TEST(ApplyConfigOpTest, UpdateAccountNotFoundThrows) {
    // 关键决策: UpdateAccount 不存在抛 std::invalid_argument
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1", "broker1"));
    auto req = make_req(TdConfigOp::UpdateAccount,
                        {{"account_id", "nonexistent"}, {"ext", {{"user_id", "u1"}}}});
    EXPECT_THROW(apply_config_op(cfg, req), std::invalid_argument);
    EXPECT_EQ(cfg.accounts[0].ext.user_id, "");  // cfg 未被修改
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
    auto req =
        make_req(TdConfigOp::SetAccountRiskControl, {{"account_id", "a1"}, {"enabled", true}});
    apply_config_op(cfg, req);
    EXPECT_TRUE(cfg.accounts[0].risk_control_enabled);
}

TEST(ApplyConfigOpTest, SetAccountCurrency) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    auto req =
        make_req(TdConfigOp::SetAccountCurrency, {{"account_id", "a1"}, {"currency_id", "USD"}});
    apply_config_op(cfg, req);
    EXPECT_EQ(cfg.accounts[0].currency_id, "USD");
}

TEST(ApplyConfigOpTest, SetAccountEnabledUnknownIdLeavesAllAccountsUnchanged) {
    // 目标不存在时静默 no-op: 不得误改首个/任一账户
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    cfg.accounts.push_back(make_account("a2"));
    auto req =
        make_req(TdConfigOp::SetAccountEnabled, {{"account_id", "missing"}, {"enabled", false}});
    EXPECT_NO_THROW(apply_config_op(cfg, req));
    EXPECT_EQ(cfg.accounts.size(), 2u);
    EXPECT_TRUE(cfg.accounts[0].enabled);
    EXPECT_TRUE(cfg.accounts[1].enabled);
}

TEST(ApplyConfigOpTest, SetAccountRiskControlUnknownIdLeavesAllAccountsUnchanged) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    cfg.accounts.push_back(make_account("a2"));
    auto req =
        make_req(TdConfigOp::SetAccountRiskControl, {{"account_id", "missing"}, {"enabled", true}});
    EXPECT_NO_THROW(apply_config_op(cfg, req));
    EXPECT_EQ(cfg.accounts.size(), 2u);
    EXPECT_FALSE(cfg.accounts[0].risk_control_enabled);
    EXPECT_FALSE(cfg.accounts[1].risk_control_enabled);
}

TEST(ApplyConfigOpTest, SetAccountCurrencyUnknownIdLeavesAllAccountsUnchanged) {
    TdConfig cfg;
    cfg.accounts.push_back(make_account("a1"));
    cfg.accounts.push_back(make_account("a2"));
    auto req = make_req(TdConfigOp::SetAccountCurrency,
                        {{"account_id", "missing"}, {"currency_id", "USD"}});
    EXPECT_NO_THROW(apply_config_op(cfg, req));
    EXPECT_EQ(cfg.accounts.size(), 2u);
    EXPECT_EQ(cfg.accounts[0].currency_id, "CNY");
    EXPECT_EQ(cfg.accounts[1].currency_id, "CNY");
}

// 契约 auto-login 迁移完成: AddSchedule/RemoveSchedule/SetAutoLogin op 已移除,
// 排程单一真相源为 SET/RTN_AUTO_LOGIN 帧 (见 td_api_scheduled.cpp / auto_login_config_)

// 契约 td-config §2.1：进程级柜台特有配置改用 SetExt（旧 SetLockMode 已删除，不留双实现）
TEST(ApplyConfigOpTest, SetExtLockMode) {
    TdConfig cfg;
    EXPECT_TRUE(cfg.ext.enable_lock_mode);  // 默认 true
    auto req = make_req(TdConfigOp::SetExt, {{"ext", {{"enable_lock_mode", false}}}});
    apply_config_op(cfg, req);
    EXPECT_FALSE(cfg.ext.enable_lock_mode);
}

TEST(ApplyConfigOpTest, SetExtMissingFieldKeepsOld) {
    // 契约 §2.1 SetExt：缺失字段保留旧值（字段级补丁）
    TdConfig cfg;
    cfg.ext.enable_lock_mode = false;
    auto req = make_req(TdConfigOp::SetExt, {{"ext", nlohmann::json::object()}});
    EXPECT_NO_THROW(apply_config_op(cfg, req));
    EXPECT_FALSE(cfg.ext.enable_lock_mode);
}

TEST(ApplyConfigOpTest, SetExtNoExtParamIsNoOp) {
    // 空 patch（总则 §8）：无操作、不抛
    TdConfig cfg;
    cfg.ext.enable_lock_mode = true;
    auto req = make_req(TdConfigOp::SetExt, nlohmann::json::object());
    EXPECT_NO_THROW(apply_config_op(cfg, req));
    EXPECT_TRUE(cfg.ext.enable_lock_mode);
}

TEST(ApplyConfigOpTest, SetQryIntervals) {
    TdConfig cfg;
    auto req = make_req(TdConfigOp::SetQryIntervals, {{"qry_account_interval_s", 10},
                                                      {"qry_position_interval_s", 15},
                                                      {"qry_flush_interval_ms", 2000}});
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
    EXPECT_EQ(cfg.qry_position_interval_s, 5);   // 保留旧值
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
    EXPECT_EQ(cfg.qry_position_interval_s, 5);   // 保留旧值
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
    // 契约 auto-login 迁移完成: AddSchedule/RemoveSchedule/SetAutoLogin 已从枚举移除
    EXPECT_FALSE(is_connection_op(TdConfigOp::SetExt));
    EXPECT_FALSE(is_connection_op(TdConfigOp::SetQryIntervals));
    EXPECT_FALSE(is_connection_op(TdConfigOp::SetAccountEnabled));
    EXPECT_FALSE(is_connection_op(TdConfigOp::SetAccountRiskControl));
    EXPECT_FALSE(is_connection_op(TdConfigOp::SetAccountCurrency));
}

}  // namespace
}  // namespace dztrader::ctp
