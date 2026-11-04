#ifndef DZTRADER_WEBUI_TD_DATA_CONTROLLER_H_
#define DZTRADER_WEBUI_TD_DATA_CONTROLLER_H_

#include <drogon/HttpController.h>

#include <memory>
#include <string>

#include "controller_base.h"
#include "repository.h"
#include "td_domain_service.h"
#include "td_orders_query.h"

namespace dztrader::webui {

/// 交易数据 REST（契约 rest §2.9）：当前仅委托列表 `GET /api/td/orders`。
///
/// 口径（§2.9.1）：在线（Ready）账户 + 当前交易日，由 `TdDomainService` 解析；记录一律
/// 只读直查交易库（ADR 0043），不读镜像、不走帧。降级（§2.9.8）由 `TdOrdersQuery` 返回
/// 200 + `available:false`，controller 不加自己的错误码。
///
/// 线程：drogon 固定单 IO 线程且查询为同步只读 + 200ms busy_timeout（§2.9），
/// 与 SysmonCtrl 的历史查询同型；无共享可变状态。
class TdCtrl : public drogon::HttpController<TdCtrl, false>, public ControllerBase {
public:
    /// @param repo 用户数据源（认证在 JWT advice 层；读端点不做角色判断）
    /// @param domain TD 数据域服务（在线账户集合 / 当前交易日）
    /// @param orders 委托列表只读查询
    TdCtrl(std::shared_ptr<Repository> repo,
           std::shared_ptr<TdDomainService> domain,
           std::shared_ptr<TdOrdersQuery> orders)
        : ControllerBase(std::move(repo)),
          domain_(std::move(domain)),
          orders_(std::move(orders)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TdCtrl::get_orders, "/api/td/orders", drogon::Get);
    METHOD_LIST_END

    /// GET `/api/td/orders`：参数 `limit`（默认 200，1..1000）、`cursor`、`accounts`（逗号）。
    /// 参数非法 / 游标非法或作用域不符 → 400 `{"error":"bad request"}`；
    /// 列表可用/降级 → 200（降级体 `{"available":false,"reason":<code>}`，契约 §2.9.8）。
    void get_orders(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<TdDomainService> domain_;
    std::shared_ptr<TdOrdersQuery> orders_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_DATA_CONTROLLER_H_
