#include "history_internal.h"

#include <spdlog/spdlog.h>

#include <format>
#include <stdexcept>
#include <string>
#include <utility>

namespace dztrader::sysmonstore::internal {

std::string quote_identifier(std::string_view identifier) {
    std::string out;
    out.reserve(identifier.size() + 2);
    out.push_back('"');
    for (const char ch : identifier) {
        if (ch == '"') {
            out.push_back('"');  // SQL 规则：标识符内的双引号翻倍
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

std::string scalar_insert_columns() {
    std::string columns = "level,bucket_ts,interval_sec";
    for (const auto& metric : kScalarMetrics) {
        for (const auto suffix : kAggregateSuffixes) {
            columns += std::format(",{}_{}", metric.column, suffix);
        }
    }
    return columns;
}

std::string scalar_upsert_sql() {
    const size_t total = 3 + kScalarMetrics.size() * 5;
    std::string placeholders;
    placeholders.reserve(total * 4);
    for (size_t i = 1; i <= total; ++i) {
        if (i > 1) {
            placeholders.push_back(',');
        }
        placeholders += std::format("?{}", i);
    }
    std::string sql = "INSERT INTO sysmon_scalar (" + scalar_insert_columns() + ") VALUES (" +
                      placeholders + ") ON CONFLICT(level,bucket_ts) DO UPDATE SET ";
    bool first = true;
    for (const auto& metric : kScalarMetrics) {
        for (const auto suffix : kAggregateSuffixes) {
            const std::string column = std::format("{}_{}", metric.column, suffix);
            if (!first) {
                sql.push_back(',');
            }
            first = false;
            sql +=
                std::format("{}=excluded.{}", quote_identifier(column), quote_identifier(column));
        }
    }
    return sql;
}

std::string scalar_select_columns() {
    std::string columns = "level,bucket_ts,interval_sec";
    for (const auto& metric : kScalarMetrics) {
        for (const auto suffix : kAggregateSuffixes) {
            columns += std::format(",{}_{}", metric.column, suffix);
        }
    }
    return columns;
}

std::string dimension_upsert_sql() {
    return "INSERT INTO sysmon_dimension "
           "(level,bucket_ts,domain,dimension,metric,min,max,avg,last,n) "
           "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) "
           "ON CONFLICT(level,bucket_ts,domain,dimension,metric) DO UPDATE SET "
           "min=excluded.min,max=excluded.max,avg=excluded.avg,last=excluded.last,n=excluded.n";
}

std::string mark_insert_sql() {
    return "INSERT INTO sysmon_mark (ts,kind,detail) VALUES (?1,?2,?3)";
}

void bind_aggregate(SQLite::Statement& stmt,
                    int& index,
                    const MetricAggregate& aggregate,
                    bool integral) {
    if (integral) {
        stmt.bind(index++, static_cast<int64_t>(aggregate.min));
        stmt.bind(index++, static_cast<int64_t>(aggregate.max));
    } else {
        stmt.bind(index++, aggregate.min);
        stmt.bind(index++, aggregate.max);
    }
    // avg 恒为实数、n 恒为整数：与建表列类型一致，避免 SQLite 亲和性把均值截断。
    stmt.bind(index++, aggregate.avg);
    if (integral) {
        stmt.bind(index++, static_cast<int64_t>(aggregate.last));
    } else {
        stmt.bind(index++, aggregate.last);
    }
    stmt.bind(index++, static_cast<int64_t>(aggregate.count));
}

void bind_null_aggregate(SQLite::Statement& stmt, int& index) {
    for (int i = 0; i < 5; ++i) {
        stmt.bind(index++);  // SQLiteCpp 无值重载 = NULL
    }
}

void write_scalar_row(HistoryStoreImpl& impl, const ScalarRecord& record) {
    auto& stmt = *impl.scalar_upsert;
    stmt.reset();
    int index = 1;
    stmt.bind(index++, static_cast<int32_t>(record.level));
    stmt.bind(index++, static_cast<int64_t>(record.bucket_ts));
    stmt.bind(index++, static_cast<int64_t>(record.interval_sec));
    for (size_t i = 0; i < kScalarMetrics.size(); ++i) {
        const auto& aggregate = record.metrics[i];
        const bool integral = kScalarMetrics[i].type == ScalarType::Integer;
        if (aggregate.empty()) {
            // count=0 = 本桶内该指标无有效样本：5 列全 NULL（不得写 0 冒充）。
            bind_null_aggregate(stmt, index);
            continue;
        }
        bind_aggregate(stmt, index, aggregate, integral);
    }
    stmt.exec();
}

void write_dimension_row(HistoryStoreImpl& impl, const DimensionRecord& record) {
    if (!dimension_domain_has_metric(record.domain, record.metric)) {
        // 未注册的域/指标名不落库：schema 是唯一真相源，出现它说明写入方有 bug。
        SPDLOG_WARN("history dimension skipped | domain={} metric={} reason=not_registered",
                    record.domain, record.metric);
        return;
    }
    auto& stmt = *impl.dimension_upsert;
    stmt.reset();
    int index = 1;
    stmt.bind(index++, static_cast<int32_t>(record.level));
    stmt.bind(index++, static_cast<int64_t>(record.bucket_ts));
    stmt.bind(index++, record.domain);
    stmt.bind(index++, record.dimension);
    stmt.bind(index++, record.metric);
    if (record.aggregate.empty()) {
        bind_null_aggregate(stmt, index);
    } else {
        // 长表统一 REAL 存储：维度指标既有比率（usage_ratio）也有字节数（rx_bytes_total），
        // 用 REAL 一种表示让 min/max/avg 的合并逻辑只有一份；字节数上限远小于 2^53，
        // double 表示无损（契约 sysmon 的计数器均为 64 位整数，实际不可能达到 2^53 以上）。
        const auto& a = record.aggregate;
        stmt.bind(index++, a.min);
        stmt.bind(index++, a.max);
        stmt.bind(index++, a.avg);
        stmt.bind(index++, a.last);
        stmt.bind(index++, static_cast<int64_t>(a.count));
    }
    stmt.exec();
}

void insert_mark(const SQLite::Database& db, const HistoryMark& mark) {
    SQLite::Statement stmt(db, mark_insert_sql());
    stmt.bind(1, static_cast<int64_t>(mark.ts));
    stmt.bind(2, mark.kind);
    stmt.bind(3, mark.detail);
    stmt.exec();
}

ScalarRecord read_scalar_row(const SQLite::Statement& stmt) {
    ScalarRecord record;
    record.level = stmt.getColumn(0).getInt();
    record.bucket_ts = stmt.getColumn(1).getInt64();
    record.interval_sec = stmt.getColumn(2).getInt64();
    int column = 3;
    for (auto& metric : record.metrics) {
        // `n = 0` / NULL 表示该桶内指标无有效样本：其余 4 列保持「空聚合」语义，
        // 读取方据此输出 null 而不是 0（与快照「不拿 0 冒充不可用」同一纪律）。
        if (!stmt.getColumn(column + 4).isNull()) {
            metric.min = stmt.getColumn(column + 0).getDouble();
            metric.max = stmt.getColumn(column + 1).getDouble();
            metric.avg = stmt.getColumn(column + 2).getDouble();
            metric.last = stmt.getColumn(column + 3).getDouble();
            metric.count = stmt.getColumn(column + 4).getInt64();
        }
        column += 5;
    }
    return record;
}

int64_t history_file_bytes(const std::filesystem::path& path) {
    std::error_code ec;
    int64_t total = 0;
    const std::filesystem::path candidates[] = {path, std::filesystem::path(path.string() + "-wal"),
                                                std::filesystem::path(path.string() + "-shm")};
    for (const auto& candidate : candidates) {
        const auto size = std::filesystem::file_size(candidate, ec);
        if (!ec) {
            total += static_cast<int64_t>(size);
        }
        ec.clear();
    }
    return total;
}

int read_schema_version(SQLite::Database& db) {
    try {
        SQLite::Statement stmt(db, "SELECT value FROM sysmon_meta WHERE key=?1");
        stmt.bind(1, std::string(kMetaSchemaVersion));
        if (!stmt.executeStep()) {
            return 0;
        }
        return stmt.getColumn(0).getInt();
    } catch (const std::exception&) {
        return 0;  // 表不存在 = 未初始化
    }
}

void ensure_parent_dir(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        throw std::runtime_error("history dir create failed | path=" + parent.string() +
                                 " error=\"" + ec.message() + "\"");
    }
}

}  // namespace dztrader::sysmonstore::internal
