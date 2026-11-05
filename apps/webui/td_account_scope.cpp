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
                                                bool* ok) {
    if (ok != nullptr) {
        *ok = true;
    }
    // 起点 = 在线账户（契约 rest §2.9.1：在线 ∪ 四表；在线账户即使库里还没有任何行
    // 也要出现在全集里——刚登录、尚未落首条记录的账户不该从 UI 里消失）。
    std::set<std::string> accounts;
    for (const auto& account : online_accounts) {
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

std::optional<std::string> td_account_anchor_day(sqlite3* db,
                                                 const std::string& account_id,
                                                 bool* ok) {
    std::optional<std::string> anchor;
    if (ok != nullptr) {
        *ok = true;
    }
    if (db == nullptr) {
        if (ok != nullptr) {
            *ok = false;
        }
        return anchor;
    }
    for (const char* table : kSnapshotTables) {
        const std::string sql =
            std::format("SELECT MAX(date) FROM \"{}\" WHERE account_id = ?", table);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            SPDLOG_WARN("td account anchor prepare failed | table={} error=\"{}\"", table,
                        sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            if (ok != nullptr) {
                *ok = false;
            }
            return anchor;
        }
        sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
        const int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            const auto* text = sqlite3_column_text(stmt, 0);
            if (text != nullptr) {
                const std::string day(reinterpret_cast<const char*>(text));
                // MAX(date) 是 TEXT 列上的字典序最大值 = 交易日最大值（YYYYMMDD 定宽）。
                if (!day.empty() && (!anchor.has_value() || day > *anchor)) {
                    anchor = day;
                }
            }
        } else if (step != SQLITE_ROW && step != SQLITE_DONE) {
            SPDLOG_WARN("td account anchor step failed | table={} error=\"{}\"", table,
                        sqlite3_errmsg(db));
            if (ok != nullptr) {
                *ok = false;
            }
            sqlite3_finalize(stmt);
            return anchor;
        }
        sqlite3_finalize(stmt);
    }
    return anchor;
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

}  // namespace dztrader::webui
