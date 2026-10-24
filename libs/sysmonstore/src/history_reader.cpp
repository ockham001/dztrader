#include "history_store_internal.h"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <spdlog/spdlog.h>

#include <format>
#include <stdexcept>
#include <string>
#include <utility>

#include <dztrader/sysmonstore/history_store.h>

namespace dztrader::sysmonstore {

namespace {

/// 只读连接的 PRAGMA：**不**尝试 journal_mode 转换（需写库头，只读连接必然失败）。
/// 其余为连接级设置，逐条尽力，失败吞掉（只读连接上某些 PRAGMA 被禁是正常的）。
void apply_readonly_pragmas(SQLite::Database& db, int busy_timeout_ms) {
    const std::string pragmas[] = {
        std::format("PRAGMA busy_timeout={}", busy_timeout_ms),
        "PRAGMA temp_store=memory",
    };
    for (const auto& pragma : pragmas) {
        try {
            db.exec(pragma);
        } catch (const std::exception&) {
        }
    }
}

}  // namespace

HistoryReader::HistoryReader(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
HistoryReader::HistoryReader(HistoryReader&&) noexcept = default;
HistoryReader& HistoryReader::operator=(HistoryReader&&) noexcept = default;
HistoryReader::~HistoryReader() = default;

std::pair<HistoryAvailability, std::unique_ptr<HistoryReader>> HistoryReader::open(
    const std::filesystem::path& path, int busy_timeout_ms) {
    // 库缺失不是错误：dzsysmon 可能未启用历史（history.enabled=false），
    // 或进程尚未落过盘。读取方据此显示「历史不可用」，不崩溃、不伪造数据。
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {HistoryAvailability::NotFound, nullptr};
    }

    auto impl = std::make_unique<Impl>();
    impl->path = path;
    try {
        impl->db = std::make_unique<SQLite::Database>(path.string(), SQLite::OPEN_READONLY);
    } catch (const std::exception& e) {
        // 文件存在但打不开：权限、损坏、非 SQLite 文件——同样降级，不抛给调用方。
        SPDLOG_WARN("sysmon history read open failed | path={} error=\"{}\"", path.string(),
                    e.what());
        return {HistoryAvailability::OpenFailed, nullptr};
    }
    auto& db = *impl->db;
    apply_readonly_pragmas(db, busy_timeout_ms);

    // 版本校验：与写入方口径一致——版本不符时**必须**降级，不得按旧结构解码
    // （否则读出的列错位，比「历史不可用」危险得多）。
    int version = 0;
    try {
        version = history_store_internal::read_schema_version(db);
    } catch (const std::exception& e) {
        SPDLOG_WARN("sysmon history schema read failed | error=\"{}\"", e.what());
        return {HistoryAvailability::OpenFailed, nullptr};
    }
    if (version == 0) {
        // 空库/未迁移：表结构可能不存在，按「历史不可用」处理（不是版本不符）。
        return {HistoryAvailability::NotFound, nullptr};
    }
    if (version != kSchemaVersion) {
        SPDLOG_WARN("sysmon history schema mismatch | found={} expected={}", version,
                    kSchemaVersion);
        return {HistoryAvailability::SchemaMismatch, nullptr};
    }
    return {HistoryAvailability::Ok,
            std::unique_ptr<HistoryReader>(new HistoryReader(std::move(impl)))};
}

int HistoryReader::schema_version() const {
    return history_store_internal::read_schema_version(*impl_->db);
}

const std::filesystem::path& HistoryReader::path() const noexcept { return impl_->path; }

std::vector<ScalarRecord> HistoryReader::query_scalars(int32_t level,
                                                       int64_t since_ts,
                                                       int64_t until_ts,
                                                       int64_t max_rows) const {
    std::vector<ScalarRecord> rows;
    if (max_rows <= 0 || until_ts <= since_ts) {
        return rows;
    }
    try {
        SQLite::Statement stmt(*impl_->db,
                               "SELECT " + history_store_internal::scalar_select_columns() +
                                   " FROM sysmon_scalar WHERE level=?1 AND bucket_ts>=?2 "
                                   "AND bucket_ts<?3 ORDER BY bucket_ts ASC LIMIT ?4");
        stmt.bind(1, static_cast<int32_t>(level));
        stmt.bind(2, static_cast<int64_t>(since_ts));
        stmt.bind(3, static_cast<int64_t>(until_ts));
        stmt.bind(4, static_cast<int64_t>(max_rows));
        while (stmt.executeStep()) {
            rows.push_back(history_store_internal::read_scalar_row(stmt));
        }
    } catch (const std::exception& e) {
        // 查询失败不抛给 REST 路径：历史不可用是降级条件，不是崩溃条件。
        SPDLOG_WARN("sysmon history scalar query failed | error=\"{}\"", e.what());
        rows.clear();
    }
    return rows;
}

std::vector<DimensionRecord> HistoryReader::query_dimensions(std::string_view domain,
                                                             std::string_view dimension,
                                                             std::string_view metric,
                                                             int32_t level,
                                                             int64_t since_ts,
                                                             int64_t until_ts,
                                                             int64_t max_rows) const {
    std::vector<DimensionRecord> rows;
    if (max_rows <= 0 || until_ts <= since_ts || domain.empty()) {
        return rows;
    }
    // 维度与指标为空表示不限（按域查全部维度 / 全部指标），因此用 `(?='' OR col=?)`
    // 而不是拼 SQL——参数化保持唯一写法，不引入字符串拼装。
    try {
        SQLite::Statement stmt(*impl_->db,
                               "SELECT level,bucket_ts,domain,dimension,metric,min,max,avg,last,n "
                               "FROM sysmon_dimension WHERE level=?1 AND bucket_ts>=?2 "
                               "AND bucket_ts<?3 AND domain=?4 "
                               "AND (?5='' OR dimension=?5) AND (?6='' OR metric=?6) "
                               "ORDER BY bucket_ts ASC, dimension ASC, metric ASC LIMIT ?7");
        stmt.bind(1, static_cast<int32_t>(level));
        stmt.bind(2, static_cast<int64_t>(since_ts));
        stmt.bind(3, static_cast<int64_t>(until_ts));
        stmt.bind(4, std::string(domain));
        stmt.bind(5, std::string(dimension));
        stmt.bind(6, std::string(metric));
        stmt.bind(7, static_cast<int64_t>(max_rows));
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
    } catch (const std::exception& e) {
        SPDLOG_WARN("sysmon history dimension query failed | error=\"{}\"", e.what());
        rows.clear();
    }
    return rows;
}

std::vector<HistoryMark> HistoryReader::query_marks(int64_t since_ts,
                                                    int64_t until_ts,
                                                    int64_t max_rows) const {
    std::vector<HistoryMark> marks;
    if (max_rows <= 0 || until_ts <= since_ts) {
        return marks;
    }
    try {
        SQLite::Statement stmt(*impl_->db,
                               "SELECT ts,kind,detail FROM sysmon_mark WHERE ts>=?1 AND ts<?2 "
                               "ORDER BY ts ASC LIMIT ?3");
        stmt.bind(1, static_cast<int64_t>(since_ts));
        stmt.bind(2, static_cast<int64_t>(until_ts));
        stmt.bind(3, static_cast<int64_t>(max_rows));
        while (stmt.executeStep()) {
            HistoryMark mark;
            mark.ts = stmt.getColumn(0).getInt64();
            mark.kind = stmt.getColumn(1).getString();
            mark.detail = stmt.getColumn(2).getString();
            marks.push_back(std::move(mark));
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("sysmon history mark query failed | error=\"{}\"", e.what());
        marks.clear();
    }
    return marks;
}

}  // namespace dztrader::sysmonstore
