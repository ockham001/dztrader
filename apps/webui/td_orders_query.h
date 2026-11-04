#ifndef DZTRADER_WEBUI_TD_ORDERS_QUERY_H_
#define DZTRADER_WEBUI_TD_ORDERS_QUERY_H_

/**
 * @file td_orders_query.h
 * @brief `GET /api/td/orders` 的只读直查实现（契约 rest §2.9.1–§2.9.3/§2.9.6/§2.9.8）。
 *
 * 分层（与 sysmon_history_service 一致）：
 * - **本文件**负责：每请求独立只读连接、委托列表 SQL 与游标分页；
 * - **`td_list_query.h`** 是列表通用参数/账户范围/游标/时间锚的唯一实现（委托与成交共用）；
 * - **`td_row_json.h`** 是委托行/成交行的内部形态与行 JSON 唯一实现（REST 与 WS 共用）；
 * - **controller** 只做参数来源适配与响应码选择；
 * - **域服务**复用 `td_order_row_json` 生成 WS `td_order_report` 的同 schema 载荷
 *   （契约 webui-ws §2.5：payload 与 REST 行同形状，唯一真相源是契约 rest §2.9.3）。
 *
 * 降级（§2.9.8）：读库失败一律 200 `{"available":false,"reason":<code>}`，
 * 不抛异常、不返回 5xx、不阻塞 IO 线程。
 *
 * 线程约定：每次调用独立打开只读连接（REST 回调在同一 IO 线程串行），无共享状态。
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

/// dzweb 只读端认可的 trade.db schema 版本（SQLite `PRAGMA user_version`，契约 rest §2.9.8）。
///
/// **必须等于** `dztrader::tdstore::kTdSchemaVersion`：dzweb 的生产链接不引入 dztdstore
/// （只是读库，不需要建库/声明门面），因此这条耦合由测试守卫——
/// `td_orders_query_test.cpp` 里 `static_assert(kTdOrdersReadSchemaVersion ==
/// tdstore::kTdSchemaVersion)` 会在 schema 升版而此处忘改时直接编译失败。
inline constexpr int kTdOrdersReadSchemaVersion = 1;

/// 查合约 `instruments.price_tick`（WS 转发事件推导精度用；库/合约不可用 = nullopt，
/// 调用方走价格小数位兜底，契约 rest §2.9.6）。打开失败不抛、不降级整条事件。
std::optional<double> td_query_price_tick(const std::string& trade_db_path,
                                          int busy_timeout_ms,
                                          const std::string& instrument_id);

/// 委托列表查询（`GET /api/td/orders`）。
class TdOrdersQuery {
public:
    /// 已校验的请求参数（与成交列表同型，唯一声明在 `td_list_query.h`）。
    using Params = TdListParams;

    /// @param trade_db_path 回调返回交易库绝对路径（空串 = 库不可用，降级 not_found）
    /// @param busy_timeout_ms 只读连接等锁上限（契约 rest §2.9：200ms，常量唯一声明在
    ///        `td_read_db.h` 的 `kTdReadBusyTimeoutMs`）
    explicit TdOrdersQuery(std::function<std::string()> trade_db_path,
                           int busy_timeout_ms = kTdReadBusyTimeoutMs);

    /// 解析并校验 HTTP query 参数：`nullopt` = 应回 400 `{"error":"bad request"}`
    /// （limit 越界/非整数、cursor/accounts 无法解码）。
    static std::optional<Params> parse(const std::function<std::string(std::string_view)>& param);

    /// 执行查询并返回响应体。
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

#endif  // DZTRADER_WEBUI_TD_ORDERS_QUERY_H_
