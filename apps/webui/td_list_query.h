#ifndef DZTRADER_WEBUI_TD_LIST_QUERY_H_
#define DZTRADER_WEBUI_TD_LIST_QUERY_H_

/**
 * @file td_list_query.h
 * @brief 交易数据列表端点的通用请求参数 / 账户范围 / 游标 / 排序时间锚
 *        （契约 rest §2.9.2/§2.9.6）的**唯一实现**。
 *
 * 委托列表（`TdOrdersQuery`）与成交列表（`TdTradesQuery`）共用本文件：参数校验、
 * 在线集合求交、游标编解码与 18:00 时间锚如果各写一份，两份会随契约修订漂移，
 * 让前端在委托与成交上看到不一致的分页/排序语义（禁 OFFSET、游标绑定作用域）。
 *
 * 游标形态（不透明串）：`hex("1|<16 hex 作用域指纹>|<端点自定义坐标文本>")`。
 * 坐标文本由端点自己定义（委托 = 时间锚 + order_id；成交 = 时间锚 + 业务键），
 * 本文件只负责外层编码、作用域校验与坐标文本透传。
 *
 * 响应信封与「空列表仍探测库可用性」也是委托/成交共用的同一份（契约 rest §2.9.2/§2.9.8）。
 */

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "td_read_db.h"

namespace dztrader::webui {

/// 契约 rest §2.9.2：默认 200 行/页、上限 1000 行/页。
inline constexpr int kTdListDefaultLimit = 200;
inline constexpr int kTdListMaxLimit = 1000;

/// 已校验的列表请求参数（契约 rest §2.9.2）。
struct TdListParams {
    int limit = kTdListDefaultLimit;
    std::string cursor;                 ///< 空 = 首页
    std::vector<std::string> accounts;  ///< 已去空白/去空项/去重排序；空 = 全部在线
};

/// 解析并校验 query：`limit` 越界/非整数、`accounts` 无法解码 → nullopt（应回 400）。
/// @param param 按 key 取原始 query 值（缺省返回空串）
std::optional<TdListParams> td_parse_list_params(
    const std::function<std::string(std::string_view)>& param);

/// 账户范围 = 请求过滤 ∩ 在线集合（排序去重；请求为空 = 全部在线；未知账户忽略）。
std::vector<std::string> td_scope_accounts(const std::vector<std::string>& requested,
                                           const std::vector<std::string>& online_accounts);

/// 游标 = 不透明串：hex("1|<16 hex 作用域指纹>|<端点自定义坐标文本>")。
std::string td_encode_cursor(uint64_t scope, std::string_view coord);

/// 解码并校验作用域；返回坐标文本。非法 / 作用域不符 = nullopt（调用方回 400）。
std::optional<std::string> td_decode_cursor(std::string_view text, uint64_t scope);

/// 作用域指纹 = 端点 + 已归一化 accounts + 交易日（FNV-1a，跨进程稳定）。
uint64_t td_cursor_scope(std::string_view endpoint,
                         const std::vector<std::string>& accounts,
                         std::string_view trading_day);

/// byte → 2 位小写 hex（游标外层编码与成交排序键/坐标共用；hex 字符序 = 字节序）。
std::string td_hex_encode(std::string_view raw);

/// `td_hex_encode` 的逆；非法 hex（奇数长度 / 非 hex 字符）= nullopt。
std::optional<std::string> td_hex_decode(std::string_view text);

/// 排序时间锚 `((time + 18:00) % 24h)`（契约 rest §2.9.6）；负数安全归一。
int32_t td_time_anchor(int32_t time);

/// 同源的 SQL 表达式：`((<alias>.time + 21600) % 86400)`。禁止在 SQL 文本里再写字面量。
std::string td_time_anchor_sql(std::string_view alias);

/// 列表响应信封（契约 rest §2.9.2）：`available/trading_day/online_accounts/items/next_cursor`。
/// 委托与成交共用同一形状，唯一真相源是契约 §2.9.2。
nlohmann::json td_list_envelope(const std::optional<std::string>& trading_day,
                                const std::vector<std::string>& online_accounts);

/// 空列表响应的可用性判定（契约 rest §2.9.8）：库确实可读（能开、schema 版本匹配）→
/// 返回 `body`（available:true + 空 items）；否则整体降级（只有 available/reason）。
///
/// 「今天没数据」与「库读不了」必须可区分，否则前端会把降级显示成 0
/// （契约 webui-ws §5 第 13 条）——因此无在线账户/无交易日时也要走这一步。
nlohmann::json td_list_with_availability(nlohmann::json body,
                                         const std::string& trade_db_path,
                                         int busy_timeout_ms);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_LIST_QUERY_H_
