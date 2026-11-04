#include "td_orders_query.h"

#include <dztrader/data_type.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::webui {

namespace {

/// 契约 rest §2.9.6：18:00 锚点（夜盘归入所属交易日且与日盘排序一致）。
/// 排序坐标在 SQL 与 `sort_key` 两处使用，必须同源——SQL 片段由这两个常量拼出
/// （下面的 `time_anchor_sql()`），禁止在 SQL 文本里再写字面量。
constexpr int32_t kSortAnchorOffsetSec = 21600;
constexpr int32_t kSecondsPerDay = 86400;

/// 排序坐标的 SQL 表达式（与 `td_order_sort_key` 的时间锚同源）。
std::string time_anchor_sql() {
    return std::format("((o.time + {}) % {})", kSortAnchorOffsetSec, kSecondsPerDay);
}

/// 降级原因码（契约 rest §2.9.8）。
constexpr const char* kReasonNotFound = "not_found";
constexpr const char* kReasonSchemaMismatch = "schema_mismatch";
constexpr const char* kReasonOpenFailed = "open_failed";
constexpr const char* kReasonBusy = "busy";
constexpr const char* kReasonQueryFailed = "query_failed";

/// 委托枚举 → 契约 rest §2.9.3 的 PascalCase 字符串。
/// 未知取值（DB 被手工改写等）不抛错，按零值语义输出并记 DEBUG（页面不能因为一行脏数据整页失败）。
std::string direction_text(int value) {
    switch (value) {
        case DZ_DIRECTION_LONG:
            return "Long";
        case DZ_DIRECTION_NET:
            return "Net";
        case DZ_DIRECTION_SHORT:
            return "Short";
        default:
            SPDLOG_DEBUG("unknown order direction | value={}", value);
            return "Net";
    }
}

std::string position_effect_text(int value) {
    switch (value) {
        case DZ_POSITION_EFFECT_AUTO:
            return "Auto";
        case DZ_POSITION_EFFECT_OPEN:
            return "Open";
        case DZ_POSITION_EFFECT_CLOSE:
            return "Close";
        case DZ_POSITION_EFFECT_CLOSE_TODAY:
            return "CloseToday";
        case DZ_POSITION_EFFECT_CLOSE_YESTDAY:
            return "CloseYesterday";
        default:
            SPDLOG_DEBUG("unknown order position effect | value={}", value);
            return "Auto";
    }
}

std::string price_type_text(int value) {
    switch (value) {
        case DZ_PRICE_LIMIT:
            return "Limit";
        case DZ_PRICE_MARKET:
            return "Market";
        case DZ_PRICE_STOP:
            return "Stop";
        case DZ_PRICE_FAK:
            return "Fak";
        case DZ_PRICE_FOK:
            return "Fok";
        case DZ_PRICE_RFQ:
            return "Rfq";
        default:
            SPDLOG_DEBUG("unknown order price type | value={}", value);
            return "Limit";
    }
}

std::string status_text(int value) {
    switch (value) {
        case DZ_ORDER_SUBMITTING:
            return "Submitting";
        case DZ_ORDER_NOT_TRADED:
            return "NotTraded";
        case DZ_ORDER_PART_TRADED:
            return "PartTraded";
        case DZ_ORDER_ALL_TRADED:
            return "AllTraded";
        case DZ_ORDER_CANCELLED:
            return "Cancelled";
        case DZ_ORDER_REJECTED:
            return "Rejected";
        default:
            SPDLOG_DEBUG("unknown order status | value={}", value);
            return "Submitting";
    }
}

/// 最小整数 d ∈ [0, 8] 使 `value × 10^d` 与最近整数之差 ≤ 1e-9（契约 rest §2.9.6 的
/// 浮点安全口径，不做字符串猜测）；无法确定返回 nullopt。仅用于 `price_tick`（量级小，
/// 绝对容差成立）。
std::optional<int> tick_decimals(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return std::nullopt;
    }
    double scaled = value;
    for (int decimals = 0; decimals <= 8; ++decimals) {
        const double nearest = std::round(scaled);
        if (std::abs(scaled - nearest) <= 1e-9) {
            return decimals;
        }
        scaled *= 10.0;
    }
    return std::nullopt;
}

/// 价格的最短十进制表示的小数位数（契约 rest §2.9.6 的兜底口径）。
/// 用 `std::to_chars` 的最短往返表示（如 3512.35 → "3512.35"、123456.789 → "123456.789"），
/// 而不是对 `value × 10^d` 做绝对容差比较——后者在价格量级大时会因浮点尾差误判。
std::optional<int> shortest_decimals(double value) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }
    const std::string_view text(buffer.data(), static_cast<size_t>(result.ptr - buffer.data()));
    // 计数小数点后的位数；科学计数法（如 "1e+06"）需扣除正指数。
    const auto exponent_pos = text.find_first_of("eE");
    const std::string_view mantissa = text.substr(0, exponent_pos);
    int exponent = 0;
    if (exponent_pos != std::string_view::npos) {
        const std::string_view exponent_text = text.substr(exponent_pos + 1);
        const auto parsed = std::from_chars(exponent_text.data(),
                                            exponent_text.data() + exponent_text.size(), exponent);
        if (parsed.ec != std::errc{}) {
            return std::nullopt;
        }
    }
    const auto dot = mantissa.find('.');
    int fraction_digits = 0;
    if (dot != std::string_view::npos) {
        fraction_digits = static_cast<int>(mantissa.size() - dot - 1);
    }
    const int decimals = std::max(0, fraction_digits - exponent);
    if (decimals > 8) {
        return std::nullopt;  // 契约范围 [0, 8]；超出按「仍无法确定」处理
    }
    return decimals;
}

/// 时间锚：(time + 18:00) mod 24h；对负数取模做安全归一（脏数据不得让排序崩）。
int32_t time_anchor(int32_t time) {
    int32_t normalized = time % kSecondsPerDay;
    if (normalized < 0) {
        normalized += kSecondsPerDay;
    }
    return (normalized + kSortAnchorOffsetSec) % kSecondsPerDay;
}

/// FNV-1a：游标作用域指纹（跨进程/重启稳定；不依赖 `std::hash` 的实现定义值）。
uint64_t fnv1a(std::string_view text) {
    uint64_t hash = 1469598103934665603ULL;
    for (const char ch : text) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_encode(std::string_view raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (const char ch : raw) {
        const auto byte = static_cast<unsigned char>(ch);
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

std::optional<std::string> hex_decode(std::string_view text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        return -1;
    };
    std::string out;
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int high = nibble(text[i]);
        const int low = nibble(text[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return out;
}

/// 游标内容（内部形态；hex 编码后作为外层不透明串）。
struct CursorPayload {
    uint64_t scope = 0;
    int32_t anchor = 0;
    int64_t order_id = 0;
};

std::string encode_cursor(const CursorPayload& payload) {
    return hex_encode(
        std::format("1|{:016x}|{:05d}|{:020d}", payload.scope, payload.anchor, payload.order_id));
}

std::optional<CursorPayload> decode_cursor(std::string_view text) {
    const auto raw = hex_decode(text);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    // 定宽解析：1|<16 hex>|<5 digits>|<20 digits>
    if (raw->size() != 2 + 16 + 1 + 5 + 1 + 20 || (*raw)[0] != '1' || (*raw)[1] != '|' ||
        (*raw)[18] != '|' || (*raw)[24] != '|') {
        return std::nullopt;
    }
    uint64_t scope = 0;
    const auto scope_result =
        std::from_chars(raw->data() + 2, raw->data() + 2 + 16, scope, /*base=*/16);
    if (scope_result.ec != std::errc{} || scope_result.ptr != raw->data() + 2 + 16) {
        return std::nullopt;
    }
    int32_t anchor = 0;
    const auto anchor_result =
        std::from_chars(raw->data() + 19, raw->data() + 19 + 5, anchor, /*base=*/10);
    if (anchor_result.ec != std::errc{} || anchor_result.ptr != raw->data() + 19 + 5) {
        return std::nullopt;
    }
    int64_t order_id = 0;
    const auto order_result =
        std::from_chars(raw->data() + 25, raw->data() + 25 + 20, order_id, /*base=*/10);
    if (order_result.ec != std::errc{} || order_result.ptr != raw->data() + 25 + 20) {
        return std::nullopt;
    }
    return CursorPayload{.scope = scope, .anchor = anchor, .order_id = order_id};
}

/// 游标作用域指纹：端点 + 已归一化 accounts + 交易日（契约 rest §2.9.2）。
uint64_t cursor_scope(const std::vector<std::string>& accounts, const std::string& trading_day) {
    std::string text = "GET /api/td/orders";
    for (const auto& account : accounts) {
        text += '\x1f';
        text += account;
    }
    text += '\x1e';
    text += trading_day;
    return fnv1a(text);
}

std::string trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

/// sqlite 错误是否属于等锁失败（契约 rest §2.9.8 的 busy）。
bool is_busy(int code) {
    const int primary = code > 0 ? (code & 0xFF) : code;
    return primary == SQLITE_BUSY || primary == SQLITE_LOCKED;
}

/// 打开失败时是否属于「库损坏 / 非 SQLite」（→ open_failed 而不是 not_found）。
bool is_corrupt(int code) {
    const int primary = code > 0 ? (code & 0xFF) : code;
    return primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB;
}

/// 打开只读连接并设 busy_timeout。失败时把原因映射成契约 reason 码。
struct OpenResult {
    sqlite3* db = nullptr;
    const char* reason = nullptr;  ///< nullptr = 成功
};

OpenResult open_readonly(const std::string& path, int busy_timeout_ms) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec) || ec) {
        return {.db = nullptr, .reason = kReasonNotFound};
    }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        const int code = db != nullptr ? sqlite3_extended_errcode(db) : SQLITE_CANTOPEN;
        SPDLOG_WARN("td orders db open failed | path={} error=\"{}\"", path,
                    db != nullptr ? sqlite3_errmsg(db) : "unknown");
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return {.db = nullptr, .reason = is_busy(code) ? kReasonBusy : kReasonOpenFailed};
    }
    sqlite3_busy_timeout(db, busy_timeout_ms);
    return {.db = db, .reason = nullptr};
}

nlohmann::json degraded_body(const char* reason) {
    return {{"available", false}, {"reason", reason}};
}

std::string column_text(sqlite3_stmt* stmt, int column) {
    const auto* text = sqlite3_column_text(stmt, column);
    return text != nullptr ? std::string(reinterpret_cast<const char*>(text)) : std::string{};
}

/// 空列表响应的可用性判定（契约 rest §2.9.8）：库确实可读（能开、schema 版本匹配）→
/// available:true + 空 items；否则整体降级（只有 available/reason）。
/// 「今天没数据」与「库读不了」必须可区分，否则前端会把降级显示成 0。
nlohmann::json with_availability(nlohmann::json body,
                                 const std::string& path,
                                 int busy_timeout_ms) {
    const OpenResult opened = open_readonly(path, busy_timeout_ms);
    if (opened.db == nullptr) {
        return degraded_body(opened.reason);
    }
    sqlite3_stmt* stmt = nullptr;
    int version = -1;
    int step = SQLITE_MISUSE;
    if (sqlite3_prepare_v2(opened.db, "PRAGMA user_version", -1, &stmt, nullptr) == SQLITE_OK) {
        step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            version = sqlite3_column_int(stmt, 0);
        }
    }
    const char* reason = nullptr;
    if (step != SQLITE_ROW) {
        const int code = sqlite3_extended_errcode(opened.db);
        reason = is_corrupt(code) ? kReasonOpenFailed
                                  : (is_busy(code) ? kReasonBusy : kReasonQueryFailed);
    } else if (version != kTdOrdersReadSchemaVersion) {
        reason = kReasonSchemaMismatch;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(opened.db);
    return reason == nullptr ? body : degraded_body(reason);
}

}  // namespace

nlohmann::json td_order_row_json(const TdOrderRecord& row, int price_decimals) {
    return {
        {"account_id", row.account_id},
        {"date", row.date},
        {"order_id", row.order_id},
        {"strategy_id", row.strategy_id},
        {"instrument_id", row.instrument_id},
        {"exchange_id", row.exchange_id},
        {"direction", direction_text(row.direction)},
        {"position_effect", position_effect_text(row.position_effect)},
        {"price_type", price_type_text(row.price_type)},
        {"status", status_text(row.status)},
        {"price", row.price},
        {"volume", row.volume},
        {"volume_traded", row.volume_traded},
        {"remark", row.remark},
        {"time", row.time},
        {"avg_price",
         row.volume_traded == 0 ? nlohmann::json(nullptr) : nlohmann::json(row.avg_price)},
        {"price_decimals", price_decimals},
        {"avg_price_decimals", td_avg_price_decimals(price_decimals)},
        {"seq", row.seq},
        {"sort_key", td_order_sort_key(row.time, row.order_id)},
    };
}

int td_price_decimals(double price_tick, double price) {
    if (const auto from_tick = tick_decimals(price_tick); from_tick.has_value()) {
        return *from_tick;
    }
    if (const auto from_price = shortest_decimals(price); from_price.has_value()) {
        return *from_price;
    }
    return 0;
}

int td_avg_price_decimals(int price_decimals) { return std::clamp(price_decimals + 2, 2, 6); }

std::string td_order_sort_key(int32_t time, int64_t order_id) {
    return std::format("{:05d}-{:020d}", time_anchor(time), order_id);
}

std::optional<double> td_query_price_tick(const std::string& trade_db_path,
                                          int busy_timeout_ms,
                                          const std::string& instrument_id) {
    const OpenResult opened = open_readonly(trade_db_path, busy_timeout_ms);
    if (opened.db == nullptr) {
        return std::nullopt;
    }
    sqlite3_stmt* stmt = nullptr;
    std::optional<double> tick;
    if (sqlite3_prepare_v2(opened.db, "SELECT price_tick FROM instruments WHERE instrument_id = ?",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, instrument_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            tick = sqlite3_column_double(stmt, 0);
        }
    } else {
        SPDLOG_DEBUG("td instrument tick query failed | instrument={}", instrument_id);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(opened.db);
    return tick;
}

TdOrdersQuery::TdOrdersQuery(std::function<std::string()> trade_db_path, int busy_timeout_ms)
    : trade_db_path_(std::move(trade_db_path)),
      busy_timeout_ms_(busy_timeout_ms) {}

std::optional<TdOrdersQuery::Params> TdOrdersQuery::parse(
    const std::function<std::string(std::string_view)>& param) {
    Params params;
    const std::string limit_text = trim(param("limit"));
    if (!limit_text.empty()) {
        int limit = 0;
        const auto result =
            std::from_chars(limit_text.data(), limit_text.data() + limit_text.size(), limit);
        if (result.ec != std::errc{} || result.ptr != limit_text.data() + limit_text.size() ||
            limit < 1 || limit > kTdOrdersMaxLimit) {
            return std::nullopt;
        }
        params.limit = limit;
    }
    params.cursor = trim(param("cursor"));

    // accounts：逗号分隔，去首尾空白、空项跳过、重复忽略（契约 rest §2.9.2）。
    const std::string accounts_text = param("accounts");
    size_t begin = 0;
    while (begin <= accounts_text.size()) {
        const size_t comma = accounts_text.find(',', begin);
        const size_t end = comma == std::string::npos ? accounts_text.size() : comma;
        std::string account = trim(std::string_view(accounts_text).substr(begin, end - begin));
        if (!account.empty()) {
            params.accounts.push_back(std::move(account));
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    // 归一化：排序 + 去重，使 "a,b" 与 "b,a" 属于同一游标作用域。
    std::sort(params.accounts.begin(), params.accounts.end());
    params.accounts.erase(std::unique(params.accounts.begin(), params.accounts.end()),
                          params.accounts.end());
    return params;
}

std::optional<nlohmann::json> TdOrdersQuery::execute(
    const Params& params,
    const std::vector<std::string>& online_accounts,
    const std::optional<std::string>& trading_day) const {
    // 账户范围 = 请求过滤 ∩ 在线集合（缺省 = 全部在线；未知账户忽略）。
    std::vector<std::string> scope_accounts;
    if (params.accounts.empty()) {
        scope_accounts = online_accounts;
        std::sort(scope_accounts.begin(), scope_accounts.end());
        scope_accounts.erase(std::unique(scope_accounts.begin(), scope_accounts.end()),
                             scope_accounts.end());
    } else {
        for (const auto& account : params.accounts) {
            if (std::find(online_accounts.begin(), online_accounts.end(), account) !=
                online_accounts.end()) {
                scope_accounts.push_back(account);
            }
        }
    }

    nlohmann::json body = {{"available", true},
                           {"trading_day", trading_day.has_value() ? nlohmann::json(*trading_day)
                                                                   : nlohmann::json(nullptr)},
                           {"online_accounts", online_accounts},
                           {"items", nlohmann::json::array()},
                           {"next_cursor", nullptr}};

    // 游标作用域（契约 rest §2.9.2）：端点 + accounts + 当前交易日。任一项变化都必须从
    // 首页重拉。校验在「空列表」早退之前——作用域不符/非法游标一律 400，不静默返回空页。
    // 注意 `online_accounts` 不参与作用域（它是口径输入，不是查询参数）：在线集合变化由
    // `data_changed{scope:"td"}` 通知前端丢弃游标（契约 webui-ws §2.3/§5）。
    const std::optional<uint64_t> scope =
        trading_day.has_value()
            ? std::optional<uint64_t>(cursor_scope(params.accounts, *trading_day))
            : std::nullopt;
    std::optional<CursorPayload> cursor;
    if (!params.cursor.empty()) {
        if (!scope.has_value()) {
            return std::nullopt;  // 无当前交易日 = 无有效游标作用域
        }
        cursor = decode_cursor(params.cursor);
        if (!cursor.has_value() || cursor->scope != *scope) {
            return std::nullopt;
        }
    }

    // 无在线账户 / 无当前交易日：空列表语义（不是错误）。但仍需探测交易库可用性——
    // 「库不可读」与「今天没数据」是不同事实，混起来前端会显示 0 冒充正常数据
    // （契约 rest §2.9.8 + webui-ws §5 第 13 条）。
    if (!scope.has_value() || scope_accounts.empty()) {
        return with_availability(std::move(body), trade_db_path_(), busy_timeout_ms_);
    }

    const OpenResult opened = open_readonly(trade_db_path_(), busy_timeout_ms_);
    if (opened.db == nullptr) {
        return degraded_body(opened.reason);
    }
    sqlite3* db = opened.db;
    const auto close_and = [db](const nlohmann::json& result) {
        sqlite3_close(db);
        return result;
    };

    // schema 版本校验（契约 rest §2.9.8：schema_mismatch）。
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr) != SQLITE_OK) {
            const int code = sqlite3_extended_errcode(db);
            SPDLOG_WARN("td orders schema probe failed | error=\"{}\"", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return close_and(degraded_body(
                is_corrupt(code) ? kReasonOpenFailed
                                 : (is_busy(code) ? kReasonBusy : kReasonQueryFailed)));
        }
        int version = -1;
        const int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            version = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        if (step != SQLITE_ROW) {
            const int code = sqlite3_extended_errcode(db);
            SPDLOG_WARN("td orders schema probe step failed | error=\"{}\"", sqlite3_errmsg(db));
            // 非 SQLite 文件 / 页损坏：`sqlite3_open` 是惰性的，失败只在首次读页暴露，
            // 契约 rest §2.9.8 把这类归 open_failed（不是 query_failed）。
            const char* reason = is_corrupt(code)
                                     ? kReasonOpenFailed
                                     : (is_busy(code) ? kReasonBusy : kReasonQueryFailed);
            return close_and(degraded_body(reason));
        }
        if (version != kTdOrdersReadSchemaVersion) {
            SPDLOG_WARN("td orders schema mismatch | found={} expected={}", version,
                        kTdOrdersReadSchemaVersion);
            return close_and(degraded_body(kReasonSchemaMismatch));
        }
    }

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
    if (cursor.has_value()) {
        // 游标坐标 = 与 sort_key 同源的 (时间锚, order_id) 降序，下一页取「更旧」的行。
        // 禁用 OFFSET（契约 rest §2.9.2），坐标条件走 idx_orders_sort。
        const std::string anchor = time_anchor_sql();
        sql += std::format(" AND ({} < ? OR ({} = ? AND o.order_id < ?))", anchor, anchor);
    }
    sql += std::format(" ORDER BY {} DESC, o.order_id DESC LIMIT ?", time_anchor_sql());

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        const int code = sqlite3_extended_errcode(db);
        SPDLOG_WARN("td orders query prepare failed | error=\"{}\"", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return close_and(degraded_body(is_busy(code) ? kReasonBusy : kReasonQueryFailed));
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
    int64_t last_order_id = 0;
    int32_t last_anchor = 0;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (fetched == params.limit) {
            has_more = true;
            break;
        }
        TdOrderRecord row;
        row.account_id = column_text(stmt, 0);
        row.date = column_text(stmt, 1);
        row.order_id = sqlite3_column_int64(stmt, 2);
        row.strategy_id = column_text(stmt, 3);
        row.instrument_id = column_text(stmt, 4);
        row.exchange_id = column_text(stmt, 5);
        row.direction = sqlite3_column_int(stmt, 6);
        row.position_effect = sqlite3_column_int(stmt, 7);
        row.price_type = sqlite3_column_int(stmt, 8);
        row.status = sqlite3_column_int(stmt, 9);
        row.price = sqlite3_column_double(stmt, 10);
        row.volume = sqlite3_column_int(stmt, 11);
        row.volume_traded = sqlite3_column_int(stmt, 12);
        row.remark = column_text(stmt, 13);
        row.time = sqlite3_column_int(stmt, 14);
        row.avg_price = sqlite3_column_double(stmt, 15);
        row.seq = static_cast<uint64_t>(sqlite3_column_int64(stmt, 16));
        const double price_tick =
            sqlite3_column_type(stmt, 17) == SQLITE_NULL ? 0.0 : sqlite3_column_double(stmt, 17);

        items.push_back(td_order_row_json(row, td_price_decimals(price_tick, row.price)));
        last_order_id = row.order_id;
        last_anchor = time_anchor(row.time);
        ++fetched;
    }
    sqlite3_finalize(stmt);
    if (step != SQLITE_ROW && step != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(db);
        SPDLOG_WARN("td orders query step failed | error=\"{}\"", sqlite3_errmsg(db));
        return close_and(degraded_body(is_busy(code) ? kReasonBusy : kReasonQueryFailed));
    }

    body["items"] = std::move(items);
    if (has_more) {
        body["next_cursor"] = encode_cursor(
            CursorPayload{.scope = *scope, .anchor = last_anchor, .order_id = last_order_id});
    }
    return close_and(body);
}

}  // namespace dztrader::webui
