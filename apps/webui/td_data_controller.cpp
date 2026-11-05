#include "td_data_controller.h"

#include "td_read_db.h"

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

}  // namespace dztrader::webui
