#ifndef DZTRADER_WEBUI_TD_DATA_CONTROLLER_H_
#define DZTRADER_WEBUI_TD_DATA_CONTROLLER_H_

#include <drogon/HttpController.h>

#include <memory>
#include <string>

#include "controller_base.h"
#include "repository.h"
#include "td_domain_service.h"
#include "td_orders_query.h"
#include "td_positions_query.h"
#include "td_trading_accounts_query.h"
#include "td_trades_query.h"

namespace dztrader::webui {

/// 交易数据 REST（契约 rest §2.9）：委托列表 `GET /api/td/orders` + 成交列表
/// `GET /api/td/trades` + 持仓列表 `GET /api/td/positions` + 资金列表
/// `GET /api/td/trading_accounts` + 统计 `GET /api/td/stats`。
///
/// 委托/成交列表口径（§2.9.1）：在线（Ready）账户 + 当前交易日，由 `TdDomainService` 解析；
/// 持仓/资金列表口径不同：**全部已登记账户（含离线）+ 各账户锚点日**，账户全集与锚点日由
/// 查询对象自己从库里取（ADR 0045/0046）。记录一律只读直查交易库（ADR 0043），
/// 不读镜像、不走帧。降级（§2.9.8）由查询对象返回 200 + `available:false`，controller
/// 不加自己的错误码。
///
/// 统计（§2.9.5）：读 dzweb 内存计数器（`TdDomainService::stats_body()`），不直查库、
/// 不接受 `accounts` 过滤；未装配 / 未就绪 → 200 `{"available":false,"reason":"not_ready"}`。
///
/// 线程：drogon 固定单 IO 线程且查询为同步只读 + 200ms busy_timeout（§2.9），
/// 与 SysmonCtrl 的历史查询同型；无共享可变状态。
class TdCtrl : public drogon::HttpController<TdCtrl, false>, public ControllerBase {
public:
    /// @param repo 用户数据源（认证在 JWT advice 层；读端点不做角色判断）
    /// @param domain TD 数据域服务（在线账户集合 / 当前交易日 / 统计计数器）
    /// @param orders 委托列表只读查询
    /// @param trades 成交列表只读查询
    /// @param positions 持仓列表只读查询
    /// @param trading_accounts 资金列表只读查询（不分页，契约 rest §2.9.2）
    TdCtrl(std::shared_ptr<Repository> repo,
           std::shared_ptr<TdDomainService> domain,
           std::shared_ptr<TdOrdersQuery> orders,
           std::shared_ptr<TdTradesQuery> trades,
           std::shared_ptr<TdPositionsQuery> positions,
           std::shared_ptr<TdTradingAccountsQuery> trading_accounts)
        : ControllerBase(std::move(repo)),
          domain_(std::move(domain)),
          orders_(std::move(orders)),
          trades_(std::move(trades)),
          positions_(std::move(positions)),
          trading_accounts_(std::move(trading_accounts)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TdCtrl::get_orders, "/api/td/orders", drogon::Get);
    ADD_METHOD_TO(TdCtrl::get_trades, "/api/td/trades", drogon::Get);
    ADD_METHOD_TO(TdCtrl::get_positions, "/api/td/positions", drogon::Get);
    ADD_METHOD_TO(TdCtrl::get_trading_accounts, "/api/td/trading_accounts", drogon::Get);
    ADD_METHOD_TO(TdCtrl::get_stats, "/api/td/stats", drogon::Get);
    METHOD_LIST_END

    /// GET `/api/td/orders`：参数 `limit`（默认 200，1..1000）、`cursor`、`accounts`（逗号）。
    /// 参数非法 / 游标非法或作用域不符 → 400 `{"error":"bad request"}`；
    /// 列表可用/降级 → 200（降级体 `{"available":false,"reason":<code>}`，契约 §2.9.8）。
    void get_orders(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// GET `/api/td/trades`：参数与 `get_orders` 完全同构（`limit`/`cursor`/`accounts`）。
    /// 参数非法 / 游标非法或作用域不符 → 400 `{"error":"bad request"}`；
    /// 列表可用/降级 → 200（降级体 `{"available":false,"reason":<code>}`，契约 §2.9.8）。
    void get_trades(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// GET `/api/td/positions`：参数与 `get_orders` 同构（`limit`/`cursor`/`accounts`），
    /// 但**账户范围是全部已登记账户（含离线）**、游标坐标是
    /// `(account_id, instrument_id, direction)` 升序（契约 §2.9.1/§2.9.2）。
    /// 参数非法 / 游标非法或作用域不符 → 400；列表可用/降级 → 200（契约 §2.9.8）。
    void get_positions(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// GET `/api/td/trading_accounts`：**只接受 `accounts`**（`limit`/`cursor` 被忽略，
    /// 契约 §2.9.2：资金不分页、`next_cursor` 恒 null）；账户范围 = 全部已登记账户
    /// （含离线），按各账户锚点日取行，无记录账户返回 `has_data:false` 占位行
    /// （契约 §2.9.4）。该端点没有 400 参数错误；可用/降级 → 200（契约 §2.9.8）。
    void get_trading_accounts(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// GET `/api/td/stats`：**忽略全部 query 参数**（stats 不接受 accounts/limit/cursor，
    /// 契约 §2.9.5；传了也不 400）。未装配 / 未就绪 → 200 `not_ready`，不返回 5xx
    /// （§2.9.8 精神）。
    void get_stats(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<TdDomainService> domain_;
    std::shared_ptr<TdOrdersQuery> orders_;
    std::shared_ptr<TdTradesQuery> trades_;
    std::shared_ptr<TdPositionsQuery> positions_;
    std::shared_ptr<TdTradingAccountsQuery> trading_accounts_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_DATA_CONTROLLER_H_
