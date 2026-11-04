#include "td_data_controller.h"

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
        callback(json_response(drogon::k200OK, {{"available", false}, {"reason", "query_failed"}}));
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

}  // namespace dztrader::webui
