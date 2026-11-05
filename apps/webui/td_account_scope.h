#ifndef DZTRADER_WEBUI_TD_ACCOUNT_SCOPE_H_
#define DZTRADER_WEBUI_TD_ACCOUNT_SCOPE_H_

/**
 * @file td_account_scope.h
 * @brief 「全部已登记账户」全集与「账户锚点日」的唯一实现
 *        （契约 rest §2.9.1，ADR 0045/0046）。
 *
 * 持仓（`TdPositionsQuery`）与资金（`TdTradingAccountsQuery`）两个端点共用本文件：
 * - **账户全集** = 在线账户 ∪ 账户注册表 ∪ `trade.db` 四表中出现过的 `account_id`（含离线）。
 *   三源取并集（契约 rest §2.9.1）：注册表是**第三源**，让网关停机 / 配置镜像尚未到达时账户
 *   仍在全集里；注册表是投影不是真相源，缺它时前两个来源仍能兜底。并集只在本文件的
 *   `td_registered_accounts` 实现一次——持仓与资金两个调用方各写一份会让它们在同一时刻
 *   显示不同的账户集合，而 UI 是按账户分组展示的。
 * - **锚点日** = 该账户 `positions` 与 `trading_accounts` 两张快照表的 `MAX(date)`（ADR 0046）。
 *   委托/成交（追加流）**不参与**——它们先于快照查询落库，算进来会让「查询失败」被显示成
 *   「空仓」，正是 ADR 0045 决策 3 判定为最危险的形态。
 * - **锚点日取行的 `(account_id, date)` 条件与绑定**（`td_anchor_pairs` /
 *   `td_bind_anchor_pairs`）也在这里：两个端点的取行 SQL 逐行同形，只有表名/列前缀不同，
 *   两份实现会让锚点语义变更时改一处漏一处。
 *
 * 为什么账户全集用四表而锚点日只用两表：前者是「该账户是否存在」的判定，删户后历史数据仍要
 * 可见；后者是「取哪个日期的行」。两件事，故意用不同的表集合。
 */

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace dztrader::webui {

/// 账户全集：在线账户 ∪ 账户注册表 ∪ 交易库四表中出现过的 account_id（升序去重）。
///
/// @param db 已打开且通过 schema 校验的只读连接（调用方负责生命周期与错误处理）
/// @param online_accounts 在线（Ready）账户集合
/// @param registry_accounts 账户注册表账户集合（契约 rest §2.9.1 第三源；空 = 注册表未就绪，
///        前两源照常兜底）。**只传 account_id 集合，不传 gateway**——本函数只回答
///        「全集有哪些账户」，归属由注册表自己的读端点回答
/// @param ok 输出：查询是否全部成功。false = 至少一张表查询失败，此时返回值只含
///           能查到的部分（调用方据此决定降级）。可为 nullptr。
/// @return 升序去重的账户全集
std::vector<std::string> td_registered_accounts(sqlite3* db,
                                                const std::vector<std::string>& online_accounts,
                                                const std::vector<std::string>& registry_accounts,
                                                bool* ok);

/// 批量锚点日查询（持仓/资金列表用）：一次 GROUP BY 取全部账户，避免 N 次查询。
///
/// @param db 已打开且通过 schema 校验的只读连接
/// @param account_ids 待查账户集合
/// @param ok 输出：查询是否成功
/// @return account_id -> "YYYYMMDD"；无快照行的账户**不出现在结果里**
std::map<std::string, std::string> td_account_anchor_days(
    sqlite3* db, const std::vector<std::string>& account_ids, bool* ok);

/// 一次「按各账户锚点日取行」查询所需的 `(account_id, date)` 对（持仓 / 资金共用）。
///
/// 两个端点的取行条件逐行同形（只有表名/列前缀不同）：各账户在自己的锚点日上取行。
/// 条件文本与绑定顺序收敛到本结构，避免锚点语义变化时改两处（§2.4 同形实现）。
struct TdAnchorPairs {
    struct Pair {
        std::string account_id;
        std::string date;
    };
    std::vector<Pair> pairs;
    /// 形如 `"(<prefix>account_id = ? AND <prefix>date = ?) OR ..."`；无锚点对时为空串
    /// （调用方应早退或返回空列表）。
    std::string sql;

    [[nodiscard]] bool empty() const noexcept { return pairs.empty(); }
};

/// 由账户集合与锚点日映射构造条件（顺序 = `accounts` 顺序，跳过无锚点日的账户）。
///
/// @param column_prefix SQL 列前缀（有表别名时传 `"p."`，无别名传空串）
TdAnchorPairs td_anchor_pairs(const std::vector<std::string>& accounts,
                              const std::map<std::string, std::string>& anchors,
                              std::string_view column_prefix = {});

/// 把锚点对依次绑定到 `stmt` 的连续参数位（`account_id, date, account_id, date, …`）。
/// @return 下一个未使用的参数下标（调用方继续绑定游标 / limit）
int td_bind_anchor_pairs(sqlite3_stmt* stmt, const TdAnchorPairs& pairs, int first_index);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_ACCOUNT_SCOPE_H_
