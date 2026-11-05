#include "td_positions_query.h"

#include "td_account_scope.h"
#include "td_list_query.h"
#include "td_read_db.h"
#include "td_row_json.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <charconv>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::webui {

namespace {

/// 持仓列表的游标作用域端点标识（参与 `td_cursor_scope` 指纹，契约 rest §2.9.2）。
/// 与委托/成交端点指纹不同 → 跨端点游标必被拒（作用域不符 → 400）。
constexpr const char* kPositionsEndpoint = "GET /api/td/positions";

/// 持仓游标坐标（契约 rest §2.9.2）：`(account_id, instrument_id, direction)` **升序**元组，
/// 下一页从该元组之后继续。`direction` 全序 Short(-1) < Net(0) < Long(1)（数值即全序，
/// 所以 SQL 直接按 `direction` 整数列升序即可）。
struct PositionsCursor {
    std::string account_id;
    std::string instrument_id;
    int direction = 0;
};

/// 坐标文本 = `"<hex(account)>\x1f<hex(instrument)>\x1f<direction>"`。
/// 业务键两段用 hex 编码保证定界安全；direction 是整数，定宽无需编码。
std::string encode_positions_coord(const PositionsCursor& cursor) {
    return std::format("{}\x1f{}\x1f{}", td_hex_encode(cursor.account_id),
                       td_hex_encode(cursor.instrument_id), cursor.direction);
}

std::optional<PositionsCursor> decode_positions_coord(std::string_view text) {
    const auto first = text.find('\x1f');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const auto second = text.find('\x1f', first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<std::string> account = td_hex_decode(text.substr(0, first));
    const std::optional<std::string> instrument =
        td_hex_decode(text.substr(first + 1, second - first - 1));
    if (!account.has_value() || !instrument.has_value()) {
        return std::nullopt;
    }
    const std::string_view direction_text = text.substr(second + 1);
    int direction = 0;
    const auto parsed = std::from_chars(direction_text.data(),
                                        direction_text.data() + direction_text.size(), direction);
    if (parsed.ec != std::errc{} || parsed.ptr != direction_text.data() + direction_text.size()) {
        return std::nullopt;
    }
    return PositionsCursor{
        .account_id = *account, .instrument_id = *instrument, .direction = direction};
}

/// 锚点日作用域（契约 rest §2.9.2：持仓的作用域 = 端点 + accounts + 各账户锚点日集合）。
/// 把 (account, date) 配对拼进指纹——只拼 date 集合会让「A 今日 B 昨日」与「A 昨日 B 今日」
/// 撞成同一作用域。
std::string anchor_scope_token(const std::map<std::string, std::string>& anchors,
                               const std::vector<std::string>& accounts) {
    std::string token;
    for (const auto& account : accounts) {
        token += '\x1f';
        token += account;
        token += '\x1e';
        const auto it = anchors.find(account);
        if (it != anchors.end()) {
            token += it->second;
        }
    }
    return token;
}

/// 该合约的价格小数位（契约 rest §2.9.6）；`price_tick` 缺失 / ≤ 0 由
/// `td_price_decimals` 走价格兜底。
int decimals_for_tick(double price_tick, double price) {
    return td_price_decimals(price_tick, price);
}

}  // namespace

TdPositionsQuery::TdPositionsQuery(std::function<std::string()> trade_db_path, int busy_timeout_ms)
    : trade_db_path_(std::move(trade_db_path)),
      busy_timeout_ms_(busy_timeout_ms) {}

std::optional<TdPositionsQuery::Params> TdPositionsQuery::parse(
    const std::function<std::string(std::string_view)>& param) {
    // 参数校验是三个列表端点共用的同一份实现（`td_list_query.h`）。
    return td_parse_list_params(param);
}

std::optional<nlohmann::json> TdPositionsQuery::execute(
    const Params& params,
    const std::vector<std::string>& online_accounts,
    const std::optional<std::string>& trading_day) const {
    // 信封：持仓端点的 trading_day 是「列表作用域的当前交易日」，与委托/成交同义
    // （契约 rest §2.9.2）。持仓行自己的 date 是各账户锚点日（§2.9.4），两者不同。
    nlohmann::json body = td_list_envelope(trading_day, online_accounts);

    // 无在线账户时仍要探测库可用性并可能返回离线账户的持仓——账户全集不只来自在线集合。
    // 因此这里**不能**像委托/成交那样提前返回空列表（契约 rest §2.9.1 持仓口径）。
    TdReadConnection conn = TdReadConnection::open(trade_db_path_(), busy_timeout_ms_);
    if (!conn.connected()) {
        return td_degraded_body(conn.reason());
    }
    sqlite3* db = conn.handle();

    // 账户全集 = 在线 ∪ 库中四表出现过的 account_id（含离线），再过 accounts 过滤。
    bool accounts_ok = true;
    const std::vector<std::string> registered =
        td_registered_accounts(db, online_accounts, &accounts_ok);
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

    // 游标作用域（契约 rest §2.9.2）：端点 + accounts + 各账户锚点日集合。
    // 校验在「空列表」早退之前——作用域不符/非法游标一律 400，不静默返回空页。
    const std::string scope_token = anchor_scope_token(anchors, scope_accounts);
    const uint64_t scope = td_cursor_scope(kPositionsEndpoint, params.accounts, scope_token);
    std::optional<PositionsCursor> cursor;
    if (!params.cursor.empty()) {
        const std::optional<std::string> coord = td_decode_cursor(params.cursor, scope);
        if (!coord.has_value()) {
            return std::nullopt;
        }
        cursor = decode_positions_coord(*coord);
        if (!cursor.has_value()) {
            return std::nullopt;
        }
    }

    // 无账户 / 无锚点日：空列表语义（不是错误）。
    if (scope_accounts.empty() || anchors.empty()) {
        body["items"] = nlohmann::json::array();
        return body;
    }

    // 主查询：按账户逐个锚点日取行（行 `date` 必须等于该账户自己的锚点日）。
    // LEFT JOIN instruments 取 price_tick；`volume > 0` 在 SQL 过滤（契约 §2.9.1 空语义）。
    std::string sql =
        "SELECT p.account_id, p.date, p.instrument_id, p.exchange_id, p.direction,"
        " p.volume, p.frozen_volume, p.today_volume, p.yd_volume, p.price, p.seq, i.price_tick"
        " FROM positions p LEFT JOIN instruments i ON i.instrument_id = p.instrument_id"
        " WHERE p.volume > 0 AND p.account_id IN (";
    for (size_t i = 0; i < scope_accounts.size(); ++i) {
        sql += i == 0 ? "?" : ",?";
    }
    sql += ") AND (";
    bool first_pair = true;
    for (const auto& account : scope_accounts) {
        const auto it = anchors.find(account);
        if (it == anchors.end()) {
            continue;  // 该账户无任何快照行 → 无锚点日 → 不可能有持仓行
        }
        sql += first_pair ? "(" : " OR (";
        sql += "p.account_id = ? AND p.date = ?)";
        first_pair = false;
    }
    if (first_pair) {
        body["items"] = nlohmann::json::array();
        return body;
    }
    sql += ")";
    if (cursor.has_value()) {
        // 游标坐标 = (account_id, instrument_id, direction) 升序元组，下一页取「更大」的行
        // （严格大于）。禁用 OFFSET（契约 rest §2.9.2）。
        sql += std::format(
            " AND (p.account_id > ? OR (p.account_id = ? AND (p.instrument_id > ? OR"
            " (p.instrument_id = ? AND p.direction > ?))))");
    }
    sql += " ORDER BY p.account_id ASC, p.instrument_id ASC, p.direction ASC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        const int code = sqlite3_extended_errcode(db);
        SPDLOG_WARN("td positions query prepare failed | error=\"{}\"", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return td_degraded_body(td_sqlite_error_reason(code));
    }

    // 绑定顺序 = SQL 出现顺序：账户集合、各账户锚点日对、游标坐标、limit+1。
    int bind_index = 1;
    for (const auto& account : scope_accounts) {
        sqlite3_bind_text(stmt, bind_index++, account.c_str(), -1, SQLITE_TRANSIENT);
    }
    for (const auto& account : scope_accounts) {
        const auto it = anchors.find(account);
        if (it == anchors.end()) {
            continue;
        }
        sqlite3_bind_text(stmt, bind_index++, account.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bind_index++, it->second.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (cursor.has_value()) {
        sqlite3_bind_text(stmt, bind_index++, cursor->account_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bind_index++, cursor->account_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bind_index++, cursor->instrument_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bind_index++, cursor->instrument_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, bind_index++, cursor->direction);
    }
    // 多取一行判断是否还有下一页。
    sqlite3_bind_int(stmt, bind_index, params.limit + 1);

    nlohmann::json items = nlohmann::json::array();
    int fetched = 0;
    bool has_more = false;
    PositionsCursor last{};
    int step = SQLITE_OK;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (fetched == params.limit) {
            has_more = true;
            break;
        }
        TdPositionRecord row;
        row.account_id = td_column_text(stmt, 0);
        row.date = td_column_text(stmt, 1);
        row.instrument_id = td_column_text(stmt, 2);
        row.exchange_id = td_column_text(stmt, 3);
        row.direction = sqlite3_column_int(stmt, 4);
        row.volume = sqlite3_column_int(stmt, 5);
        row.frozen_volume = sqlite3_column_int(stmt, 6);
        row.today_volume = sqlite3_column_int(stmt, 7);
        row.yd_volume = sqlite3_column_int(stmt, 8);
        row.price = sqlite3_column_double(stmt, 9);
        row.seq = static_cast<uint64_t>(sqlite3_column_int64(stmt, 10));
        const double price_tick =
            sqlite3_column_type(stmt, 11) == SQLITE_NULL ? 0.0 : sqlite3_column_double(stmt, 11);

        items.push_back(td_position_row_json(row, decimals_for_tick(price_tick, row.price)));
        last.account_id = row.account_id;
        last.instrument_id = row.instrument_id;
        last.direction = row.direction;
        ++fetched;
    }
    sqlite3_finalize(stmt);
    if (step != SQLITE_ROW && step != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(db);
        SPDLOG_WARN("td positions query step failed | error=\"{}\"", sqlite3_errmsg(db));
        return td_degraded_body(td_sqlite_error_reason(code));
    }

    body["items"] = std::move(items);
    if (has_more) {
        body["next_cursor"] = td_encode_cursor(scope, encode_positions_coord(last));
    }
    return body;
}

}  // namespace dztrader::webui
