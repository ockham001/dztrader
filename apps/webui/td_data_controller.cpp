#include "td_data_controller.h"

#include <algorithm>

#include "td_read_db.h"
#include "td_session_guard.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <utility>

namespace dztrader::webui {

void TdCtrl::get_orders(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    // 认证由 JWT advice 处理；读端点全部已认证用户可读全部账户（契约 rest §2.9.1）。
    if (!orders_ || !domain_) {
        // 装配缺失（测试/异常装配）：按降级而非 5xx，避免把配置问题当服务器故障
        // （契约 rest §2.9.8：读端点不上 5xx）。
        SPDLOG_WARN("td orders query not wired");
        callback(json_response(drogon::k200OK, td_degraded_body(kTdReasonQueryFailed)));
        return;
    }
    const auto parsed = TdOrdersQuery::parse(
        [&req](std::string_view key) { return req->getParameter(std::string(key)); });
    if (!parsed.has_value()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    auto body =
        orders_->execute(*parsed, domain_->online_accounts(), domain_->current_trading_day());
    if (!body.has_value()) {
        // 游标非法 / 作用域不符（契约 rest §2.9.2）：必须从首页重拉，属请求错误不是降级。
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    callback(json_response(drogon::k200OK, *body));
}

void TdCtrl::get_trades(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    // 认证由 JWT advice 处理；读端点全部已认证用户可读全部账户（契约 rest §2.9.1）。
    // 与 get_orders 完全同构：参数解析/游标作用域/降级语义的唯一真相源是契约 §2.9，
    // 此处不另写规则。
    if (!trades_ || !domain_) {
        // 装配缺失（测试/异常装配）：按降级而非 5xx，避免把配置问题当服务器故障
        // （契约 rest §2.9.8：读端点不上 5xx）。
        SPDLOG_WARN("td trades query not wired");
        callback(json_response(drogon::k200OK, td_degraded_body(kTdReasonQueryFailed)));
        return;
    }
    const auto parsed = TdTradesQuery::parse(
        [&req](std::string_view key) { return req->getParameter(std::string(key)); });
    if (!parsed.has_value()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    auto body =
        trades_->execute(*parsed, domain_->online_accounts(), domain_->current_trading_day());
    if (!body.has_value()) {
        // 游标非法 / 作用域不符（契约 rest §2.9.2）：必须从首页重拉，属请求错误不是降级。
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    callback(json_response(drogon::k200OK, *body));
}

void TdCtrl::get_positions(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    // 认证由 JWT advice 处理；读端点全部已认证用户可读全部账户（契约 rest §2.9.1）。
    // 与 get_orders 同构（参数解析/游标作用域/降级语义的唯一真相源是契约 §2.9），
    // 差异只在查询对象：持仓的账户全集与锚点日由 TdPositionsQuery 自己从库里取
    // （全部已登记账户，含离线；ADR 0045/0046）。
    if (!positions_ || !domain_) {
        // 装配缺失（测试/异常装配）：按降级而非 5xx，避免把配置问题当服务器故障
        // （契约 rest §2.9.8：读端点不上 5xx）。
        SPDLOG_WARN("td positions query not wired");
        callback(json_response(drogon::k200OK, td_degraded_body(kTdReasonQueryFailed)));
        return;
    }
    const auto parsed = TdPositionsQuery::parse(
        [&req](std::string_view key) { return req->getParameter(std::string(key)); });
    if (!parsed.has_value()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    // 账户全集第三源：注册表账户（网关停机/配置镜像未到时仍在全集里，契约 rest §2.9.1）。
    auto body = positions_->execute(*parsed, domain_->online_accounts(),
                                    domain_->current_trading_day(), registry_accounts());
    if (!body.has_value()) {
        // 游标非法 / 作用域不符（契约 rest §2.9.2）：必须从首页重拉，属请求错误不是降级。
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    callback(json_response(drogon::k200OK, *body));
}

void TdCtrl::get_trading_accounts(const drogon::HttpRequestPtr& req,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    // 认证由 JWT advice 处理；读端点全部已认证用户可读全部账户（契约 rest §2.9.1）。
    // 与持仓端点同型（唯一真相源是契约 §2.9），差异只在查询对象：资金**不分页**，
    // `limit`/`cursor` 被忽略（契约 §2.9.2），因此这里没有 400 路径——只有可用/降级 200。
    if (!trading_accounts_ || !domain_) {
        // 装配缺失（测试/异常装配）：按降级而非 5xx，避免把配置问题当服务器故障
        // （契约 rest §2.9.8：读端点不上 5xx）。
        SPDLOG_WARN("td trading accounts query not wired");
        callback(json_response(drogon::k200OK, td_degraded_body(kTdReasonQueryFailed)));
        return;
    }
    const auto params = TdTradingAccountsQuery::parse(
        [&req](std::string_view key) { return req->getParameter(std::string(key)); });
    callback(json_response(
        drogon::k200OK,
        trading_accounts_->execute(params, domain_->online_accounts(),
                                   domain_->current_trading_day(), registry_accounts())));
}

void TdCtrl::get_stats(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    // 认证由 JWT advice 处理；读端点全部已认证用户可读（契约 rest §2.9.1）。
    // 契约 rest §2.9.5：stats 口径固定为「全部在线账户 + 当前交易日」，**不接受 accounts
    // 过滤**，也没有 limit/cursor——因此本端点**忽略全部 query 参数**，不做任何校验
    // （传了也不 400）。req 只用于保持 handler 签名；显式标记未使用。
    (void)req;
    if (!domain_) {
        // 装配缺失（测试/异常装配）：按未就绪降级而非 5xx（契约 rest §2.9.8 精神）。
        SPDLOG_WARN("td stats domain not wired");
        callback(json_response(drogon::k200OK, td_degraded_body(kTdReasonNotReady)));
        return;
    }
    callback(json_response(drogon::k200OK, domain_->stats_body()));
}

// 契约 rest §2.9.11：资金手动刷新（写端点，fire-and-forget）。
void TdCtrl::refresh_trading_account(const drogon::HttpRequestPtr& req,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                     const std::string& account_id) {
    // 认证由 JWT advice 处理；本端点权限 = 任意已认证用户（与资金读端点一致，非 admin）。
    (void)req;
    // 预检 2：账户注册表解析（契约 rest §2.9.11）。
    if (!registry_) {
        SPDLOG_WARN("td refresh trading account rejected: registry not wired | account={}",
                    account_id);
        callback(error_response(drogon::k503ServiceUnavailable, "account registry unavailable"));
        return;
    }
    if (!registry_->available()) {
        SPDLOG_WARN("td refresh trading account rejected: registry degraded | account={}",
                    account_id);
        callback(error_response(drogon::k503ServiceUnavailable, "account registry unavailable"));
        return;
    }
    const auto entry = registry_->find(account_id);
    if (!entry.has_value()) {
        SPDLOG_WARN("td refresh trading account rejected: account not found | account={}",
                    account_id);
        callback(error_with_code(drogon::k404NotFound, "account not found", "account_not_found"));
        return;
    }
    // 预检 3：目标网关必须是已注册的 dztd_* 且 Running（契约 rest §2.9.11）。
    if (!process_mirror_ || !td_gateway_registered(*process_mirror_, entry->gateway) ||
        !td_gateway_running(*process_mirror_, entry->gateway)) {
        SPDLOG_WARN(
            "td refresh trading account rejected: gateway not running | account={} "
            "gateway={}",
            account_id, entry->gateway);
        callback(
            error_with_code(drogon::k409Conflict, "td gateway not running", "gateway_not_running"));
        return;
    }
    // 预检 4：账户在线（Ready）——离线无会话可查询（契约 rest §2.9.11）。
    const auto online = domain_ ? domain_->online_accounts() : std::vector<std::string>{};
    if (std::find(online.begin(), online.end(), account_id) == online.end()) {
        SPDLOG_WARN("td refresh trading account rejected: account offline | account={}",
                    account_id);
        callback(error_with_code(drogon::k409Conflict, "account offline", "account_offline"));
        return;
    }
    // 预检 5：写通道可用；写失败同样 503（不带 code，同契约 rest §2.9.7）。
    if (!shm_writer_ || !shm_writer_->is_ready()) {
        SPDLOG_ERROR("td refresh trading account rejected: shm writer not available | account={}",
                     account_id);
        callback(error_response(drogon::k503ServiceUnavailable, "shm writer not available"));
        return;
    }
    if (!shm_writer_->write_td_refresh_req(account_id, /*request_id=*/0)) {
        SPDLOG_ERROR("td refresh trading account dispatch failed | account={}", account_id);
        callback(error_response(drogon::k503ServiceUnavailable, "shm write failed"));
        return;
    }
    SPDLOG_INFO("td refresh trading account dispatched | account={} gateway={}", account_id,
                entry->gateway);
    callback(json_response(drogon::k200OK, nlohmann::json{{"ok", true}}));
}

}  // namespace dztrader::webui
