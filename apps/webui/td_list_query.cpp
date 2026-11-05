#include "td_list_query.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <format>
#include <string>
#include <utility>

namespace dztrader::webui {

namespace {

/// 契约 rest §2.9.6：18:00 锚点（夜盘归入所属交易日且与日盘排序一致）。
/// 排序坐标在 SQL 与 `sort_key` 两处使用，必须同源——SQL 片段由这两个常量拼出
/// （`td_time_anchor_sql`），禁止在 SQL 文本里再写字面量。
constexpr int32_t kSortAnchorOffsetSec = 21600;
constexpr int32_t kSecondsPerDay = 86400;

/// FNV-1a：游标作用域指纹（跨进程/重启稳定；不依赖 `std::hash` 的实现定义值）。
uint64_t fnv1a(std::string_view text) {
    uint64_t hash = 1469598103934665603ULL;
    for (const char ch : text) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_encode_impl(std::string_view raw) {
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

std::optional<std::string> hex_decode_impl(std::string_view text) {
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

}  // namespace

std::string td_hex_encode(std::string_view raw) { return hex_encode_impl(raw); }

std::optional<std::string> td_hex_decode(std::string_view text) { return hex_decode_impl(text); }

std::optional<TdListParams> td_parse_list_params(
    const std::function<std::string(std::string_view)>& param) {
    TdListParams params;
    const std::string limit_text = trim(param("limit"));
    if (!limit_text.empty()) {
        int limit = 0;
        const auto result =
            std::from_chars(limit_text.data(), limit_text.data() + limit_text.size(), limit);
        if (result.ec != std::errc{} || result.ptr != limit_text.data() + limit_text.size() ||
            limit < 1 || limit > kTdListMaxLimit) {
            return std::nullopt;
        }
        params.limit = limit;
    }
    params.cursor = trim(param("cursor"));
    params.accounts = td_parse_accounts(param("accounts"));
    return params;
}

std::vector<std::string> td_parse_accounts(std::string_view text) {
    // accounts：逗号分隔，去首尾空白、空项跳过、重复忽略（契约 rest §2.9.2）。
    std::vector<std::string> accounts;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t comma = text.find(',', begin);
        const size_t end = comma == std::string_view::npos ? text.size() : comma;
        std::string account = trim(text.substr(begin, end - begin));
        if (!account.empty()) {
            accounts.push_back(std::move(account));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    // 归一化：排序 + 去重，使 "a,b" 与 "b,a" 属于同一游标作用域（资金端点不分页，
    // 归一化让它与列表端点的账户过滤语义保持一致）。
    std::sort(accounts.begin(), accounts.end());
    accounts.erase(std::unique(accounts.begin(), accounts.end()), accounts.end());
    return accounts;
}

std::vector<std::string> td_scope_accounts(const std::vector<std::string>& requested,
                                           const std::vector<std::string>& online_accounts) {
    // 账户范围 = 请求过滤 ∩ 在线集合（缺省 = 全部在线；未知账户忽略）。
    std::vector<std::string> scope;
    if (requested.empty()) {
        scope = online_accounts;
        std::sort(scope.begin(), scope.end());
        scope.erase(std::unique(scope.begin(), scope.end()), scope.end());
        return scope;
    }
    for (const auto& account : requested) {
        if (std::find(online_accounts.begin(), online_accounts.end(), account) !=
            online_accounts.end()) {
            scope.push_back(account);
        }
    }
    return scope;
}

std::string td_encode_cursor(uint64_t scope, std::string_view coord) {
    return hex_encode_impl(std::format("1|{:016x}|{}", scope, coord));
}

std::optional<std::string> td_decode_cursor(std::string_view text, uint64_t scope) {
    const auto raw = hex_decode_impl(text);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    // 定宽前缀 "1|" + 16 hex + "|"，其余为端点自定义坐标文本。
    constexpr size_t kPrefixSize = 2 + 16 + 1;
    if (raw->size() < kPrefixSize || (*raw)[0] != '1' || (*raw)[1] != '|' || (*raw)[18] != '|') {
        return std::nullopt;
    }
    uint64_t decoded_scope = 0;
    const auto result =
        std::from_chars(raw->data() + 2, raw->data() + 2 + 16, decoded_scope, /*base=*/16);
    if (result.ec != std::errc{} || result.ptr != raw->data() + 2 + 16) {
        return std::nullopt;
    }
    // 契约 rest §2.9.2：游标与查询作用域绑定，作用域变化必须从首页开始。
    if (decoded_scope != scope) {
        return std::nullopt;
    }
    return raw->substr(kPrefixSize);
}

uint64_t td_cursor_scope(std::string_view endpoint,
                         const std::vector<std::string>& accounts,
                         std::string_view trading_day) {
    std::string text(endpoint);
    for (const auto& account : accounts) {
        text += '\x1f';
        text += account;
    }
    text += '\x1e';
    text += trading_day;
    return fnv1a(text);
}

int32_t td_time_anchor(int32_t time) {
    // 时间锚：(time + 18:00) mod 24h；对负数取模做安全归一（脏数据不得让排序崩）。
    int32_t normalized = time % kSecondsPerDay;
    if (normalized < 0) {
        normalized += kSecondsPerDay;
    }
    return (normalized + kSortAnchorOffsetSec) % kSecondsPerDay;
}

std::string td_time_anchor_sql(std::string_view alias) {
    return std::format("(({}.time + {}) % {})", alias, kSortAnchorOffsetSec, kSecondsPerDay);
}

nlohmann::json td_list_envelope(const std::optional<std::string>& trading_day,
                                const std::vector<std::string>& online_accounts) {
    return {{"available", true},
            {"trading_day",
             trading_day.has_value() ? nlohmann::json(*trading_day) : nlohmann::json(nullptr)},
            {"online_accounts", online_accounts},
            {"items", nlohmann::json::array()},
            {"next_cursor", nullptr}};
}

nlohmann::json td_list_with_availability(nlohmann::json body,
                                         const std::string& trade_db_path,
                                         int busy_timeout_ms) {
    const TdReadConnection conn = TdReadConnection::open(trade_db_path, busy_timeout_ms);
    return conn.connected() ? std::move(body) : td_degraded_body(conn.reason());
}

}  // namespace dztrader::webui
