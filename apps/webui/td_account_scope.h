#ifndef DZTRADER_WEBUI_TD_ACCOUNT_SCOPE_H_
#define DZTRADER_WEBUI_TD_ACCOUNT_SCOPE_H_

/**
 * @file td_account_scope.h
 * @brief 「全部已登记账户」全集与「账户锚点日」的唯一实现
 *        （契约 rest §2.9.1，ADR 0045/0046）。
 *
 * 持仓（`TdPositionsQuery`）与资金（#250）两个端点共用本文件：
 * - **账户全集** = 在线账户 ∪ `trade.db` 四表中出现过的 `account_id`（含离线）。两份实现
 *   漂移会让持仓与资金在同一时刻显示不同的账户集合，而 UI 是按账户分组展示的。
 * - **锚点日** = 该账户 `positions` 与 `trading_accounts` 两张快照表的 `MAX(date)`（ADR 0046）。
 *   委托/成交（追加流）**不参与**——它们先于快照查询落库，算进来会让「查询失败」被显示成
 *   「空仓」，正是 ADR 0045 决策 3 判定为最危险的形态。
 *
 * 为什么账户全集用四表而锚点日只用两表：前者是「该账户是否存在」的判定，删户后历史数据仍要
 * 可见；后者是「取哪个日期的行」。两件事，故意用不同的表集合。
 */

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace dztrader::webui {

/// 账户全集：在线账户 ∪ 交易库四表中出现过的 account_id（升序去重）。
///
/// @param db 已打开且通过 schema 校验的只读连接（调用方负责生命周期与错误处理）
/// @param online_accounts 在线（Ready）账户集合
/// @param ok 输出：查询是否全部成功。false = 至少一张表查询失败，此时返回值只含
///           能查到的部分（调用方据此决定降级）。可为 nullptr。
/// @return 升序去重的账户全集
std::vector<std::string> td_registered_accounts(sqlite3* db,
                                                const std::vector<std::string>& online_accounts,
                                                bool* ok);

/// 单账户锚点日查询（契约 rest §2.9.1，ADR 0046）：`positions` / `trading_accounts`
/// 两张快照表的 `MAX(date)` 取较大者；两张表都无行 = nullopt。
///
/// @param db 已打开且通过 schema 校验的只读连接
/// @param account_id 账户标识
/// @param ok 输出：查询是否成功。false = 查询执行失败（区分「无记录」与「查不到」）。
/// @return "YYYYMMDD"；无记录 = nullopt
std::optional<std::string> td_account_anchor_day(sqlite3* db,
                                                 const std::string& account_id,
                                                 bool* ok);

/// 批量锚点日查询（持仓/资金列表用）：一次 GROUP BY 取全部账户，避免 N 次查询。
///
/// @param db 已打开且通过 schema 校验的只读连接
/// @param account_ids 待查账户集合
/// @param ok 输出：查询是否成功
/// @return account_id -> "YYYYMMDD"；无快照行的账户**不出现在结果里**
std::map<std::string, std::string> td_account_anchor_days(
    sqlite3* db, const std::vector<std::string>& account_ids, bool* ok);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_ACCOUNT_SCOPE_H_
