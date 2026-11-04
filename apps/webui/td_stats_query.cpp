#include "td_stats_query.h"

#include <dztrader/data_type.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::webui {

namespace {

/// 未成交状态在 SQL 里的取值列表（契约 rest §2.9.5）——与 `td_status_unfilled` 同源：
/// 这里只列枚举量，判定仍只有 `td_status_unfilled` 一份实现。
constexpr int kUnfilledStatuses[] = {DZ_ORDER_SUBMITTING, DZ_ORDER_NOT_TRADED,
                                     DZ_ORDER_PART_TRADED};

/// 单只读事务内的查询一律走本助手：准备失败 / 步进失败都记 WARN 并返回 false，
/// 调用方统一降级为「保持旧计数器」（契约 rest §2.9.5：库短暂不可读不降级统计）。
bool prepare(sqlite3* db, const std::string& sql, sqlite3_stmt** stmt) {
    *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, stmt, nullptr) != SQLITE_OK) {
        SPDLOG_WARN("td stats query prepare failed | error=\"{}\"", sqlite3_errmsg(db));
        sqlite3_finalize(*stmt);
        *stmt = nullptr;
        return false;
    }
    return true;
}

/// `?, ?, ...`（n ≥ 1；调用方保证账户集合非空）。
std::string placeholders(size_t count) {
    std::string sql;
    for (size_t i = 0; i < count; ++i) {
        sql += i == 0 ? "?" : ",?";
    }
    return sql;
}

/// 绑定成交日（TEXT affinity 列，字符串 "YYYYMMDD"）。
void bind_day(sqlite3_stmt* stmt, int index, const std::string& trading_day) {
    sqlite3_bind_text(stmt, index, trading_day.c_str(), -1, SQLITE_TRANSIENT);
}

/// 绑定账户集合（从 1 起始的连续索引段）。
void bind_accounts(sqlite3_stmt* stmt, int first_index, const std::vector<std::string>& accounts) {
    int index = first_index;
    for (const std::string& account : accounts) {
        sqlite3_bind_text(stmt, index++, account.c_str(), -1, SQLITE_TRANSIENT);
    }
}

/// 结束语句并检查最终 step 结果；SQLITE_ROW/SQLITE_DONE 之外的都算查询失败。
bool finish(sqlite3* db, sqlite3_stmt* stmt, int step, const char* what) {
    sqlite3_finalize(stmt);
    if (step == SQLITE_ROW || step == SQLITE_DONE) {
        return true;
    }
    SPDLOG_WARN("td stats query step failed | query={} error=\"{}\"", what, sqlite3_errmsg(db));
    return false;
}

}  // namespace

TdStatsQuery::TdStatsQuery(std::function<std::string()> trade_db_path, int busy_timeout_ms)
    : trade_db_path_(std::move(trade_db_path)),
      busy_timeout_ms_(busy_timeout_ms) {}

bool TdStatsQuery::query_values(sqlite3* db,
                                const std::string& trading_day,
                                const std::vector<std::string>& accounts,
                                TdStatsValues& values) const {
    // 契约 rest §2.9.5 口径（design §2.3 照抄）：COUNT/SUM 全量 + 未成交笔数 + 未成交剩余量。
    // 占位符顺序 = SQL 文本出现顺序：先 SELECT 里的 6 个 status，再 WHERE 的 date 与 accounts。
    const std::string sql =
        "SELECT COUNT(*), COALESCE(SUM(volume), 0),"
        " COALESCE(SUM(CASE WHEN status IN (?, ?, ?) THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN status IN (?, ?, ?) THEN volume - volume_traded ELSE 0 END), 0)"
        " FROM orders WHERE date = ? AND account_id IN (" +
        placeholders(accounts.size()) + ")";
    sqlite3_stmt* stmt = nullptr;
    if (!prepare(db, sql, &stmt)) {
        return false;
    }
    int index = 1;
    for (const int status : kUnfilledStatuses) {
        sqlite3_bind_int(stmt, index++, status);
    }
    for (const int status : kUnfilledStatuses) {
        sqlite3_bind_int(stmt, index++, status);
    }
    bind_day(stmt, index++, trading_day);
    bind_accounts(stmt, index, accounts);

    const int step = sqlite3_step(stmt);
    if (step != SQLITE_ROW) {
        return finish(db, stmt, step, "values");
    }
    values.total_orders = sqlite3_column_int64(stmt, 0);
    values.total_volume = sqlite3_column_int64(stmt, 1);
    values.unfilled_orders = sqlite3_column_int64(stmt, 2);
    values.unfilled_volume = sqlite3_column_int64(stmt, 3);
    return finish(db, stmt, step, "values");
}

bool TdStatsQuery::query_active(sqlite3* db,
                                const std::string& trading_day,
                                const std::vector<std::string>& accounts,
                                std::vector<TdStatsActiveOrder>& active) const {
    // 活动（未成交）委托种子：规模 = 当日活跃挂单数（有界，ADR 0043 决策 1）。
    const std::string sql =
        "SELECT account_id, order_id, status, volume, volume_traded FROM orders"
        " WHERE date = ? AND account_id IN (" +
        placeholders(accounts.size()) + ") AND status IN (?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (!prepare(db, sql, &stmt)) {
        return false;
    }
    int index = 1;
    bind_day(stmt, index++, trading_day);
    bind_accounts(stmt, index, accounts);
    index += static_cast<int>(accounts.size());
    for (const int status : kUnfilledStatuses) {
        sqlite3_bind_int(stmt, index++, status);
    }

    int step = SQLITE_OK;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        TdStatsActiveOrder order;
        const auto* account_text = sqlite3_column_text(stmt, 0);
        order.account_id = account_text != nullptr
                               ? std::string(reinterpret_cast<const char*>(account_text))
                               : std::string{};
        order.order_id = sqlite3_column_int64(stmt, 1);
        order.status = sqlite3_column_int(stmt, 2);
        order.volume = sqlite3_column_int(stmt, 3);
        order.volume_traded = sqlite3_column_int(stmt, 4);
        active.push_back(std::move(order));
    }
    return finish(db, stmt, step, "active");
}

bool TdStatsQuery::query_watermarks(sqlite3* db,
                                    const std::vector<std::string>& accounts,
                                    std::map<std::string, uint64_t>& watermarks) const {
    // 契约 td-data-sync §1/§5：水位 = 该账户四表 MAX(seq)，**不带 date 过滤**（账户级
    // 跨日累积，与 SDK ingest 一致）。四表共享取号器 → 取各表 GROUP BY 结果的最大值。
    constexpr const char* kTables[] = {"orders", "trades", "positions", "trading_accounts"};
    for (const char* table : kTables) {
        const std::string sql = "SELECT account_id, MAX(seq) FROM " + std::string(table) +
                                " WHERE account_id IN (" + placeholders(accounts.size()) +
                                ") GROUP BY account_id";
        sqlite3_stmt* stmt = nullptr;
        if (!prepare(db, sql, &stmt)) {
            return false;
        }
        bind_accounts(stmt, 1, accounts);
        int step = SQLITE_OK;
        while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
            const auto* account_text = sqlite3_column_text(stmt, 0);
            if (account_text == nullptr) {
                continue;
            }
            const std::string account(reinterpret_cast<const char*>(account_text));
            // MAX(seq) 为空（该账户该表无行）时 SQL 给 NULL，跳过。
            if (sqlite3_column_type(stmt, 1) == SQLITE_NULL) {
                continue;
            }
            const auto seq = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
            auto it = watermarks.find(account);
            if (it == watermarks.end()) {
                watermarks.emplace(account, seq);
            } else if (seq > it->second) {
                it->second = seq;
            }
        }
        if (!finish(db, stmt, step, table)) {
            return false;
        }
    }
    return true;
}

std::optional<TdStatsSnapshot> TdStatsQuery::recompute(
    const std::vector<std::string>& online_accounts,
    const std::optional<std::string>& trading_day) const {
    TdStatsSnapshot snapshot;
    // 无在线账户 / 无当前交易日：契约 rest §2.9.5 的口径为空，不读库（空集合 = 0 值）。
    if (online_accounts.empty() || !trading_day.has_value()) {
        return snapshot;
    }

    std::vector<std::string> accounts = online_accounts;
    std::sort(accounts.begin(), accounts.end());
    accounts.erase(std::unique(accounts.begin(), accounts.end()), accounts.end());

    TdReadConnection conn = TdReadConnection::open(trade_db_path_(), busy_timeout_ms_);
    if (!conn.connected()) {
        SPDLOG_WARN("td stats recompute skipped | reason={}", conn.reason());
        return std::nullopt;
    }
    sqlite3* db = conn.handle();

    // 契约 td-data-sync §5：三样必须同一快照——分次独立查询会因写端在两次查询之间提交
    // 而产生「快照没有却被水位判定已含」的静默误吞。BEGIN DEFERRED 起各 SELECT 共享同一
    // 读快照（读锁保持到 COMMIT），写端短暂阻塞由 busy_timeout 吸收。
    if (sqlite3_exec(db, "BEGIN DEFERRED", nullptr, nullptr, nullptr) != SQLITE_OK) {
        SPDLOG_WARN("td stats recompute begin failed | error=\"{}\"", sqlite3_errmsg(db));
        return std::nullopt;
    }
    const auto rollback = [db]() { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); };

    const bool ok = query_values(db, *trading_day, accounts, snapshot.values) &&
                    query_active(db, *trading_day, accounts, snapshot.active) &&
                    query_watermarks(db, accounts, snapshot.watermarks);
    if (!ok) {
        rollback();
        return std::nullopt;
    }
    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        SPDLOG_WARN("td stats recompute commit failed | error=\"{}\"", sqlite3_errmsg(db));
        rollback();
        return std::nullopt;
    }
    return snapshot;
}

}  // namespace dztrader::webui
