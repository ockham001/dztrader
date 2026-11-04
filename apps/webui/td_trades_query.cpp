#include "td_trades_query.h"

#include "td_list_query.h"
#include "td_read_db.h"
#include "td_row_json.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::webui {

namespace {

/// 成交列表的游标作用域端点标识（参与 `td_cursor_scope` 指纹，契约 rest §2.9.2）。
/// 与委托端点的指纹不同 → 委托游标拿到成交端点必被拒（作用域不符 → 400）。
constexpr const char* kTradesEndpoint = "GET /api/td/trades";

/// 成交游标坐标（契约 rest §2.9.2/§2.9.6）：与 sort_key 同源的
/// (时间锚, account_id, exchange_id, trade_id) 降序元组。
struct TradesCursor {
    int32_t anchor = 0;
    std::string account_id;
    std::string exchange_id;
    std::string trade_id;
};

/// 坐标文本 = `"%05d|<hex(account)>\x1f<hex(exchange)>\x1f<hex(trade_id)>"`。
/// 业务键三段用 hex 编码保证定界安全（不经 UTF-8 引号/控制字符污染外层串）。
std::string encode_trades_coord(const TradesCursor& cursor) {
    return std::format("{:05d}|{}\x1f{}\x1f{}", cursor.anchor, td_hex_encode(cursor.account_id),
                       td_hex_encode(cursor.exchange_id), td_hex_encode(cursor.trade_id));
}

std::optional<TradesCursor> decode_trades_coord(std::string_view text) {
    if (text.size() < 5 + 1 || text[5] != '|') {
        return std::nullopt;
    }
    int32_t anchor = 0;
    const auto anchor_result = std::from_chars(text.data(), text.data() + 5, anchor);
    if (anchor_result.ec != std::errc{} || anchor_result.ptr != text.data() + 5) {
        return std::nullopt;
    }
    const std::string_view rest = text.substr(6);
    const auto first = rest.find('\x1f');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const auto second = rest.find('\x1f', first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<std::string> account = td_hex_decode(rest.substr(0, first));
    const std::optional<std::string> exchange =
        td_hex_decode(rest.substr(first + 1, second - first - 1));
    const std::optional<std::string> trade = td_hex_decode(rest.substr(second + 1));
    if (!account.has_value() || !exchange.has_value() || !trade.has_value()) {
        return std::nullopt;
    }
    return TradesCursor{
        .anchor = anchor, .account_id = *account, .exchange_id = *exchange, .trade_id = *trade};
}

}  // namespace

TdTradesQuery::TdTradesQuery(std::function<std::string()> trade_db_path, int busy_timeout_ms)
    : trade_db_path_(std::move(trade_db_path)),
      busy_timeout_ms_(busy_timeout_ms) {}

std::optional<TdTradesQuery::Params> TdTradesQuery::parse(
    const std::function<std::string(std::string_view)>& param) {
    // 参数校验是委托/成交共用的同一份实现（`td_list_query.h`）。
    return td_parse_list_params(param);
}

std::optional<nlohmann::json> TdTradesQuery::execute(
    const Params& params,
    const std::vector<std::string>& online_accounts,
    const std::optional<std::string>& trading_day) const {
    // 账户范围 = 请求过滤 ∩ 在线集合（缺省 = 全部在线；未知账户忽略）。
    std::vector<std::string> scope_accounts = td_scope_accounts(params.accounts, online_accounts);

    nlohmann::json body = td_list_envelope(trading_day, online_accounts);

    // 游标作用域（契约 rest §2.9.2）：端点 + accounts + 当前交易日。任一项变化都必须从
    // 首页重拉。校验在「空列表」早退之前——作用域不符/非法游标一律 400，不静默返回空页。
    const std::optional<uint64_t> scope = trading_day.has_value()
                                              ? std::optional<uint64_t>(td_cursor_scope(
                                                    kTradesEndpoint, params.accounts, *trading_day))
                                              : std::nullopt;
    std::optional<TradesCursor> cursor;
    if (!params.cursor.empty()) {
        if (!scope.has_value()) {
            return std::nullopt;  // 无当前交易日 = 无有效游标作用域
        }
        const std::optional<std::string> coord = td_decode_cursor(params.cursor, *scope);
        if (!coord.has_value()) {
            return std::nullopt;
        }
        cursor = decode_trades_coord(*coord);
        if (!cursor.has_value()) {
            return std::nullopt;
        }
    }

    // 无在线账户 / 无当前交易日：空列表语义（不是错误）。但仍需探测交易库可用性——
    // 「库不可读」与「今天没数据」是不同事实（契约 rest §2.9.8）。
    if (!scope.has_value() || scope_accounts.empty()) {
        return td_list_with_availability(std::move(body), trade_db_path_(), busy_timeout_ms_);
    }

    // 只读连接 + schema 版本校验（契约 rest §2.9.8），与委托列表/统计重算同一实现。
    TdReadConnection conn = TdReadConnection::open(trade_db_path_(), busy_timeout_ms_);
    if (!conn.connected()) {
        return td_degraded_body(conn.reason());
    }
    sqlite3* db = conn.handle();

    // 主查询：LEFT JOIN instruments 取 price_tick（查不到走成交价小数位兜底）。
    // 成交行**没有** avg_price/avg_price_decimals（契约 rest §2.9.3 成交行字段表）。
    std::string sql =
        "SELECT t.account_id, t.date, t.trade_id, t.order_id, t.strategy_id,"
        " t.instrument_id, t.exchange_id, t.direction, t.position_effect, t.price,"
        " t.volume, t.time, t.seq, i.price_tick"
        " FROM trades t LEFT JOIN instruments i ON i.instrument_id = t.instrument_id"
        " WHERE t.date = ? AND t.account_id IN (";
    for (size_t i = 0; i < scope_accounts.size(); ++i) {
        sql += i == 0 ? "?" : ",?";
    }
    sql += ")";
    const std::string anchor_sql = td_time_anchor_sql("t");
    if (cursor.has_value()) {
        // 游标坐标 = 与 sort_key 同源的 (时间锚, account_id, exchange_id, trade_id) 降序元组，
        // 下一页取「更旧」的行（严格小于）。禁用 OFFSET（契约 rest §2.9.2）。
        sql += std::format(
            " AND ({} < ? OR ({} = ? AND (t.account_id < ? OR (t.account_id = ? AND"
            " (t.exchange_id < ? OR (t.exchange_id = ? AND t.trade_id < ?))))))",
            anchor_sql, anchor_sql);
    }
    sql += std::format(
        " ORDER BY {} DESC, t.account_id DESC, t.exchange_id DESC, t.trade_id DESC"
        " LIMIT ?",
        anchor_sql);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        const int code = sqlite3_extended_errcode(db);
        SPDLOG_WARN("td trades query prepare failed | error=\"{}\"", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return td_degraded_body(td_sqlite_error_reason(code));
    }

    // 绑定顺序 = SQL 出现顺序：交易日、账户集合、游标坐标（时间锚 ×2 + 三业务键）。
    int bind_index = 1;
    sqlite3_bind_text(stmt, bind_index++, trading_day->c_str(), -1, SQLITE_TRANSIENT);
    for (const auto& account : scope_accounts) {
        sqlite3_bind_text(stmt, bind_index++, account.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (cursor.has_value()) {
        sqlite3_bind_int(stmt, bind_index++, cursor->anchor);
        sqlite3_bind_int(stmt, bind_index++, cursor->anchor);
        sqlite3_bind_text(stmt, bind_index++, cursor->account_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bind_index++, cursor->account_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bind_index++, cursor->exchange_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bind_index++, cursor->exchange_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bind_index++, cursor->trade_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    // 多取一行判断是否还有下一页。
    sqlite3_bind_int(stmt, bind_index, params.limit + 1);

    nlohmann::json items = nlohmann::json::array();
    int fetched = 0;
    bool has_more = false;
    TradesCursor last{};
    int step = SQLITE_OK;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (fetched == params.limit) {
            has_more = true;
            break;
        }
        TdTradeRecord row;
        row.account_id = td_column_text(stmt, 0);
        row.date = td_column_text(stmt, 1);
        row.trade_id = td_column_text(stmt, 2);
        row.order_id = sqlite3_column_int64(stmt, 3);
        row.strategy_id = td_column_text(stmt, 4);
        row.instrument_id = td_column_text(stmt, 5);
        row.exchange_id = td_column_text(stmt, 6);
        row.direction = sqlite3_column_int(stmt, 7);
        row.position_effect = sqlite3_column_int(stmt, 8);
        row.price = sqlite3_column_double(stmt, 9);
        row.volume = sqlite3_column_int(stmt, 10);
        row.time = sqlite3_column_int(stmt, 11);
        row.seq = static_cast<uint64_t>(sqlite3_column_int64(stmt, 12));
        const double price_tick =
            sqlite3_column_type(stmt, 13) == SQLITE_NULL ? 0.0 : sqlite3_column_double(stmt, 13);

        items.push_back(td_trade_row_json(row, td_price_decimals(price_tick, row.price)));
        last.anchor = td_time_anchor(row.time);
        last.account_id = row.account_id;
        last.exchange_id = row.exchange_id;
        last.trade_id = row.trade_id;
        ++fetched;
    }
    sqlite3_finalize(stmt);
    if (step != SQLITE_ROW && step != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(db);
        SPDLOG_WARN("td trades query step failed | error=\"{}\"", sqlite3_errmsg(db));
        return td_degraded_body(td_sqlite_error_reason(code));
    }

    body["items"] = std::move(items);
    if (has_more) {
        body["next_cursor"] = td_encode_cursor(*scope, encode_trades_coord(last));
    }
    return body;
}

}  // namespace dztrader::webui
