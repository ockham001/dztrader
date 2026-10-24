#include "history_store_internal.h"

#include <SQLiteCpp/Transaction.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <dztrader/sysmonstore/history_store.h>
#include <dztrader/sysmonstore/schema.h>

namespace dztrader::sysmonstore {

namespace {

/// 一分钟压缩桶的时间宽度（与 schema 的 `kMinuteResolutionSec` 同义）。
constexpr int64_t kMinuteSec = kMinuteResolutionSec;

/// 保留策略的两个时间水位。
///
/// 关键语义（契约 #41「保留与压缩」）：**原始记录的保留时长取决于记录间隔**——
/// 记录间隔比一分钟细时，原始记录只在 `raw_retention_sec` 内可读，超期先压缩再删；
/// 记录间隔已不小于一分钟时不做压缩，原始记录直接按 `max_retention_sec` 保留。
///
/// 把这条规则收在一个结构里，是因为它同时决定「压缩要不要跑」与「原始删到哪个水位」
/// 两件事——分散在两处会让其中一个条件被漏掉（本票首版就漏了后者）。
struct RetentionHorizon {
    int64_t raw_cutoff = 0;  ///< 原始记录的删除水位（Unix 秒）
    int64_t max_cutoff = 0;  ///< 聚合记录与标记的删除水位
    bool compact = false;    ///< 是否把超期原始记录压成一分钟聚合

    [[nodiscard]] static RetentionHorizon from(const RetentionPolicy& policy) {
        RetentionHorizon horizon;
        horizon.max_cutoff = policy.now_ts - policy.max_retention_sec;
        // 「记录间隔小于 1 分钟」是唯一的压缩条件：间隔本身 ≥ 60 秒时原始记录粒度已比
        // 一分钟粗，压缩压不出更细的粒度，直接按 30 天保留（不压缩、也不在 24 小时删）。
        horizon.compact = policy.record_interval_sec > 0 && policy.record_interval_sec < kMinuteSec;
        horizon.raw_cutoff =
            horizon.compact ? policy.now_ts - policy.raw_retention_sec : horizon.max_cutoff;
        return horizon;
    }
};

// ===== 读取原始桶（压缩输入）=====

/// 按 bucket_ts 升序取一批超期原始标量桶。
std::vector<ScalarRecord> select_expired_raw_scalars(SQLite::Database& db,
                                                     int64_t cutoff_ts,
                                                     int64_t limit) {
    std::vector<ScalarRecord> rows;
    SQLite::Statement stmt(db, "SELECT " + history_store_internal::scalar_select_columns() +
                                   " FROM sysmon_scalar WHERE level=?1 AND bucket_ts < ?2 "
                                   "ORDER BY bucket_ts ASC LIMIT ?3");
    stmt.bind(1, static_cast<int32_t>(kLevelRaw));
    stmt.bind(2, static_cast<int64_t>(cutoff_ts));
    stmt.bind(3, static_cast<int64_t>(limit));
    while (stmt.executeStep()) {
        rows.push_back(history_store_internal::read_scalar_row(stmt));
    }
    return rows;
}

/// 按 bucket_ts 升序取一批超期原始维度行。
///
/// 维度行与标量行**同样要压缩**：CPU 每核、卷、物理盘、网卡、温度传感器的历史都在长表里，
/// 只压缩宽表会让半个 schema 的历史在 24 小时后静默消失（本票首版的缺陷）。
std::vector<DimensionRecord> select_expired_raw_dimensions(SQLite::Database& db,
                                                           int64_t cutoff_ts,
                                                           int64_t limit) {
    std::vector<DimensionRecord> rows;
    SQLite::Statement stmt(db,
                           "SELECT level,bucket_ts,domain,dimension,metric,min,max,avg,last,n "
                           "FROM sysmon_dimension WHERE level=?1 AND bucket_ts < ?2 "
                           "ORDER BY bucket_ts ASC, domain, dimension, metric LIMIT ?3");
    stmt.bind(1, static_cast<int32_t>(kLevelRaw));
    stmt.bind(2, static_cast<int64_t>(cutoff_ts));
    stmt.bind(3, static_cast<int64_t>(limit));
    while (stmt.executeStep()) {
        DimensionRecord record;
        record.level = stmt.getColumn(0).getInt();
        record.bucket_ts = stmt.getColumn(1).getInt64();
        record.domain = stmt.getColumn(2).getString();
        record.dimension = stmt.getColumn(3).getString();
        record.metric = stmt.getColumn(4).getString();
        if (!stmt.getColumn(9).isNull()) {
            record.aggregate.min = stmt.getColumn(5).getDouble();
            record.aggregate.max = stmt.getColumn(6).getDouble();
            record.aggregate.avg = stmt.getColumn(7).getDouble();
            record.aggregate.last = stmt.getColumn(8).getDouble();
            record.aggregate.count = stmt.getColumn(9).getInt64();
        }
        rows.push_back(std::move(record));
    }
    return rows;
}

/// 取某个分钟桶的既有标量聚合（压缩跨维护周期分批时，必须与已有结果合并，
/// 否则后一批会覆盖前一批的 min/max/n）。
std::optional<ScalarRecord> select_minute_scalar(SQLite::Database& db, int64_t bucket_ts) {
    SQLite::Statement stmt(db, "SELECT " + history_store_internal::scalar_select_columns() +
                                   " FROM sysmon_scalar WHERE level=?1 AND bucket_ts=?2");
    stmt.bind(1, static_cast<int32_t>(kLevelMinute));
    stmt.bind(2, static_cast<int64_t>(bucket_ts));
    if (!stmt.executeStep()) {
        return std::nullopt;
    }
    return history_store_internal::read_scalar_row(stmt);
}

/// 取某个分钟桶内某个维度指标的既有聚合。
std::optional<DimensionRecord> select_minute_dimension(SQLite::Database& db,
                                                       int64_t bucket_ts,
                                                       std::string_view domain,
                                                       std::string_view dimension,
                                                       std::string_view metric) {
    SQLite::Statement stmt(db,
                           "SELECT level,bucket_ts,domain,dimension,metric,min,max,avg,last,n "
                           "FROM sysmon_dimension WHERE level=?1 AND bucket_ts=?2 AND domain=?3 "
                           "AND dimension=?4 AND metric=?5");
    stmt.bind(1, static_cast<int32_t>(kLevelMinute));
    stmt.bind(2, static_cast<int64_t>(bucket_ts));
    stmt.bind(3, std::string(domain));
    stmt.bind(4, std::string(dimension));
    stmt.bind(5, std::string(metric));
    if (!stmt.executeStep()) {
        return std::nullopt;
    }
    DimensionRecord record;
    record.level = stmt.getColumn(0).getInt();
    record.bucket_ts = stmt.getColumn(1).getInt64();
    record.domain = stmt.getColumn(2).getString();
    record.dimension = stmt.getColumn(3).getString();
    record.metric = stmt.getColumn(4).getString();
    if (!stmt.getColumn(9).isNull()) {
        record.aggregate.min = stmt.getColumn(5).getDouble();
        record.aggregate.max = stmt.getColumn(6).getDouble();
        record.aggregate.avg = stmt.getColumn(7).getDouble();
        record.aggregate.last = stmt.getColumn(8).getDouble();
        record.aggregate.count = stmt.getColumn(9).getInt64();
    }
    return record;
}

// ===== 删除 =====

/// 删除某个层级的整桶（标量 + 该桶全部维度行）。
void delete_bucket(SQLite::Database& db, int32_t level, int64_t bucket_ts) {
    SQLite::Statement scalar(db, "DELETE FROM sysmon_scalar WHERE level=?1 AND bucket_ts=?2");
    scalar.bind(1, static_cast<int32_t>(level));
    scalar.bind(2, static_cast<int64_t>(bucket_ts));
    scalar.exec();
    SQLite::Statement dimension(db, "DELETE FROM sysmon_dimension WHERE level=?1 AND bucket_ts=?2");
    dimension.bind(1, static_cast<int32_t>(level));
    dimension.bind(2, static_cast<int64_t>(bucket_ts));
    dimension.exec();
}

/// 删除一条维度行（压缩后按原始行的精确身份删；一个桶里有多条维度行，
/// 按桶删会连带删掉本批未压缩的维度）。
void delete_dimension_row(SQLite::Database& db, const DimensionRecord& record) {
    SQLite::Statement stmt(db,
                           "DELETE FROM sysmon_dimension WHERE level=?1 AND bucket_ts=?2 "
                           "AND domain=?3 AND dimension=?4 AND metric=?5");
    stmt.bind(1, static_cast<int32_t>(record.level));
    stmt.bind(2, static_cast<int64_t>(record.bucket_ts));
    stmt.bind(3, record.domain);
    stmt.bind(4, record.dimension);
    stmt.bind(5, record.metric);
    stmt.exec();
}

// ===== 压缩（单个批次）=====

/// 压缩计数。
struct CompactionCounts {
    int64_t raw_rows = 0;  ///< 被压缩掉的原始行数（标量 + 维度）
    int64_t buckets = 0;   ///< 生成的分钟桶数（按分钟去重）
};

/// 把一批超期原始桶压进一分钟聚合桶，然后删除这批原始桶。
///
/// 合并语义：新聚合与**已有分钟桶**合并（`MetricAggregate::merge` 按样本数加权），
/// 因此跨维护周期分批压缩不会丢 min/max/n。
CompactionCounts compact_batch(SQLite::Database& db,
                               SQLite::Statement& scalar_upsert,
                               SQLite::Statement& dimension_upsert,
                               int64_t cutoff_ts,
                               int64_t limit) {
    CompactionCounts counts;

    const auto raw_scalars = select_expired_raw_scalars(db, cutoff_ts, limit);
    const auto raw_dimensions = select_expired_raw_dimensions(db, cutoff_ts, limit);
    if (raw_scalars.empty() && raw_dimensions.empty()) {
        return counts;
    }

    // ---- 标量：按分钟分组后与既有聚合合并 ----
    std::map<int64_t, ScalarRecord> scalar_buckets;
    for (const auto& record : raw_scalars) {
        const int64_t minute_ts = minute_bucket_start(record.bucket_ts);
        auto it = scalar_buckets.find(minute_ts);
        if (it == scalar_buckets.end()) {
            ScalarRecord entry;
            entry.level = kLevelMinute;
            entry.bucket_ts = minute_ts;
            entry.interval_sec = kMinuteSec;
            it = scalar_buckets.emplace(minute_ts, entry).first;
        }
        for (size_t i = 0; i < kScalarMetrics.size(); ++i) {
            it->second.metrics[i].merge(record.metrics[i]);
        }
    }
    for (auto& [minute_ts, entry] : scalar_buckets) {
        if (auto existing = select_minute_scalar(db, minute_ts)) {
            for (size_t i = 0; i < kScalarMetrics.size(); ++i) {
                // 已有聚合在前、新聚合在后：min/max 取并集，avg 按 n 加权，last 取新的
                // （select_expired_raw_scalars 按 bucket_ts 升序，新聚合时间更晚）。
                existing->metrics[i].merge(entry.metrics[i]);
            }
            entry = std::move(*existing);
        }
    }

    // ---- 维度：按 (分钟, 域, 维度, 指标) 分组后与既有聚合合并 ----
    struct DimensionKey {
        int64_t minute_ts = 0;
        std::string domain;
        std::string dimension;
        std::string metric;
        bool operator<(const DimensionKey& other) const {
            return std::tie(minute_ts, domain, dimension, metric) <
                   std::tie(other.minute_ts, other.domain, other.dimension, other.metric);
        }
    };
    std::map<DimensionKey, MetricAggregate> dimension_buckets;
    for (const auto& record : raw_dimensions) {
        DimensionKey key;
        key.minute_ts = minute_bucket_start(record.bucket_ts);
        key.domain = record.domain;
        key.dimension = record.dimension;
        key.metric = record.metric;
        dimension_buckets[key].merge(record.aggregate);
    }
    for (auto& [key, aggregate] : dimension_buckets) {
        if (auto existing =
                select_minute_dimension(db, key.minute_ts, key.domain, key.dimension, key.metric)) {
            existing->aggregate.merge(aggregate);
            aggregate = existing->aggregate;
        }
    }

    // ---- 写聚合 + 删原始，同一事务：不允许出现「压了没删」（重复计数）或
    //      「删了没压」（数据丢失）的中间状态 ----
    try {
        SQLite::Transaction txn(db);
        for (const auto& [minute_ts, entry] : scalar_buckets) {
            history_store_internal::write_scalar_row(scalar_upsert, entry);
        }
        for (const auto& [key, aggregate] : dimension_buckets) {
            DimensionRecord record;
            record.level = kLevelMinute;
            record.bucket_ts = key.minute_ts;
            record.domain = key.domain;
            record.dimension = key.dimension;
            record.metric = key.metric;
            record.aggregate = aggregate;
            history_store_internal::write_dimension_row(dimension_upsert, record);
        }
        for (const auto& record : raw_scalars) {
            delete_bucket(db, kLevelRaw, record.bucket_ts);
        }
        for (const auto& record : raw_dimensions) {
            delete_dimension_row(db, record);
        }
        txn.commit();
    } catch (const std::exception& e) {
        throw std::runtime_error("history compaction failed | error=\"" + std::string(e.what()) +
                                 "\"");
    }

    counts.raw_rows = static_cast<int64_t>(raw_scalars.size() + raw_dimensions.size());
    // 桶数按「出现过的分钟」去重：标量与维度可能命中同一分钟，不能各算一次。
    std::vector<int64_t> minutes;
    minutes.reserve(scalar_buckets.size() + dimension_buckets.size());
    for (const auto& [minute_ts, entry] : scalar_buckets) {
        minutes.push_back(minute_ts);
    }
    for (const auto& [key, aggregate] : dimension_buckets) {
        minutes.push_back(key.minute_ts);
    }
    std::sort(minutes.begin(), minutes.end());
    minutes.erase(std::unique(minutes.begin(), minutes.end()), minutes.end());
    counts.buckets = static_cast<int64_t>(minutes.size());
    return counts;
}

/// 删除某层级所有 `bucket_ts < cutoff` 的桶，按 `batch` 分段（标量 + 同桶维度一起删，
/// 避免孤儿行）。返回删除的标量桶数。
int64_t delete_expired_level(SQLite::Database& db,
                             int32_t level,
                             int64_t cutoff_ts,
                             int64_t batch) {
    int64_t deleted = 0;
    while (true) {
        std::vector<int64_t> buckets;
        {
            SQLite::Statement select(db,
                                     "SELECT bucket_ts FROM sysmon_scalar WHERE level=?1 "
                                     "AND bucket_ts < ?2 ORDER BY bucket_ts LIMIT ?3");
            select.bind(1, static_cast<int32_t>(level));
            select.bind(2, static_cast<int64_t>(cutoff_ts));
            select.bind(3, static_cast<int64_t>(batch));
            while (select.executeStep()) {
                buckets.push_back(select.getColumn(0).getInt64());
            }
        }
        try {
            SQLite::Transaction txn(db);
            for (const int64_t bucket_ts : buckets) {
                delete_bucket(db, level, bucket_ts);
            }
            if (buckets.empty()) {
                // 标量表已无该层级的超期桶，但维度表可能还有（写入方异常时会短暂出现）：
                // 一并按水位清掉，避免孤儿行永久滞留。
                SQLite::Statement orphan(
                    db, "DELETE FROM sysmon_dimension WHERE level=?1 AND bucket_ts < ?2");
                orphan.bind(1, static_cast<int32_t>(level));
                orphan.bind(2, static_cast<int64_t>(cutoff_ts));
                orphan.exec();
            }
            txn.commit();
        } catch (const std::exception& e) {
            throw std::runtime_error("history delete failed | error=\"" + std::string(e.what()) +
                                     "\"");
        }
        deleted += static_cast<int64_t>(buckets.size());
        if (static_cast<int64_t>(buckets.size()) < batch) {
            break;
        }
    }
    return deleted;
}

/// 删除超期标记。
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

/// 总量上限兜底：**从全库最旧的桶开始**删，直到库文件回落到 `max_bytes` 内。
///
/// 「最旧优先」按 `bucket_ts` 全局排序（不按层级分组）：同一时刻的原始记录与分钟聚合
/// 代表同一段历史，先删哪一个都不改变「保留最新数据」的语义；按层级固定先后删是错的
/// ——分钟聚合恰恰是更粗、更该长期保留的那一份。
int64_t enforce_size_limit(SQLite::Database& db,
                           const std::filesystem::path& path,
                           int64_t max_bytes,
                           int64_t batch) {
    try {
        db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    } catch (const std::exception& e) {
        SPDLOG_WARN("history size checkpoint failed | error=\"{}\"", e.what());
    }
    int64_t deleted = 0;
    while (history_store_internal::history_file_bytes(path) > max_bytes) {
        // 取全库最旧的一批桶（跨层级混合排序，按 (level, bucket_ts) 配对删除）。
        std::vector<std::pair<int32_t, int64_t>> pairs;
        {
            SQLite::Statement select(db,
                                     "SELECT level, bucket_ts FROM sysmon_scalar "
                                     "ORDER BY bucket_ts, level LIMIT ?1");
            select.bind(1, static_cast<int64_t>(batch));
            while (select.executeStep()) {
                pairs.emplace_back(select.getColumn(0).getInt(), select.getColumn(1).getInt64());
            }
        }
        if (pairs.empty()) {
            // 数据已删空仍超限：占用在元数据/空闲页上，VACUUM 收尾后停止
            // （不 VACUUM 会在「已删空但文件没缩」上无限循环）。
            try {
                db.exec("VACUUM");
                db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
            } catch (const std::exception& e) {
                SPDLOG_WARN("history size vacuum failed | error=\"{}\"", e.what());
            }
            break;
        }
        try {
            SQLite::Transaction txn(db);
            for (const auto& [level, bucket_ts] : pairs) {
                delete_bucket(db, level, bucket_ts);
            }
            txn.commit();
        } catch (const std::exception& e) {
            throw std::runtime_error("history size limit failed | error=\"" +
                                     std::string(e.what()) + "\"");
        }
        deleted += static_cast<int64_t>(pairs.size());
        try {
            db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
        } catch (const std::exception& e) {
            SPDLOG_WARN("history size checkpoint failed | error=\"{}\"", e.what());
        }
    }
    if (deleted > 0) {
        SPDLOG_INFO("history size limit enforced | deleted_buckets={} max_bytes={}", deleted,
                    max_bytes);
    }
    return deleted;
}

}  // namespace

RetentionResult HistoryStore::apply_retention(const RetentionPolicy& policy) {
    RetentionResult result;
    auto& db = *impl_->db;
    result.bytes_before = history_store_internal::history_file_bytes(impl_->path);

    const auto horizon = RetentionHorizon::from(policy);
    const int64_t batch = std::max<int64_t>(1, policy.maintenance_batch);

    // 1. 压缩（仅当记录间隔 < 1 分钟）：超期原始记录压成一分钟聚合后再删。
    //    记录间隔已 ≥ 1 分钟时不压缩——原始记录本就是分钟级或更粗，压不出更细的粒度，
    //    且此时原始记录的保留水位是 max_retention_sec（30 天）而不是 24 小时。
    if (horizon.compact) {
        while (true) {
            const auto counts = compact_batch(db, *impl_->scalar_upsert, *impl_->dimension_upsert,
                                              horizon.raw_cutoff, batch);
            if (counts.raw_rows == 0) {
                break;
            }
            result.compacted_rows += counts.raw_rows;
            result.compacted_buckets += counts.buckets;
        }
    }

    // 2. 删除超期：原始按 horizon.raw_cutoff（可压缩模式下 24 小时，否则 30 天）、
    //    聚合与标记按 max_cutoff（30 天）。
    if (horizon.raw_cutoff > 0) {
        result.deleted_raw_rows += delete_expired_level(db, kLevelRaw, horizon.raw_cutoff, batch);
    }
    if (horizon.max_cutoff > 0) {
        result.deleted_minute_rows +=
            delete_expired_level(db, kLevelMinute, horizon.max_cutoff, batch);
        result.deleted_marks += delete_expired_marks(db, horizon.max_cutoff, batch);
    }

    // 3. 总量上限兜底：从全库最旧的桶开始删，直到回落到上限内。
    if (policy.max_bytes > 0) {
        result.deleted_by_size_rows =
            enforce_size_limit(db, impl_->path, static_cast<int64_t>(policy.max_bytes), batch);
    }

    result.bytes_after = history_store_internal::history_file_bytes(impl_->path);
    return result;
}

}  // namespace dztrader::sysmonstore
