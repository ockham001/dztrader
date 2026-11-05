#include "td_trading_accounts_query.h"

#include "td_account_scope.h"
#include "td_list_query.h"
#include "td_read_db.h"
#include "td_row_json.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::webui {

TdTradingAccountsQuery::TdTradingAccountsQuery(std::function<std::string()> trade_db_path,
                                               int busy_timeout_ms)
    : trade_db_path_(std::move(trade_db_path)),
      busy_timeout_ms_(busy_timeout_ms) {}

TdTradingAccountsQuery::Params TdTradingAccountsQuery::parse(
    const std::function<std::string(std::string_view)>& param) {
    // 资金端点只认 `accounts`（契约 rest §2.9.2：不分页，limit/cursor 被忽略）。
    // 解码口径与委托/成交共用 `td_parse_accounts`，禁止第二份实现。
    return Params{.accounts = td_parse_accounts(param("accounts"))};
}

nlohmann::json TdTradingAccountsQuery::execute(
    const Params& params,
    const std::vector<std::string>& online_accounts,
    const std::optional<std::string>& trading_day,
    const std::vector<std::string>& registry_accounts) const {
    // 信封：与持仓列表同形（契约 rest §2.9.2）。资金行自己的 date 是各账户锚点日，
    // 信封的 trading_day 是列表作用域的当前交易日，两者不同。`next_cursor` 恒 null
    // ——资金端点不分页（契约 §2.9.2），因此信封构造后不再改写它。
    nlohmann::json body = td_list_envelope(trading_day, online_accounts);

    // 无在线账户时仍要探测库可用性并可能返回离线账户的资金——账户全集不只来自在线集合。
    // 因此这里**不能**像委托/成交那样提前返回空列表（契约 rest §2.9.1 持仓/资金口径）。
    TdReadConnection conn = TdReadConnection::open(trade_db_path_(), busy_timeout_ms_);
    if (!conn.connected()) {
        return td_degraded_body(conn.reason());
    }
    sqlite3* db = conn.handle();

    // 账户全集 = 在线 ∪ 注册表 ∪ 库中四表出现过的 account_id（含离线），再过 accounts
    // 过滤。与持仓共用 `td_account_scope`：两份实现会让持仓与资金显示不同的账户集合。
    bool accounts_ok = true;
    const std::vector<std::string> registered =
        td_registered_accounts(db, online_accounts, registry_accounts, &accounts_ok);
    if (!accounts_ok) {
        return td_degraded_body(kTdReasonQueryFailed);
    }
    const std::vector<std::string> scope_accounts = td_scope_accounts(params.accounts, registered);

    // 锚点日（ADR 0046：positions / trading_accounts 两张快照表 MAX(date)）。
    bool anchors_ok = true;
    const std::map<std::string, std::string> anchors =
        td_account_anchor_days(db, scope_accounts, &anchors_ok);
    if (!anchors_ok) {
        return td_degraded_body(kTdReasonQueryFailed);
    }

    // 在锚点日取资金行：只查「(account, date) 对」（条件与绑定由 `td_anchor_pairs` 生成，
    // 持仓端点共用同一份）——主键 (account_id, date) 保证每账户至多一行，无需过滤/去重。
    // 无锚点日的账户不可能有锚点日行（不必进 SQL）。
    std::map<std::string, TdTradingAccountRecord> found;
    const TdAnchorPairs anchor_pairs = td_anchor_pairs(scope_accounts, anchors);
    if (!anchor_pairs.empty()) {
        std::string sql =
            "SELECT account_id, date, balance, available, frozen, commission, margin,"
            " withdraw_quota, deposit, withdraw, seq FROM trading_accounts WHERE ";
        sql += anchor_pairs.sql;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            const int code = sqlite3_extended_errcode(db);
            SPDLOG_WARN("td trading accounts query prepare failed | error=\"{}\"",
                        sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return td_degraded_body(td_sqlite_error_reason(code));
        }
        td_bind_anchor_pairs(stmt, anchor_pairs, 1);
        int step = SQLITE_OK;
        while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
            TdTradingAccountRecord row;
            row.account_id = td_column_text(stmt, 0);
            row.date = td_column_text(stmt, 1);
            row.balance = sqlite3_column_double(stmt, 2);
            row.available = sqlite3_column_double(stmt, 3);
            row.frozen = sqlite3_column_double(stmt, 4);
            row.commission = sqlite3_column_double(stmt, 5);
            row.margin = sqlite3_column_double(stmt, 6);
            row.withdraw_quota = sqlite3_column_double(stmt, 7);
            row.deposit = sqlite3_column_double(stmt, 8);
            row.withdraw = sqlite3_column_double(stmt, 9);
            row.seq = static_cast<uint64_t>(sqlite3_column_int64(stmt, 10));
            found.emplace(row.account_id, std::move(row));
        }
        sqlite3_finalize(stmt);
        if (step != SQLITE_DONE) {
            const int code = sqlite3_extended_errcode(db);
            SPDLOG_WARN("td trading accounts query step failed | error=\"{}\"", sqlite3_errmsg(db));
            return td_degraded_body(td_sqlite_error_reason(code));
        }
    }

    // 每个已登记账户一行（契约 rest §2.9.4：无记录账户仍返回，has_data:false），
    // 行序 = account_id 升序（§2.9.2 资金端点顺序；`scope_accounts` 本身已升序）。
    nlohmann::json items = nlohmann::json::array();
    for (const auto& account : scope_accounts) {
        const auto it = found.find(account);
        if (it != found.end()) {
            items.push_back(td_trading_account_row_json(it->second));
        } else {
            items.push_back(td_trading_account_empty_row_json(account));
        }
    }
    body["items"] = std::move(items);
    return body;
}

}  // namespace dztrader::webui
