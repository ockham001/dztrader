#ifndef DZTRADER_WEBUI_TD_POSITIONS_QUERY_H_
#define DZTRADER_WEBUI_TD_POSITIONS_QUERY_H_

/**
 * @file td_positions_query.h
 * @brief `GET /api/td/positions` 的只读直查实现
 *        （契约 rest §2.9.1/§2.9.2/§2.9.4/§2.9.6/§2.9.8）。
 *
 * 与 `TdOrdersQuery` / `TdTradesQuery` 的差异面只有三条，其余全部复用既有唯一实现：
 * - **账户范围不是「在线集合求交」**：持仓覆盖**全部已登记账户（含离线）**，按各账户
 *   锚点日取最新行（`td_account_scope.h`）；委托/成交才是「在线 + 当前交易日」。
 * - **没有时间序与 sort_key**：`positions` 表无 `time` 列，契约 rest §2.9.2 规定游标坐标 =
 *   `(account_id, instrument_id, direction)` **升序**元组，`direction` 全序
 *   Short(-1) < Net(0) < Long(1)。列表顺序即服务端返回顺序。
 * - **`volume = 0` 的行不返回**（契约 rest §2.9.1 空语义）：库里可能残留绝对态清零帧，
 *   那不是持仓事实。过滤在 SQL 层做，否则分页计数会虚高。
 *
 * 降级（§2.9.8）：读库失败一律 200 `{"available":false,"reason":<code>}`，不抛、不 5xx。
 */

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "td_list_query.h"
#include "td_read_db.h"
#include "td_row_json.h"

namespace dztrader::webui {

/// 持仓列表查询（`GET /api/td/positions`）。
class TdPositionsQuery {
public:
    /// 已校验的请求参数：与委托/成交同型（`limit` / `cursor` / `accounts`），唯一声明在
    /// `td_list_query.h`——参数校验语义是共用的，不因端点不同而不同。
    using Params = TdListParams;

    /// @param trade_db_path 回调返回交易库绝对路径（空串 = 库不可用，降级 not_found）
    /// @param busy_timeout_ms 只读连接等锁上限（契约 rest §2.9：200ms）
    explicit TdPositionsQuery(std::function<std::string()> trade_db_path,
                              int busy_timeout_ms = kTdReadBusyTimeoutMs);

    /// 解析并校验 HTTP query 参数：`nullopt` = 应回 400 `{"error":"bad request"}`。
    static std::optional<Params> parse(const std::function<std::string(std::string_view)>& param);

    /// 执行查询并返回响应体（信封与委托/成交同形，契约 rest §2.9.2）。
    ///
    /// @param online_accounts 在线（Ready）账户集合——**只是账户全集的一个来源**，
    ///        与库里出现过的 account_id 求并（契约 §2.9.1），不是范围上限。
    /// @param trading_day 当前交易日（持仓端点不用它过滤，仅用于**保持信封形状一致**；
    ///        持仓行的 `date` 是各账户锚点日，不是列表作用域的当前交易日）
    /// @return 正常/降级响应体；`nullopt` = 游标非法或作用域不符（应回 400）
    std::optional<nlohmann::json> execute(const Params& params,
                                          const std::vector<std::string>& online_accounts,
                                          const std::optional<std::string>& trading_day) const;

private:
    std::function<std::string()> trade_db_path_;
    int busy_timeout_ms_ = kTdReadBusyTimeoutMs;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_POSITIONS_QUERY_H_
