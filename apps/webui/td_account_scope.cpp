#include "td_account_scope.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <algorithm>
#include <format>
#include <set>
#include <string>

namespace dztrader::webui {

namespace {

/// 账户全集来源：四表（契约 rest §2.9.1）。追加流表也要算——删户后历史数据仍要能被看到。
constexpr const char* kAccountTables[] = {"orders", "trades", "positions", "trading_accounts"};

/// 锚点日来源：只有两张快照表（ADR 0046）。
constexpr const char* kSnapshotTables[] = {"positions", "trading_accounts"};

/// `IN (?,?,…)` 占位符串（空集合不可传给 SQL）。
std::string placeholders(size_t count) {
    std::string text;
    text.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        text += i == 0 ? "?" : ",?";
    }
    return text;
}

}  // namespace

std::vector<std::string> td_registered_accounts(sqlite3* db,
                                                const std::vector<std::string>& online_accounts,
                                                const std::vector<std::string>& registry_accounts,
                                                bool* ok) {
    if (ok != nullptr) {
        *ok = true;
    }
    // 起点 = 在线账户 ∪ 注册表账户（契约 rest §2.9.1：在线 ∪ 注册表 ∪ 四表）。
    // 在线账户即使库里还没有任何行也要出现在全集里——刚登录、尚未落首条记录的账户不该从
    // UI 里消失；注册表账户即使网关停机、库里也还没行同样要出现（第三源的全部意义）。
    std::set<std::string> accounts;
    for (const auto& account : online_accounts) {
        if (!account.empty()) {
            accounts.insert(account);
        }
    }
    for (const auto& account : registry_accounts) {
        if (!account.empty()) {
            accounts.insert(account);
        }
    }
    if (db == nullptr) {
        if (ok != nullptr) {
            *ok = false;
        }
        return {accounts.begin(), accounts.end()};
    }
    for (const char* table : kAccountTables) {
        const std::string sql = std::format("SELECT DISTINCT account_id FROM \"{}\"", table);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            SPDLOG_WARN("td account scope prepare failed | table={} error=\"{}\"", table,
                        sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            if (ok != nullptr) {
                *ok = false;
            }
            continue;
        }
        int step = SQLITE_OK;
        while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(stmt, 0);
            if (text != nullptr) {
                const std::string account(reinterpret_cast<const char*>(text));
                if (!account.empty()) {
                    accounts.insert(account);
                }
            }
        }
        if (step != SQLITE_DONE) {
            SPDLOG_WARN("td account scope step failed | table={} error=\"{}\"", table,
                        sqlite3_errmsg(db));
            if (ok != nullptr) {
                *ok = false;
            }
        }
        sqlite3_finalize(stmt);
    }
    return {accounts.begin(), accounts.end()};
}

std::map<std::string, std::string> td_account_anchor_days(
    sqlite3* db, const std::vector<std::string>& account_ids, bool* ok) {
    std::map<std::string, std::string> anchors;
    if (ok != nullptr) {
        *ok = true;
    }
    if (db == nullptr || account_ids.empty()) {
        if (db == nullptr && ok != nullptr) {
            *ok = false;
        }
        return anchors;
    }
    for (const char* table : kSnapshotTables) {
        const std::string sql = std::format(
            "SELECT account_id, MAX(date) FROM \"{}\" WHERE account_id IN ({}) GROUP BY account_id",
            table, placeholders(account_ids.size()));
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            SPDLOG_WARN("td account anchors prepare failed | table={} error=\"{}\"", table,
                        sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            if (ok != nullptr) {
                *ok = false;
            }
            return anchors;
        }
        int index = 1;
        for (const auto& account : account_ids) {
            sqlite3_bind_text(stmt, index++, account.c_str(), -1, SQLITE_TRANSIENT);
        }
        int step = SQLITE_OK;
        while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
            const auto* account_text = sqlite3_column_text(stmt, 0);
            if (account_text == nullptr || sqlite3_column_type(stmt, 1) == SQLITE_NULL) {
                continue;
            }
            const auto* day_text = sqlite3_column_text(stmt, 1);
            if (day_text == nullptr) {
                continue;
            }
            const std::string account(reinterpret_cast<const char*>(account_text));
            const std::string day(reinterpret_cast<const char*>(day_text));
            auto it = anchors.find(account);
            if (it == anchors.end()) {
                anchors.emplace(account, day);
            } else if (day > it->second) {
                it->second = day;
            }
        }
        if (step != SQLITE_DONE) {
            SPDLOG_WARN("td account anchors step failed | table={} error=\"{}\"", table,
                        sqlite3_errmsg(db));
            if (ok != nullptr) {
                *ok = false;
            }
            sqlite3_finalize(stmt);
            return anchors;
        }
        sqlite3_finalize(stmt);
    }
    return anchors;
}

TdAnchorPairs td_anchor_pairs(const std::vector<std::string>& accounts,
                              const std::map<std::string, std::string>& anchors,
                              std::string_view column_prefix) {
    TdAnchorPairs result;
    for (const auto& account : accounts) {
        const auto it = anchors.find(account);
        if (it == anchors.end()) {
            continue;  // 该账户无任何快照行 → 无锚点日 → 不可能有当日行
        }
        result.sql += result.pairs.empty() ? "(" : " OR (";
        result.sql += std::string(column_prefix) + "account_id = ? AND " +
                      std::string(column_prefix) + "date = ?)";
        result.pairs.push_back({.account_id = account, .date = it->second});
    }
    return result;
}

int td_bind_anchor_pairs(sqlite3_stmt* stmt, const TdAnchorPairs& pairs, int first_index) {
    int index = first_index;
    for (const auto& pair : pairs.pairs) {
        sqlite3_bind_text(stmt, index++, pair.account_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, index++, pair.date.c_str(), -1, SQLITE_TRANSIENT);
    }
    return index;
}

}  // namespace dztrader::webui
