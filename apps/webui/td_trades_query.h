#ifndef DZTRADER_WEBUI_TD_TRADES_QUERY_H_
#define DZTRADER_WEBUI_TD_TRADES_QUERY_H_

/**
 * @file td_trades_query.h
 * @brief `GET /api/td/trades` 的只读直查实现
 *        （契约 rest §2.9.1–§2.9.3/§2.9.6/§2.9.8）。
 *
 * 与 `TdOrdersQuery` 同构：参数/账户范围/游标/时间锚来自 `td_list_query.h`，成交行 JSON
 * 来自 `td_row_json.h`，只读连接与降级原因码来自 `td_read_db.h`——本文件只写「成交列表
 * 的 SQL 与游标坐标」这一份差异，不复制上述任何共性实现。
 *
 * 排序（契约 rest §2.9.6）：18:00 时间锚降序，同一秒依次按 `account_id`、`exchange_id`、
 * `trade_id` 的 UTF-8 字节序降序；游标续页条件与排序同源（严格小于坐标元组），禁止 OFFSET。
 *
 * 降级（§2.9.8）：读库失败一律 200 `{"available":false,"reason":<code>}`，不抛、不 5xx。
 */

#include <cstdint>
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

/// 成交列表查询（`GET /api/td/trades`）。
class TdTradesQuery {
public:
    /// 已校验的请求参数（与委托列表同型，唯一声明在 `td_list_query.h`）。
    using Params = TdListParams;

    /// @param trade_db_path 回调返回交易库绝对路径（空串 = 库不可用，降级 not_found）
    /// @param busy_timeout_ms 只读连接等锁上限（契约 rest §2.9：200ms）
    explicit TdTradesQuery(std::function<std::string()> trade_db_path,
                           int busy_timeout_ms = kTdReadBusyTimeoutMs);

    /// 解析并校验 HTTP query 参数：`nullopt` = 应回 400 `{"error":"bad request"}`。
    static std::optional<Params> parse(const std::function<std::string(std::string_view)>& param);

    /// 执行查询并返回响应体（信封与委托列表同形，契约 rest §2.9.2）。
    /// @param online_accounts 在线（Ready）账户集合，用于 accounts 求交
    /// @param trading_day 当前交易日 "YYYYMMDD"（无在线账户 = nullopt）
    /// @return 正常/降级响应体；`nullopt` = 游标非法或作用域不符（应回 400）
    std::optional<nlohmann::json> execute(const Params& params,
                                          const std::vector<std::string>& online_accounts,
                                          const std::optional<std::string>& trading_day) const;

private:
    std::function<std::string()> trade_db_path_;
    int busy_timeout_ms_ = kTdReadBusyTimeoutMs;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_TRADES_QUERY_H_
