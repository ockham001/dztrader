#include <dztrader/sysmonstore/schema.h>

#include "history_store_internal.h"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

namespace dztrader::sysmonstore {

namespace {

std::string_view column_type(ScalarType type, std::string_view suffix) {
    // `avg` 恒为 REAL：整型指标（字节数、计数）的窗口均值几乎必为小数，用 INTEGER
    // 亲和性会把均值截断（SQLite 按列亲和性转换），压缩后的均值就偏了。
    // `n` 恒为 INTEGER（有效样本数）。
    if (suffix == "n") {
        return "INTEGER";
    }
    if (suffix == "avg") {
        return "REAL";
    }
    return type == ScalarType::Real ? "REAL" : "INTEGER";
}

}  // namespace

const ScalarMetricSpec* find_scalar_metric(std::string_view key) noexcept {
    for (const auto& metric : kScalarMetrics) {
        if (metric.key == key) {
            return &metric;
        }
    }
    return nullptr;
}

const ScalarMetricSpec* find_scalar_metric_by_column(std::string_view column) noexcept {
    for (const auto& metric : kScalarMetrics) {
        if (metric.column == column) {
            return &metric;
        }
    }
    return nullptr;
}

const DimensionDomainSpec* find_dimension_domain(std::string_view domain) noexcept {
    for (const auto& spec : kDimensionDomains) {
        if (spec.domain == domain) {
            return &spec;
        }
    }
    return nullptr;
}

bool dimension_domain_has_metric(std::string_view domain, std::string_view metric) noexcept {
    const auto* spec = find_dimension_domain(domain);
    if (spec == nullptr) {
        return false;
    }
    return std::find(spec->metrics.begin(), spec->metrics.end(), metric) != spec->metrics.end();
}

bool is_known_mark_kind(std::string_view kind) noexcept {
    return std::find(kMarkKinds.begin(), kMarkKinds.end(), kind) != kMarkKinds.end();
}

std::string build_schema_ddl() {
    std::string ddl;

    // 元数据：schema 版本与将来可能的迁移水位。读取方先读它再决定是否解码。
    ddl +=
        "CREATE TABLE IF NOT EXISTS sysmon_meta ("
        "    key TEXT PRIMARY KEY,"
        "    value TEXT NOT NULL"
        ");\n";

    // 宽表：一行一个时间桶的全部固定标量指标（每指标 min/max/avg/last/n 五列）。
    ddl +=
        "CREATE TABLE IF NOT EXISTS sysmon_scalar ("
        "    level INTEGER NOT NULL,"
        "    bucket_ts INTEGER NOT NULL,"
        "    interval_sec INTEGER NOT NULL";
    for (const auto& metric : kScalarMetrics) {
        for (const auto suffix : history_store_internal::kAggregateSuffixes) {
            ddl +=
                std::format(", {}_{} {}", metric.column, suffix, column_type(metric.type, suffix));
        }
    }
    // 复合主键：原始记录与分钟聚合同表共存、互不覆盖；按 (level, bucket_ts) 范围扫描即
    // 时间范围查询，无需额外索引。
    ddl += ", PRIMARY KEY (level, bucket_ts)) WITHOUT ROWID;\n";

    // 长表：按维度展开（CPU 核 / 卷 / 物理盘 / 网卡 / 温度传感器）。
    // 维度元数是可变的（核数、盘数、网卡数随机器与热插拔变化），固定列会把表结构
    // 钉死在某一台机器上，故用 (域, 维度, 指标) 三元组展开。
    ddl +=
        "CREATE TABLE IF NOT EXISTS sysmon_dimension ("
        "    level INTEGER NOT NULL,"
        "    bucket_ts INTEGER NOT NULL,"
        "    domain TEXT NOT NULL,"
        "    dimension TEXT NOT NULL,"
        "    metric TEXT NOT NULL,"
        "    min REAL,"
        "    max REAL,"
        "    avg REAL,"
        "    last REAL,"
        // `n` 可空：`n IS NULL` 表示该桶内该维度指标无有效样本（读取方输出「不可用」
        // 而不是 0）。写 NOT NULL 会让「无有效样本」的写入路径直接违反约束失败。
        "    n INTEGER,"
        "    PRIMARY KEY (level, bucket_ts, domain, dimension, metric)"
        ") WITHOUT ROWID;\n"
        // 按域 + 维度查时间序列（#79 的「按指标/时间范围/粒度查询」走这条索引）。
        "CREATE INDEX IF NOT EXISTS idx_sysmon_dimension_series "
        "ON sysmon_dimension(domain, dimension, metric, level, bucket_ts);\n";

    // 标记：采集中断 / 进程重启 / 时钟跳变。与数据分表存放，避免「有标记的桶」
    // 被误当成正常数据；读取方按 ts 范围取标记，独立于数据桶对齐。
    ddl +=
        "CREATE TABLE IF NOT EXISTS sysmon_mark ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    ts INTEGER NOT NULL,"
        "    kind TEXT NOT NULL,"
        "    detail TEXT NOT NULL DEFAULT ''"
        ");\n"
        "CREATE INDEX IF NOT EXISTS idx_sysmon_mark_ts ON sysmon_mark(ts);\n";

    return ddl;
}

}  // namespace dztrader::sysmonstore
