#ifndef DZTRADER_WEBUI_TD_TRADING_ACCOUNTS_QUERY_H_
#define DZTRADER_WEBUI_TD_TRADING_ACCOUNTS_QUERY_H_

/**
 * @file td_trading_accounts_query.h
 * @brief `GET /api/td/trading_accounts` 的只读直查实现
 *        （契约 rest §2.9.1/§2.9.2/§2.9.4/§2.9.8）。
 *
 * 与 `TdPositionsQuery` 同属「全部已登记账户（含离线）+ 各账户锚点日」这一类端点，差异面：
 * - **不分页**（契约 rest §2.9.2）：`limit` / `cursor` 被**忽略**（不校验、不参与作用域），
 *   响应 `next_cursor` 恒 `null`；`accounts` 过滤照常生效。
 * - **无资金记录的账户仍返回一行**（契约 rest §2.9.4）：`has_data:false`、`date` / `seq` 与
 *   全部资金字段为 `null`——账户全集来自 `td_account_scope`（与持仓共用单一实现），
 *   行序 = `account_id` 升序。
 * - 资金行 `date` = 该账户**锚点日**（`positions` 与 `trading_accounts` 两张快照表
 *   `MAX(date)`，ADR 0046）。账户在锚点日没有 `trading_accounts` 行 → `has_data:false`
 *   （不拿更早日期的旧行冒充锚点日数据，契约 §2.9.4 字面口径）。
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

/// 资金列表查询（`GET /api/td/trading_accounts`）。
class TdTradingAccountsQuery {
public:
    /// 已校验的请求参数：资金端点**只接受 `accounts` 过滤**——`limit` / `cursor` 被契约
    /// 规定忽略（§2.9.2：资金不分页，next_cursor 恒 null），因此不解析、不校验它们。
    struct Params {
        std::vector<std::string> accounts;  ///< 已去空白/去空项/去重排序；空 = 全部已登记账户
    };

    /// @param trade_db_path 回调返回交易库绝对路径（空串 = 库不可用，降级 not_found）
    /// @param busy_timeout_ms 只读连接等锁上限（契约 rest §2.9：200ms）
    explicit TdTradingAccountsQuery(std::function<std::string()> trade_db_path,
                                    int busy_timeout_ms = kTdReadBusyTimeoutMs);

    /// 解析 `accounts` 参数（契约 rest §2.9.2）。该端点没有会失败的参数校验：
    /// 非法/未知账户由账户全集求交时忽略，`limit` / `cursor` 被忽略。
    static Params parse(const std::function<std::string(std::string_view)>& param);

    /// 执行查询并返回响应体（信封与持仓同形，契约 rest §2.9.2；`next_cursor` 恒 null）。
    ///
    /// @param online_accounts 在线（Ready）账户集合——**只是账户全集的一个来源**，
    ///        与库里出现过的 account_id 求并（契约 §2.9.1），不是范围上限。
    /// @param trading_day 当前交易日（资金端点不用它过滤，仅用于保持信封形状一致；
    ///        资金行的 `date` 是各账户锚点日，不是列表作用域的当前交易日）
    /// @param registry_accounts 账户注册表账户集合（契约 rest §2.9.1 第三源；默认空 =
    ///        注册表未就绪，在线集合与库内四表照常兜底）—影响行集合（每个已登记账户一行，
    ///        无资金记录的账户返回 `has_data:false`），因此第三源到达会让资金列表多出占位行
    /// @return 正常/降级响应体（不会因参数返回 400——资金端点没有游标）
    nlohmann::json execute(const Params& params,
                           const std::vector<std::string>& online_accounts,
                           const std::optional<std::string>& trading_day,
                           const std::vector<std::string>& registry_accounts = {}) const;

private:
    std::function<std::string()> trade_db_path_;
    int busy_timeout_ms_ = kTdReadBusyTimeoutMs;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_TRADING_ACCOUNTS_QUERY_H_
