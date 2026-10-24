#include "history_internal.h"

#include <SQLiteCpp/Transaction.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <dztrader/sysmonstore/history_store.h>

namespace dztrader::sysmonstore {

namespace {

/// 按时间升序取一批超期原始桶。
std::vector<ScalarRecord> select_expired_raw(SQLite::Database& db,
                                             int64_t cutoff_ts,
                                             int64_t limit);

/// 取某个分钟桶的既有聚合行（压缩可能跨多个维护周期分批进行，必须与已有结果合并，
/// 否则后一批会覆盖前一批的聚合，`min`/`max`/`n` 都会丢）。
std::optional<ScalarRecord> select_minute_bucket(SQLite::Database& db, int64_t bucket_ts);

/// 删除一批已压缩的原始桶（宽表 + 长表同桶数据）。
void delete_raw_buckets(SQLite::Database& db, const std::vector<ScalarRecord>& rows);

/// 删除指定层级的超期桶（宽表与长表一并删，避免孤儿维度行）。
/// @return 删除的宽表行数
int64_t delete_expired_level(SQLite::Database& db, int32_t level, int64_t cutoff_ts, int64_t batch);

/// 删除超期标记。
int64_t delete_expired_marks(SQLite::Database& db, int64_t cutoff_ts, int64_t batch);

/// 从最旧的数据开始删，直到库文件回落到 max_bytes 内。
int64_t enforce_size_limit(SQLite::Database& db,
                           const std::filesystem::path& path,
                           int64_t max_bytes,
                           int64_t batch);

}  // namespace

RetentionResult HistoryStore::apply_retention(const RetentionPolicy& policy) {
    RetentionResult result;
    auto& db = *impl_->db;
    result.bytes_before = internal::history_file_bytes(impl_->path);

    const int64_t raw_cutoff = policy.now_ts - policy.raw_retention_sec;
    const int64_t max_cutoff = policy.now_ts - policy.max_retention_sec;
    const int64_t batch = std::max<int64_t>(1, policy.maintenance_batch);

    // 1. 压缩：记录间隔 < 一分钟时，超期原始记录先聚合成一分钟桶，再随删除一起清理。
    //    记录间隔已 >= 60 时不做压缩——原始记录本就是分钟级或更粗，压不出更细的粒度
    //    （契约 #41「保留与压缩」明文）。
    if (policy.record_interval_sec > 0 && policy.record_interval_sec < kMinuteResolutionSec &&
        raw_cutoff > 0) {
        while (true) {
            auto raw = select_expired_raw(db, raw_cutoff, batch);
            if (raw.empty()) {
                break;
            }
            // 按分钟分组后与已有聚合合并：一个分钟桶可能需要多个维护周期才凑满
            // （原始记录是分批取的），直接覆盖会丢掉前一批的 min/max/n。
            std::map<int64_t, ScalarRecord> compacted;
            for (const auto& record : raw) {
                const int64_t minute_ts = minute_bucket_start(record.bucket_ts);
                auto it = compacted.find(minute_ts);
                if (it == compacted.end()) {
                    ScalarRecord entry;
                    entry.level = kLevelMinute;
                    entry.bucket_ts = minute_ts;
                    entry.interval_sec = kMinuteResolutionSec;
                    it = compacted.emplace(minute_ts, entry).first;
                }
                for (size_t i = 0; i < kScalarMetrics.size(); ++i) {
                    // 原始桶的 last 是「该桶内最后一个样本」，同分钟内后桶的 last 更晚：
                    // 按 bucket_ts 升序遍历（select_expired_raw 保证），merge 的 last 覆盖
                    // 语义正好给出该分钟真正的最后一个值。
                    if (!record.metrics[i].empty()) {
                        it->second.metrics[i].merge(record.metrics[i]);
                    }
                }
            }

            HistoryBatch out;
            for (const auto& [minute_ts, entry] : compacted) {
                if (auto existing = select_minute_bucket(db, minute_ts)) {
                    ScalarRecord merged = *existing;
                    for (size_t i = 0; i < kScalarMetrics.size(); ++i) {
                        // 已有聚合（时间更早的一批）在前、新聚合在后：先合并旧值再并新值，
                        // 这样 last 取到新一批的 last（时间更晚），min/max 取并集。
                        MetricAggregate base = merged.metrics[i];
                        base.merge(entry.metrics[i]);
                        merged.metrics[i] = base;
                    }
                    out.scalars.push_back(std::move(merged));
                } else {
                    out.scalars.push_back(entry);
                }
            }
            ++result.compacted_buckets;

            try {
                SQLite::Transaction txn(db);
                for (const auto& record : out.scalars) {
                    internal::write_scalar_row(*impl_, record);
                }
                txn.commit();
            } catch (const std::exception& e) {
                throw std::runtime_error("history compaction write failed | error=\"" +
                                         std::string(e.what()) + "\"");
            }
            result.compacted_rows += static_cast<int64_t>(raw.size());

            // 压完立即删掉这批原始桶：否则下一轮会重复压缩同一批数据（重复计数）。
            try {
                SQLite::Transaction txn(db);
                delete_raw_buckets(db, raw);
                txn.commit();
            } catch (const std::exception& e) {
                throw std::runtime_error("history compaction cleanup failed | error=\"" +
                                         std::string(e.what()) + "\"");
            }
        }
    }

    // 2. 删除超期：原始按 raw_retention_sec、聚合按 max_retention_sec，标记按 max_retention_sec。
    if (raw_cutoff > 0) {
        result.deleted_raw_rows += delete_expired_level(db, kLevelRaw, raw_cutoff, batch);
    }
    if (max_cutoff > 0) {
        result.deleted_minute_rows += delete_expired_level(db, kLevelMinute, max_cutoff, batch);
        result.deleted_marks += delete_expired_marks(db, max_cutoff, batch);
    }

    // 3. 总量上限兜底：从最旧的数据开始删，直到回落到上限内。
    if (policy.max_bytes > 0) {
        result.deleted_by_size_rows =
            enforce_size_limit(db, impl_->path, static_cast<int64_t>(policy.max_bytes), batch);
    }

    result.bytes_after = internal::history_file_bytes(impl_->path);
    return result;
}

namespace {

std::vector<ScalarRecord> select_expired_raw(SQLite::Database& db,
                                             int64_t cutoff_ts,
                                             int64_t limit) {
    std::vector<ScalarRecord> rows;
    SQLite::Statement stmt(db, "SELECT " + internal::scalar_select_columns() +
                                   " FROM sysmon_scalar WHERE level=?1 AND bucket_ts < ?2 "
                                   "ORDER BY bucket_ts ASC LIMIT ?3");
    stmt.bind(1, static_cast<int32_t>(kLevelRaw));
    stmt.bind(2, static_cast<int64_t>(cutoff_ts));
    stmt.bind(3, static_cast<int64_t>(limit));
    while (stmt.executeStep()) {
        rows.push_back(internal::read_scalar_row(stmt));
    }
    return rows;
}

std::optional<ScalarRecord> select_minute_bucket(SQLite::Database& db, int64_t bucket_ts) {
    SQLite::Statement stmt(db, "SELECT " + internal::scalar_select_columns() +
                                   " FROM sysmon_scalar WHERE level=?1 AND bucket_ts=?2");
    stmt.bind(1, static_cast<int32_t>(kLevelMinute));
    stmt.bind(2, static_cast<int64_t>(bucket_ts));
    if (!stmt.executeStep()) {
        return std::nullopt;
    }
    return internal::read_scalar_row(stmt);
}

/// 删除一批已压缩的原始桶（宽表 + 长表同桶数据）。
void delete_raw_buckets(SQLite::Database& db, const std::vector<ScalarRecord>& rows) {
    SQLite::Statement scalar(db, "DELETE FROM sysmon_scalar WHERE level=?1 AND bucket_ts=?2");
    SQLite::Statement dimension(db, "DELETE FROM sysmon_dimension WHERE level=?1 AND bucket_ts=?2");
    for (const auto& row : rows) {
        scalar.reset();
        scalar.bind(1, static_cast<int32_t>(kLevelRaw));
        scalar.bind(2, static_cast<int64_t>(row.bucket_ts));
        scalar.exec();
        dimension.reset();
        dimension.bind(1, static_cast<int32_t>(kLevelRaw));
        dimension.bind(2, static_cast<int64_t>(row.bucket_ts));
        dimension.exec();
    }
}

int64_t delete_expired_level(SQLite::Database& db,
                             int32_t level,
                             int64_t cutoff_ts,
                             int64_t batch) {
    int64_t deleted = 0;
    while (true) {
        SQLite::Transaction txn(db);
        // 表是 WITHOUT ROWID（复合主键 (level,bucket_ts)），没有 rowid 列：
        // 按主键列做子查询删除。
        SQLite::Statement stmt(db,
                               "DELETE FROM sysmon_scalar WHERE level=?1 AND bucket_ts IN ("
                               "SELECT bucket_ts FROM sysmon_scalar "
                               "WHERE level=?1 AND bucket_ts < ?2 "
                               "ORDER BY bucket_ts LIMIT ?3)");
        stmt.bind(1, static_cast<int32_t>(level));
        stmt.bind(2, static_cast<int64_t>(cutoff_ts));
        stmt.bind(3, static_cast<int64_t>(batch));
        stmt.exec();
        const int changed = db.getChanges();
        // 长表同桶数据一并删除，否则按域查询会读到已被清理的桶（孤儿行）。
        SQLite::Statement dim(db, "DELETE FROM sysmon_dimension WHERE level=?1 AND bucket_ts < ?2");
        dim.bind(1, static_cast<int32_t>(level));
        dim.bind(2, static_cast<int64_t>(cutoff_ts));
        dim.exec();
        txn.commit();
        deleted += changed;
        if (changed < batch) {
            break;
        }
    }
    return deleted;
}

int64_t delete_expired_marks(SQLite::Database& db, int64_t cutoff_ts, int64_t batch) {
    int64_t deleted = 0;
    while (true) {
        SQLite::Transaction txn(db);
        SQLite::Statement stmt(db,
                               "DELETE FROM sysmon_mark WHERE id IN (SELECT id FROM sysmon_mark "
                               "WHERE ts < ?1 ORDER BY ts LIMIT ?2)");
        stmt.bind(1, static_cast<int64_t>(cutoff_ts));
        stmt.bind(2, static_cast<int64_t>(batch));
        stmt.exec();
        const int changed = db.getChanges();
        txn.commit();
        deleted += changed;
        if (changed < batch) {
            break;
        }
    }
    return deleted;
}

int64_t enforce_size_limit(SQLite::Database& db,
                           const std::filesystem::path& path,
                           int64_t max_bytes,
                           int64_t batch) {
    // 先 checkpoint 让 -wal 并回主库：否则库文件看起来偏小，上限形同失效。
    try {
        db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    } catch (const std::exception& e) {
        SPDLOG_WARN("history size checkpoint failed | error=\"{}\"", e.what());
    }
    int64_t deleted = 0;
    while (internal::history_file_bytes(path) > max_bytes) {
        // 从最旧的行开始删，且优先牺牲分钟聚合（更旧、粒度更粗），再删原始记录。
        int64_t removed = 0;
        for (const auto level : {kLevelMinute, kLevelRaw}) {
            SQLite::Transaction txn(db);
            // WITHOUT ROWID：按主键列删，不引用 rowid。
            SQLite::Statement stmt(db,
                                   "DELETE FROM sysmon_scalar WHERE level=?1 AND bucket_ts IN ("
                                   "SELECT bucket_ts FROM sysmon_scalar "
                                   "WHERE level=?1 ORDER BY bucket_ts LIMIT ?2)");
            stmt.bind(1, static_cast<int32_t>(level));
            stmt.bind(2, static_cast<int64_t>(batch));
            stmt.exec();
            const int changed = db.getChanges();
            SQLite::Statement dim(db,
                                  "DELETE FROM sysmon_dimension WHERE level=?1 AND bucket_ts IN ("
                                  "SELECT bucket_ts FROM sysmon_dimension "
                                  "WHERE level=?1 ORDER BY bucket_ts LIMIT ?2)");
            dim.bind(1, static_cast<int32_t>(level));
            dim.bind(2, static_cast<int64_t>(batch * 16));
            dim.exec();
            txn.commit();
            if (changed > 0) {
                removed = changed;
                break;
            }
        }
        if (removed == 0) {
            // 数据已删空仍超限：说明占用在元数据/空闲页上，VACUUM 收尾后停止
            // （不 VACUUM 就会在「已删空但文件没缩」上无限循环）。
            try {
                db.exec("VACUUM");
                db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
            } catch (const std::exception& e) {
                SPDLOG_WARN("history size vacuum failed | error=\"{}\"", e.what());
            }
            break;
        }
        deleted += removed;
    }
    if (deleted > 0) {
        SPDLOG_INFO("history size limit enforced | deleted_rows={} max_bytes={}", deleted,
                    max_bytes);
    }
    return deleted;
}

}  // namespace

}  // namespace dztrader::sysmonstore
