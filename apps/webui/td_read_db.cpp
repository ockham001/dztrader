#include "td_read_db.h"

#include "td_orders_query.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace dztrader::webui {

namespace {

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

/// `PRAGMA user_version` 探测：返回 nullptr = 版本匹配，否则为 reason 码。
const char* probe_schema_version(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr) != SQLITE_OK) {
        const int code = sqlite3_extended_errcode(db);
        SPDLOG_WARN("td read db schema probe failed | error=\"{}\"", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return td_sqlite_error_reason(code);
    }
    int version = -1;
    const int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (step != SQLITE_ROW) {
        const int code = sqlite3_extended_errcode(db);
        SPDLOG_WARN("td read db schema probe step failed | error=\"{}\"", sqlite3_errmsg(db));
        // 非 SQLite 文件 / 页损坏：`sqlite3_open` 是惰性的，失败只在首次读页暴露，
        // 契约 rest §2.9.8 把这类归 open_failed（不是 query_failed）。
        return td_sqlite_error_reason(code);
    }
    if (version != kTdOrdersReadSchemaVersion) {
        SPDLOG_WARN("td read db schema mismatch | found={} expected={}", version,
                    kTdOrdersReadSchemaVersion);
        return kTdReasonSchemaMismatch;
    }
    return nullptr;
}

}  // namespace

const char* td_sqlite_error_reason(int code) {
    if (is_busy(code)) {
        return kTdReasonBusy;
    }
    if (is_corrupt(code)) {
        return kTdReasonOpenFailed;
    }
    return kTdReasonQueryFailed;
}

TdReadConnection::TdReadConnection(sqlite3* db, const char* reason) noexcept
    : db_(db),
      reason_(reason) {}

TdReadConnection::~TdReadConnection() { close(); }

TdReadConnection::TdReadConnection(TdReadConnection&& other) noexcept
    : db_(other.db_),
      reason_(other.reason_) {
    other.db_ = nullptr;
    other.reason_ = nullptr;
}

TdReadConnection& TdReadConnection::operator=(TdReadConnection&& other) noexcept {
    if (this != &other) {
        close();
        db_ = other.db_;
        reason_ = other.reason_;
        other.db_ = nullptr;
        other.reason_ = nullptr;
    }
    return *this;
}

void TdReadConnection::close() noexcept {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

TdReadConnection TdReadConnection::open(const std::string& trade_db_path, int busy_timeout_ms) {
    std::error_code ec;
    if (trade_db_path.empty() || !std::filesystem::exists(trade_db_path, ec) || ec) {
        return TdReadConnection(nullptr, kTdReasonNotFound);
    }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(trade_db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        const int code = db != nullptr ? sqlite3_extended_errcode(db) : SQLITE_CANTOPEN;
        SPDLOG_WARN("td read db open failed | path={} error=\"{}\"", trade_db_path,
                    db != nullptr ? sqlite3_errmsg(db) : "unknown");
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return TdReadConnection(nullptr, is_busy(code) ? kTdReasonBusy : kTdReasonOpenFailed);
    }
    sqlite3_busy_timeout(db, busy_timeout_ms);
    const char* reason = probe_schema_version(db);
    if (reason != nullptr) {
        sqlite3_close(db);
        return TdReadConnection(nullptr, reason);
    }
    return TdReadConnection(db, nullptr);
}

bool TdReadConnection::connected() const noexcept { return db_ != nullptr; }

const char* TdReadConnection::reason() const noexcept { return reason_; }

sqlite3* TdReadConnection::handle() const noexcept { return db_; }

nlohmann::json td_degraded_body(const char* reason) {
    return {{"available", false}, {"reason", reason}};
}

}  // namespace dztrader::webui
