#include "history_internal.h"

#include <SQLiteCpp/Transaction.h>

#include <spdlog/spdlog.h>

#include <format>
#include <stdexcept>
#include <string>
#include <utility>

#include <dztrader/sysmonstore/history_store.h>

namespace dztrader::sysmonstore {

namespace {

/// WAL 转换：失败不致命（沿用当前模式），记 WARN 便于诊断。
void try_set_journal_wal(SQLite::Database& db) {
    try {
        const std::string mode = db.execAndGet("PRAGMA journal_mode=WAL").getString();
        if (mode != "wal") {
            SPDLOG_WARN("sysmon history wal conversion not applied | mode={}", mode);
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("sysmon history wal conversion failed | error=\"{}\"", e.what());
    }
}

}  // namespace

std::string_view history_availability_reason(HistoryAvailability availability) noexcept {
    switch (availability) {
        case HistoryAvailability::Ok:
            return "ok";
        case HistoryAvailability::NotFound:
            return "not_found";
        case HistoryAvailability::SchemaMismatch:
            return "schema_mismatch";
        case HistoryAvailability::OpenFailed:
            return "open_failed";
    }
    return "open_failed";
}

HistoryStore::HistoryStore(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
HistoryStore::HistoryStore(HistoryStore&&) noexcept = default;
HistoryStore& HistoryStore::operator=(HistoryStore&&) noexcept = default;
HistoryStore::~HistoryStore() = default;

HistoryStore HistoryStore::open(const std::filesystem::path& path, int busy_timeout_ms) {
    internal::ensure_parent_dir(path);

    auto impl = std::make_unique<Impl>();
    impl->path = path;
    try {
        impl->db = std::make_unique<SQLite::Database>(path.string(),
                                                      SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    } catch (const std::exception& e) {
        throw std::runtime_error("history open failed | path=" + path.string() + " error=\"" +
                                 e.what() + "\"");
    }
    auto& db = *impl->db;
    try {
        db.exec(std::format("PRAGMA busy_timeout={}", busy_timeout_ms));
        // 历史数据必须抗断电：写入是低频批量、不在交易路径上，故用 FULL 而非 NORMAL。
        db.exec("PRAGMA synchronous=FULL");
        try_set_journal_wal(db);
        db.exec(build_schema_ddl());
    } catch (const std::exception& e) {
        throw std::runtime_error("history schema failed | path=" + path.string() + " error=\"" +
                                 e.what() + "\"");
    }

    // 版本处理：比当前新 ⇒ 拒绝打开（旧写入方不得覆盖新结构）；
    // 旧或缺失 ⇒ 写成当前版本（版本 1 尚无历史包袱，建表 DDL 幂等）。
    const int existing = internal::read_schema_version(db);
    if (existing > kSchemaVersion) {
        throw std::runtime_error(
            std::format("history schema too new | path={} found={} expected={}", path.string(),
                        existing, kSchemaVersion));
    }
    try {
        SQLite::Transaction txn(db);
        SQLite::Statement upsert(db,
                                 "INSERT INTO sysmon_meta (key,value) VALUES (?1,?2) "
                                 "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        upsert.bind(1, std::string(kMetaSchemaVersion));
        upsert.bind(2, std::to_string(kSchemaVersion));
        upsert.exec();
        txn.commit();
    } catch (const std::exception& e) {
        throw std::runtime_error("history migrate failed | path=" + path.string() + " error=\"" +
                                 e.what() + "\"");
    }

    try {
        impl->scalar_upsert =
            std::make_unique<SQLite::Statement>(db, internal::scalar_upsert_sql());
        impl->dimension_upsert =
            std::make_unique<SQLite::Statement>(db, internal::dimension_upsert_sql());
        impl->mark_insert = std::make_unique<SQLite::Statement>(db, internal::mark_insert_sql());
    } catch (const std::exception& e) {
        throw std::runtime_error("history prepare failed | path=" + path.string() + " error=\"" +
                                 e.what() + "\"");
    }
    return HistoryStore(std::move(impl));
}

int HistoryStore::schema_version() const { return internal::read_schema_version(*impl_->db); }

const std::filesystem::path& HistoryStore::path() const noexcept { return impl_->path; }

void HistoryStore::write_batch(const HistoryBatch& batch) {
    if (batch.empty()) {
        return;
    }
    auto& db = *impl_->db;
    try {
        SQLite::Transaction txn(db);
        for (const auto& record : batch.scalars) {
            internal::write_scalar_row(*impl_, record);
        }
        for (const auto& record : batch.dimensions) {
            internal::write_dimension_row(*impl_, record);
        }
        for (const auto& mark : batch.marks) {
            internal::insert_mark(db, mark);
        }
        txn.commit();
    } catch (const std::exception& e) {
        throw std::runtime_error("history write failed | error=\"" + std::string(e.what()) + "\"");
    }
}

void HistoryStore::write_mark(const HistoryMark& mark) {
    try {
        SQLite::Transaction txn(*impl_->db);
        internal::insert_mark(*impl_->db, mark);
        txn.commit();
    } catch (const std::exception& e) {
        throw std::runtime_error("history mark write failed | error=\"" + std::string(e.what()) +
                                 "\"");
    }
}

void HistoryStore::close() {
    if (!impl_ || !impl_->db) {
        return;
    }
    impl_->scalar_upsert.reset();
    impl_->dimension_upsert.reset();
    impl_->mark_insert.reset();
    // WAL checkpoint：退出时把 -wal 内容并回主库，减少下次启动的恢复工作。
    try {
        impl_->db->exec("PRAGMA wal_checkpoint(TRUNCATE)");
    } catch (const std::exception& e) {
        SPDLOG_WARN("history wal checkpoint failed | error=\"{}\"", e.what());
    }
    impl_->db.reset();
}

}  // namespace dztrader::sysmonstore
