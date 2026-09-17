#include "sqlite_driver.h"

#include <chrono>
#include <thread>

#include <SQLiteCpp/Statement.h>
#include <spdlog/spdlog.h>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>

#include "sqlite_migrations.h"

namespace dztrader::db {

namespace {

std::string option_or(const std::map<std::string, std::string, std::less<>>& options,
                      std::string_view key, std::string fallback) {
    const auto it = options.find(key);
    return it == options.end() ? std::move(fallback) : it->second;
}

/// 应用连接级 PRAGMA（迁移连接与 Session 连接共用）
void apply_pragmas(SQLite::Database& db,
                   const std::map<std::string, std::string, std::less<>>& options) {
    db.exec("PRAGMA synchronous=" + option_or(options, "synchronous", "full"));
    const std::string journal_mode = option_or(options, "journal_mode", "wal");
    bool wal_ready = false;
    for (int attempt = 0; attempt < 3 && !wal_ready; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        try {
            wal_ready =
                db.execAndGet("PRAGMA journal_mode=" + journal_mode).getString() == journal_mode;
        } catch (const std::exception&) {
            // 独占锁竞争: 有界重试
        }
    }
    if (!wal_ready) {
        SPDLOG_WARN("td db WAL conversion not applied, continue in current mode");
    }
    db.exec("PRAGMA busy_timeout=" + option_or(options, "busy_timeout_ms", "5000"));
    db.exec("PRAGMA cache_size=-" + option_or(options, "cache_size_kb", "8000"));
    db.exec("PRAGMA temp_store=" + option_or(options, "temp_store", "memory"));
}

}  // namespace

SqliteDatabaseImpl::SqliteDatabaseImpl(
    std::string path, std::map<std::string, std::string, std::less<>> options,
    std::span<const ResourceSchema> schemas)
    : path_(std::move(path)), options_(std::move(options)), schemas_(schemas.begin(), schemas.end()) {
    for (const ResourceSchema& schema : schemas_) {
        schema_by_name_.emplace(schema.name, &schema);
    }
}

const ResourceSchema* SqliteDatabaseImpl::find_schema(std::string_view name) const {
    const auto it = schema_by_name_.find(name);
    return it == schema_by_name_.end() ? nullptr : it->second;
}

Capability SqliteDatabaseImpl::capabilities() const noexcept {
    return Capability::Transactions | Capability::SnapshotRead | Capability::Upsert |
           Capability::OrderedRangeScan;
}

std::unique_ptr<Session> SqliteDatabaseImpl::session(bool read_only) {
    // Task 5 实现（本任务先声明并抛错，Task 5 替换）
    (void)read_only;
    throw Exception(DZ_EC_DB_QUERY_FAILED, "sqlite session not implemented yet");
}

void SqliteDatabaseImpl::migrate() {
    try {
        SQLite::Database db(path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        apply_pragmas(db, options_);
        internal::apply_td_migrations(db);
        internal::create_missing_collections(db, schemas_);
    } catch (const Exception&) {
        throw;
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_DB_MIGRATE_FAILED, "sqlite migrate failed: {}", e.what());
    }
}

std::unique_ptr<Database> Database::open(const Config& config,
                                         std::span<const ResourceSchema> schemas) {
    if (config.backend == "sqlite") {
        const auto it = config.options.find("path");
        if (it == config.options.end() || it->second.empty()) {
            throw Exception(DZ_EC_DB_OPEN_FAILED, "sqlite path option is required");
        }
        return std::make_unique<SqliteDatabaseImpl>(it->second, config.options, schemas);
    }
    throw Exception(DZ_EC_DB_UNSUPPORTED_BACKEND, "unsupported backend: backend={}", config.backend);
}

}  // namespace dztrader::db
