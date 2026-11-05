#include "td_orders_query.h"

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

/// 委托列表的游标作用域端点标识（参与 `td_cursor_scope` 指纹，契约 rest §2.9.2）。
constexpr const char* kOrdersEndpoint = "GET /api/td/orders";

/// 委托游标坐标：与 sort_key 同源的 (时间锚, order_id)（契约 rest §2.9.2）。
struct OrdersCursor {
    int32_t anchor = 0;
    int64_t order_id = 0;
};

std::string encode_orders_coord(const OrdersCursor& cursor) {
    return std::format("{:05d}|{:020d}", cursor.anchor, cursor.order_id);
}

std::optional<OrdersCursor> decode_orders_coord(std::string_view text) {
    // 定宽：5 digits + '|' + 20 digits
    if (text.size() != 5 + 1 + 20 || text[5] != '|') {
        return std::nullopt;
    }
    int32_t anchor = 0;
    const auto anchor_result = std::from_chars(text.data(), text.data() + 5, anchor);
    if (anchor_result.ec != std::errc{} || anchor_result.ptr != text.data() + 5) {
        return std::nullopt;
    }
    int64_t order_id = 0;
    const auto order_result = std::from_chars(text.data() + 6, text.data() + 6 + 20, order_id);
    if (order_result.ec != std::errc{} || order_result.ptr != text.data() + 6 + 20) {
        return std::nullopt;
    }
    return OrdersCursor{.anchor = anchor, .order_id = order_id};
}

}  // namespace

std::optional<double> td_query_price_tick(const std::string& trade_db_path,
                                          int busy_timeout_ms,
                                          const std::string& instrument_id) {
    const TdReadConnection conn = TdReadConnection::open(trade_db_path, busy_timeout_ms);
    if (!conn.connected()) {
        return std::nullopt;
    }
    sqlite3_stmt* stmt = nullptr;
    std::optional<double> tick;
    if (sqlite3_prepare_v2(conn.handle(),
                           "SELECT price_tick FROM instruments WHERE instrument_id = ?", -1, &stmt,
                           nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, instrument_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            tick = sqlite3_column_double(stmt, 0);
        }
    } else {
        SPDLOG_DEBUG("td instrument tick query failed | instrument={}", instrument_id);
    }
    sqlite3_finalize(stmt);
    return tick;
}

TdOrdersQuery::TdOrdersQuery(std::function<std::string()> trade_db_path, int busy_timeout_ms)
    : trade_db_path_(std::move(trade_db_path)),
      busy_timeout_ms_(busy_timeout_ms) {}

std::optional<TdOrdersQuery::Params> TdOrdersQuery::parse(
    const std::function<std::string(std::string_view)>& param) {
    // 参数校验是委托/成交共用的同一份实现（`td_list_query.h`）。
    return td_parse_list_params(param);
}

std::optional<nlohmann::json> TdOrdersQuery::execute(
    const Params& params,
    const std::vector<std::string>& online_accounts,
    const std::optional<std::string>& trading_day) const {
    // 账户范围 = 请求过滤 ∩ 在线集合（缺省 = 全部在线；未知账户忽略）。
    std::vector<std::string> scope_accounts = td_scope_accounts(params.accounts, online_accounts);

    nlohmann::json body = td_list_envelope(trading_day, online_accounts);

    // 游标作用域（契约 rest §2.9.2）：端点 + accounts + 当前交易日。任一项变化都必须从
    // 首页重拉。校验在「空列表」早退之前——作用域不符/非法游标一律 400，不静默返回空页。
    // 注意 `online_accounts` 不参与作用域（它是口径输入，不是查询参数）：在线集合变化由
    // `data_changed{scope:"td"}` 通知前端丢弃游标（契约 webui-ws §2.3/§5）。
    const std::optional<uint64_t> scope = trading_day.has_value()
                                              ? std::optional<uint64_t>(td_cursor_scope(
                                                    kOrdersEndpoint, params.accounts, *trading_day))
                                              : std::nullopt;
    std::optional<OrdersCursor> cursor;
    if (!params.cursor.empty()) {
        if (!scope.has_value()) {
            return std::nullopt;  // 无当前交易日 = 无有效游标作用域
        }
        const std::optional<std::string> coord = td_decode_cursor(params.cursor, *scope);
        if (!coord.has_value()) {
            return std::nullopt;
        }
        cursor = decode_orders_coord(*coord);
        if (!cursor.has_value()) {
            return std::nullopt;
        }
    }

    // 无在线账户 / 无当前交易日：空列表语义（不是错误）。但仍需探测交易库可用性——
    // 「库不可读」与「今天没数据」是不同事实，混起来前端会显示 0 冒充正常数据
    // （契约 rest §2.9.8 + webui-ws §5 第 13 条）。
    if (!scope.has_value() || scope_accounts.empty()) {
        return td_list_with_availability(std::move(body), trade_db_path_(), busy_timeout_ms_);
    }

    // 只读连接 + schema 版本校验（契约 rest §2.9.8）：抽取在 td_read_db，
    // 与统计重算共用同一份实现（reason 码是对外协议，禁止两份）。
    TdReadConnection conn = TdReadConnection::open(trade_db_path_(), busy_timeout_ms_);
    if (!conn.connected()) {
        return td_degraded_body(conn.reason());
    }
    sqlite3* db = conn.handle();

    // 主查询：LEFT JOIN instruments 取 price_tick（查不到走价格小数位兜底）。
    std::string sql =
        "SELECT o.account_id, o.date, o.order_id, o.strategy_id, o.instrument_id,"
        " o.exchange_id, o.direction, o.position_effect, o.price_type, o.status,"
        " o.price, o.volume, o.volume_traded, o.remark, o.time, o.avg_price, o.seq,"
        " i.price_tick"
        " FROM orders o LEFT JOIN instruments i ON i.instrument_id = o.instrument_id"
        " WHERE o.date = ?1 AND o.account_id IN (";
    for (size_t i = 0; i < scope_accounts.size(); ++i) {
        sql += i == 0 ? "?" : ",?";
    }
    sql += ")";
    const std::string anchor_sql = td_time_anchor_sql("o");
    if (cursor.has_value()) {
        // 游标坐标 = 与 sort_key 同源的 (时间锚, order_id) 降序，下一页取「更旧」的行。
        // 禁用 OFFSET（契约 rest §2.9.2），坐标条件走 idx_orders_sort。
        sql += std::format(" AND ({} < ? OR ({} = ? AND o.order_id < ?))", anchor_sql, anchor_sql);
    }
    sql += std::format(" ORDER BY {} DESC, o.order_id DESC LIMIT ?", anchor_sql);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        const int code = sqlite3_extended_errcode(db);
        SPDLOG_WARN("td orders query prepare failed | error=\"{}\"", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return td_degraded_body(td_sqlite_error_reason(code));
    }

    int bind_index = 1;
    sqlite3_bind_text(stmt, bind_index++, trading_day->c_str(), -1, SQLITE_TRANSIENT);
    for (const auto& account : scope_accounts) {
        sqlite3_bind_text(stmt, bind_index++, account.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (cursor.has_value()) {
        sqlite3_bind_int(stmt, bind_index++, cursor->anchor);
        sqlite3_bind_int(stmt, bind_index++, cursor->anchor);
        sqlite3_bind_int64(stmt, bind_index++, cursor->order_id);
    }
    // 多取一行判断是否还有下一页。
    sqlite3_bind_int(stmt, bind_index, params.limit + 1);

    nlohmann::json items = nlohmann::json::array();
    int fetched = 0;
    bool has_more = false;
    OrdersCursor last{};
    int step = SQLITE_OK;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (fetched == params.limit) {
            has_more = true;
            break;
        }
        TdOrderRecord row;
        row.account_id = td_column_text(stmt, 0);
        row.date = td_column_text(stmt, 1);
        row.order_id = sqlite3_column_int64(stmt, 2);
        row.strategy_id = td_column_text(stmt, 3);
        row.instrument_id = td_column_text(stmt, 4);
        row.exchange_id = td_column_text(stmt, 5);
        row.direction = sqlite3_column_int(stmt, 6);
        row.position_effect = sqlite3_column_int(stmt, 7);
        row.price_type = sqlite3_column_int(stmt, 8);
        row.status = sqlite3_column_int(stmt, 9);
        row.price = sqlite3_column_double(stmt, 10);
        row.volume = sqlite3_column_int(stmt, 11);
        row.volume_traded = sqlite3_column_int(stmt, 12);
        row.remark = td_column_text(stmt, 13);
        row.time = sqlite3_column_int(stmt, 14);
        row.avg_price = sqlite3_column_double(stmt, 15);
        row.seq = static_cast<uint64_t>(sqlite3_column_int64(stmt, 16));
        const double price_tick =
            sqlite3_column_type(stmt, 17) == SQLITE_NULL ? 0.0 : sqlite3_column_double(stmt, 17);

        items.push_back(td_order_row_json(row, td_price_decimals(price_tick, row.price)));
        last.order_id = row.order_id;
        last.anchor = td_time_anchor(row.time);
        ++fetched;
    }
    sqlite3_finalize(stmt);
    if (step != SQLITE_ROW && step != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(db);
        SPDLOG_WARN("td orders query step failed | error=\"{}\"", sqlite3_errmsg(db));
        return td_degraded_body(td_sqlite_error_reason(code));
    }

    body["items"] = std::move(items);
    if (has_more) {
        body["next_cursor"] = td_encode_cursor(*scope, encode_orders_coord(last));
    }
    return body;
}

}  // namespace dztrader::webui
