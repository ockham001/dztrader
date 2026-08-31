#include "td/td_config.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

namespace dztrader::ctp {

// ============================================================================
// td_api_pure.cpp: 纯函数实现, 不依赖 TdApi 类和 CTP 头文件
// 供单元测试直接编译 (无需链接 CTP 库)
// ============================================================================

// 纯粹的 op 应用: switch(req.op) 修改 cfg, 无副作用无 I/O。
void apply_config_op(TdConfig& cfg, const TdConfigOpReq& req) {
    switch (req.op) {
        case TdConfigOp::AddAccount: {
            AccountConfig acct;
            acct.account_id = req.params.value("account_id", "");
            // 关键决策: AddAccount 检查 account_id 不重复, 重复抛 std::invalid_argument。
            // account_id 为空时跳过查重 (由 validate 在持久化前拦截空 id)。
            if (!acct.account_id.empty()) {
                for (const auto& a : cfg.accounts) {
                    if (a.account_id == acct.account_id) {
                        throw std::invalid_argument(
                            std::format("addaccount duplicate | account_id=\"{}\"",
                                        acct.account_id));
                    }
                }
            }
            if (req.params.contains("broker")) {
                acct.broker = req.params["broker"].get<BrokerEntry>();
            }
            acct.auth_code = req.params.value("auth_code", "");
            acct.app_id = req.params.value("app_id", "");
            acct.flow_dir = req.params.value("flow_dir", "");
            acct.enabled = req.params.value("enabled", true);
            acct.risk_control_enabled = req.params.value("risk_control_enabled", false);
            acct.currency_id = req.params.value("currency_id", "");
            cfg.accounts.push_back(std::move(acct));
            break;
        }
        case TdConfigOp::RemoveAccount: {
            auto id = req.params.value("account_id", "");
            auto& accts = cfg.accounts;
            accts.erase(std::remove_if(accts.begin(), accts.end(),
                                       [&](const AccountConfig& a) { return a.account_id == id; }),
                        accts.end());
            break;
        }
        case TdConfigOp::UpdateAccount: {
            auto id = req.params.value("account_id", "");
            bool found = false;
            for (auto& a : cfg.accounts) {
                if (a.account_id == id) {
                    // account_id 是不可变 key (不修改)
                    // broker 字段级 merge: 未提供的字段保留旧值, password 特殊处理 ****
                    if (req.params.contains("broker")) {
                        const auto& bj = req.params["broker"];
                        a.broker.name = bj.value("name", a.broker.name);
                        a.broker.broker_id = bj.value("broker_id", a.broker.broker_id);
                        a.broker.user_id = bj.value("user_id", a.broker.user_id);
                        a.broker.product_info = bj.value("product_info", a.broker.product_info);
                        // password 特殊处理: "****" 或空串时保留旧密码
                        // (避免脱敏值覆盖真实密码)
                        auto pwd = bj.value("password", "");
                        if (pwd != "****" && !pwd.empty()) {
                            a.broker.password = pwd;
                        }
                        // frontends 是数组, 若提供则整体替换, 不提供则保留
                        if (bj.contains("frontends")) {
                            a.broker.frontends =
                                bj["frontends"].get<std::vector<BrokerFrontend>>();
                        }
                    }
                    a.auth_code = req.params.value("auth_code", a.auth_code);
                    a.app_id = req.params.value("app_id", a.app_id);
                    a.flow_dir = req.params.value("flow_dir", a.flow_dir);
                    a.currency_id = req.params.value("currency_id", a.currency_id);
                    found = true;
                    break;
                }
            }
            // 关键决策: UpdateAccount 不存在抛 std::invalid_argument
            if (!found) {
                throw std::invalid_argument(
                    std::format("updateaccount not found | account_id=\"{}\"", id));
            }
            break;
        }
        case TdConfigOp::SetAccountEnabled: {
            auto id = req.params.value("account_id", "");
            for (auto& a : cfg.accounts) {
                if (a.account_id == id) {
                    a.enabled = req.params.value("enabled", a.enabled);
                    break;
                }
            }
            break;
        }
        case TdConfigOp::SetAccountRiskControl: {
            auto id = req.params.value("account_id", "");
            for (auto& a : cfg.accounts) {
                if (a.account_id == id) {
                    a.risk_control_enabled = req.params.value("enabled", a.risk_control_enabled);
                    break;
                }
            }
            break;
        }
        case TdConfigOp::SetAccountCurrency: {
            auto id = req.params.value("account_id", "");
            for (auto& a : cfg.accounts) {
                if (a.account_id == id) {
                    a.currency_id = req.params.value("currency_id", a.currency_id);
                    break;
                }
            }
            break;
        }
        case TdConfigOp::SetLockMode: {
            cfg.enable_lock_mode = req.params.value("enabled", false);
            break;
        }
        case TdConfigOp::SetQryIntervals: {
            // 关键决策: int_val 越界 (<=0) 抛 std::invalid_argument。
            // 部分更新: 仅校验提供的字段, 未提供的字段保留旧值。
            if (req.params.contains("qry_account_interval_s")) {
                int v = req.params["qry_account_interval_s"].get<int>();
                if (v <= 0) {
                    throw std::invalid_argument(
                        std::format("setqryintervals invalid | qry_account_interval_s={}", v));
                }
                cfg.qry_account_interval_s = v;
            }
            if (req.params.contains("qry_position_interval_s")) {
                int v = req.params["qry_position_interval_s"].get<int>();
                if (v <= 0) {
                    throw std::invalid_argument(
                        std::format("setqryintervals invalid | qry_position_interval_s={}", v));
                }
                cfg.qry_position_interval_s = v;
            }
            if (req.params.contains("qry_flush_interval_ms")) {
                int v = req.params["qry_flush_interval_ms"].get<int>();
                if (v <= 0) {
                    throw std::invalid_argument(
                        std::format("setqryintervals invalid | qry_flush_interval_ms={}", v));
                }
                cfg.qry_flush_interval_ms = v;
            }
            break;
        }
    }
}

// 在 accounts 中查找 account_id 对应的账户 (纯函数, 供单元测试)。
const AccountConfig* find_current_account_in(const std::vector<AccountConfig>& accounts,
                                              const std::string& account_id) {
    if (account_id.empty()) {
        return nullptr;
    }
    for (const auto& a : accounts) {
        if (a.account_id == account_id) {
            return &a;
        }
    }
    return nullptr;
}

// 在 cfg.accounts 中查找 account_id 对应的账户 (纯函数, 供单元测试)。
// account_id 为空或未找到时返回 nullptr。Plan 接口: 接受 TdConfig 引用的便捷重载,
// 委托给 find_current_account_in(cfg.accounts, account_id)。
const AccountConfig* find_account_in(const TdConfig& cfg, const std::string& account_id) {
    return find_current_account_in(cfg.accounts, account_id);
}

// 2115 查询路由: queried 为空串 = 查全部; 否则命中配置才应答 (不在配置不回, master 兜底)
bool account_query_matches(const std::vector<std::string>& configured,
                           const std::string& queried) {
    if (queried.empty()) {
        return true;
    }
    return std::ranges::find(configured, queried) != configured.end();
}

}  // namespace dztrader::ctp
