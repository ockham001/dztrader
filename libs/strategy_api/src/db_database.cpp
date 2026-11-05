#include "db_database.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <dztrader/core/exception.h>
#include <dztrader/core/path.h>
#include <dztrader/error.h>

#include "td_schema.h"
#include "trading_day.h"

namespace dztrader::strategy_api_internal {

namespace {

/// 诊断/过滤用的平台内部列：不进 C API 业务列投影（ADR 0044 决策 2）。
/// 列名本身仍来自 `tdstore::schema_catalog.h`（单源），本表只声明"哪些不对外"。
constexpr std::string_view kPlatformInternalColumns[] = {"gateway", "ext"};

bool is_platform_internal_column(std::string_view name) {
    return std::any_of(std::begin(kPlatformInternalColumns), std::end(kPlatformInternalColumns),
                       [name](std::string_view internal) { return internal == name; });
}

DzColumnType db_value_type_to_dz(db::ValueType type) {
    switch (type) {
        case db::ValueType::Bool:
            return DZ_COL_TYPE_BOOL;
        case db::ValueType::Int64:
            return DZ_COL_TYPE_INT64;
        case db::ValueType::Float64:
            return DZ_COL_TYPE_FLOAT64;
        case db::ValueType::String:
            return DZ_COL_TYPE_STRING;
        case db::ValueType::Null:
            return DZ_COL_TYPE_NULL;
    }
    return DZ_COL_TYPE_NULL;
}

/// 水位/回补 resource 名 -> collection 名（C API 既有词表）
const char* resource_collection(const std::string& resource) {
    if (resource == "order") return "orders";
    if (resource == "trade") return "trades";
    if (resource == "position") return "positions";
    if (resource == "trading_account") return "trading_accounts";
    throw Exception(DZ_EC_INVALID_PARAM, "unknown resource: resource={}", resource);
}

/// 按 collection 名取 td schema 声明（声明单源 = `tdstore::schema_catalog.h` 的投影）
const db::ResourceSchema& require_td_schema(std::string_view table) {
    for (const db::ResourceSchema& schema : db::td_schemas()) {
        if (schema.name == table) {
            return schema;
        }
    }
    throw Exception(DZ_EC_INVALID_PARAM, "unknown table: table={}", table);
}

/// 全列结果 -> 业务列 + seq（跳过平台内部列）；列名/列序仍来自 schema 声明
DbQueryResult project_business_columns(const db::ResourceSchema& schema, db::ResultSet all) {
    std::vector<size_t> keep;
    keep.reserve(schema.fields.size());
    DbQueryResult out;
    out.columns.reserve(schema.fields.size());
    for (const db::FieldSchema& field : schema.fields) {
        if (is_platform_internal_column(field.name)) {
            continue;
        }
        out.columns.push_back(ColumnMeta{db_value_type_to_dz(field.type), std::string(field.name)});
        if (all.columns().empty()) {
            continue;  // 哨兵空结果（未执行查询）：只回溯列元信息，无行可投影
        }
        const std::optional<size_t> index = all.column_index(field.name);
        if (!index.has_value()) {
            throw Exception(DZ_EC_INTERNAL, "declared column missing from result | field={}",
                            field.name);
        }
        keep.push_back(*index);
    }
    if (keep.empty()) {
        return out;
    }
    out.rows.reserve(all.rows().size());
    for (const db::Row& row : all.rows()) {
        std::vector<ColumnValue> values;
        values.reserve(keep.size());
        for (const size_t index : keep) {
            values.push_back(row.values()[index]);
        }
        out.rows.emplace_back(std::move(values));
    }
    return out;
}

/// ingest 句柄缓存 + 失败诊断去重 + 失败退避（进程级；缓存键 = 库路径）
struct IngestDbCache {
    std::mutex mutex;
    std::optional<std::string> path;  ///< 当前句柄对应的库路径
    std::unique_ptr<DzDatabase> handle;
    std::optional<std::string> failed_path;  ///< 最近打开失败的库路径
    bool failure_reported = false;
    std::optional<std::chrono::steady_clock::time_point> retry_not_before;
    std::chrono::milliseconds backoff = kIngestFailureBackoffMs;
    uint64_t open_attempts = 0;
};

IngestDbCache& ingest_db_cache() {
    static IngestDbCache cache;
    return cache;
}

/// `dz_db_open_default` 的进程级**连接**缓存（句柄按次新建、共享该连接）
/// 缓存键 = 库路径: `DZTRADER_HOME` 变化（测试换 home）时自动重开, 避免读到上一个 home。
struct DefaultDbCache {
    std::mutex mutex;
    std::optional<std::string> path;
    std::shared_ptr<db::Database> database;
    std::shared_ptr<db::Session> session;
};

DefaultDbCache& default_db_cache() {
    static DefaultDbCache cache;
    return cache;
}

/// 只用 schema 业务键做过滤（`date` 列是 TEXT `YYYYMMDD`，与锚点日同表示）
void add_date_filter(db::Filter* filter, DzDate date) {
    const std::string day = format_trading_day(date);
    if (day.empty()) {
        throw Exception(DZ_EC_INVALID_PARAM, "invalid trading day: date={}", date);
    }
    filter->add(db::filters::eq("date", day));
}

/// 每账户锚点日过滤：行序保持；无锚点行的账户被丢弃（锚点日 = null）
db::ResultSet filter_rows_by_anchor(DzDatabase* db, db::ResultSet all) {
    const std::optional<size_t> account_index = all.column_index("account_id");
    const std::optional<size_t> date_index = all.column_index("date");
    if (!account_index.has_value() || !date_index.has_value()) {
        throw Exception(DZ_EC_INTERNAL, "result lacks account_id/date column");
    }
    const std::unordered_map<std::string, std::string> anchors = account_anchor_days(db);
    std::vector<db::Column> columns(all.columns().begin(), all.columns().end());
    std::vector<db::Row> rows;
    rows.reserve(all.rows().size());
    for (const db::Row& row : all.rows()) {
        const auto* account = std::get_if<std::string>(&row.values()[*account_index]);
        const auto* day = std::get_if<std::string>(&row.values()[*date_index]);
        if (account == nullptr || day == nullptr) {
            continue;
        }
        const auto anchor = anchors.find(*account);
        if (anchor != anchors.end() && anchor->second == *day) {
            rows.push_back(row);
        }
    }
    return db::ResultSet(std::move(columns), std::move(rows));
}

/// 四种 C API 查询的公共实现：条件 + 交易日 + 业务列投影
DbQueryResult db_query_by_day(DzDatabase* db,
                              const std::string& table,
                              const std::string& account_id,
                              const std::string& instrument_id,
                              DzDate date) {
    const db::ResourceSchema& schema = require_td_schema(table);
    db::Filter filter;
    if (!account_id.empty()) {
        filter.add(db::filters::eq("account_id", account_id));
    }
    if (!instrument_id.empty()) {
        filter.add(db::filters::eq("instrument_id", instrument_id));
    }
    const bool anchor_day = date == DZ_DATE_NA;
    // 指定账户 + 锚点日: 锚点可直接下推成 date 等值条件（无需全表过滤）
    bool per_account_anchor = false;
    if (anchor_day) {
        if (!account_id.empty()) {
            const std::unordered_map<std::string, std::string> anchors = account_anchor_days(db);
            const auto anchor = anchors.find(account_id);
            if (anchor == anchors.end()) {
                // 该账户无任何快照行 → 锚点日 null → 空结果（列元信息仍按声明填充）
                return project_business_columns(
                    schema, db::ResultSet(std::vector<db::Column>(), std::vector<db::Row>()));
            }
            filter.add(db::filters::eq("date", anchor->second));
        } else {
            per_account_anchor = true;  // 每账户各自锚点日: 全量取回后在内存按锚点过滤
        }
    } else {
        add_date_filter(&filter, date);
    }
    db::ResultSet all = db->session->find(
        table, filter, db::FindOptions{.sort = {{"seq", db::SortOrder::Ascending}}});
    if (per_account_anchor) {
        all = filter_rows_by_anchor(db, std::move(all));
    }
    return project_business_columns(schema, std::move(all));
}

}  // namespace

std::string trade_db_path() { return paths::trade_db().string(); }

std::unique_ptr<DzDatabase> db_open_readonly(const std::string& path) {
    std::shared_ptr<db::Database> database = db::Database::open(
        db::Config{.backend = "sqlite", .options = {{"path", path}, {"busy_timeout_ms", "5000"}}},
        db::td_schemas());
    std::shared_ptr<db::Session> session = database->session(/*read_only=*/true);
    auto handle = std::make_unique<DzDatabase>();
    handle->database = std::move(database);
    handle->session = std::move(session);
    return handle;
}

std::unique_ptr<DzDatabase> default_db_handle() {
    DefaultDbCache& cache = default_db_cache();
    const std::string path = trade_db_path();
    const std::lock_guard<std::mutex> lock(cache.mutex);
    // 懒打开: 首次调用才建连接（SDK 初始化不打开任何 DB，ADR 0044 决策 3）;
    // 路径变化（换 DZTRADER_HOME）时重开, 不读到上一个 home 的库。
    if (cache.database == nullptr || !cache.path.has_value() || *cache.path != path) {
        std::unique_ptr<DzDatabase> opened = db_open_readonly(path);
        cache.path = path;
        cache.database = opened->database;
        cache.session = opened->session;
    }
    auto handle = std::make_unique<DzDatabase>();
    handle->database = cache.database;
    handle->session = cache.session;
    return handle;
}

void reset_default_db_cache_for_test() {
    DefaultDbCache& cache = default_db_cache();
    const std::lock_guard<std::mutex> lock(cache.mutex);
    cache.path.reset();
    cache.database.reset();
    cache.session.reset();
}

IngestDbAcquire acquire_ingest_db(const std::string& path) {
    IngestDbCache& cache = ingest_db_cache();
    const std::lock_guard<std::mutex> lock(cache.mutex);

    if (cache.handle != nullptr && cache.path.has_value() && *cache.path == path) {
        return IngestDbAcquire{.db = cache.handle.get(), .report = false, .error = {}};
    }

    // WP3b 失败退避: 同一路径刚失败过 → 窗口内直接判"库不可用" (不发起打开、不重复
    // 诊断); 换路径立即尝试, 保证测试/部署换目标可隔离。
    const auto now = std::chrono::steady_clock::now();
    if (cache.failed_path.has_value() && *cache.failed_path == path &&
        cache.retry_not_before.has_value() && now < *cache.retry_not_before) {
        return IngestDbAcquire{.db = nullptr, .report = false, .error = {}};
    }

    // 路径变化 (或首次): 关旧句柄再打开新路径, 保证同时只有一份句柄
    cache.handle.reset();
    cache.path.reset();

    ++cache.open_attempts;
    std::string error;
    try {
        cache.handle = db_open_readonly(path);
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "unknown exception";
    }
    if (cache.handle != nullptr) {
        cache.path = path;
        // 失败状态按路径隔离: 仅当该路径自己恢复时清除退避与诊断去重,
        // 不因其他路径打开成功而重复报告同一路径的旧失败。
        if (cache.failed_path.has_value() && *cache.failed_path == path) {
            cache.failed_path.reset();
            cache.failure_reported = false;
            cache.retry_not_before.reset();
        }
        return IngestDbAcquire{.db = cache.handle.get(), .report = false, .error = {}};
    }

    // 失败不缓存: 窗口过后重新尝试; 同一路径只诊断一次 (避免热路径刷屏)
    IngestDbAcquire out;
    out.error = std::move(error);
    const bool already_reported =
        cache.failed_path.has_value() && *cache.failed_path == path && cache.failure_reported;
    cache.failed_path = path;
    if (!already_reported) {
        cache.failure_reported = true;
        out.report = true;
    }
    cache.retry_not_before = std::chrono::steady_clock::now() + cache.backoff;
    return out;
}

void set_ingest_failure_backoff_for_test(std::chrono::milliseconds backoff) {
    IngestDbCache& cache = ingest_db_cache();
    const std::lock_guard<std::mutex> lock(cache.mutex);
    cache.backoff = backoff;
}

void reset_ingest_cache_for_test() {
    IngestDbCache& cache = ingest_db_cache();
    const std::lock_guard<std::mutex> lock(cache.mutex);
    cache.handle.reset();
    cache.path.reset();
    cache.failed_path.reset();
    cache.failure_reported = false;
    cache.retry_not_before.reset();
    cache.backoff = kIngestFailureBackoffMs;
    cache.open_attempts = 0;
}

void expire_ingest_backoff_for_test() {
    IngestDbCache& cache = ingest_db_cache();
    const std::lock_guard<std::mutex> lock(cache.mutex);
    cache.retry_not_before.reset();
}

uint64_t ingest_open_attempts_for_test() {
    IngestDbCache& cache = ingest_db_cache();
    const std::lock_guard<std::mutex> lock(cache.mutex);
    return cache.open_attempts;
}

std::unordered_map<std::string, std::string> account_anchor_days(DzDatabase* db) {
    std::unordered_map<std::string, std::string> anchors;
    // 锚点日 = 两张**快照表**的 MAX(date)（ADR 0046）：orders/trades 是追加流，登录重放
    // 会先于持仓查询落库，把它们算进锚点会让"持仓查询失败当天"的锚点误推进到当日。
    for (const char* table : {"positions", "trading_accounts"}) {
        db::Aggregation aggregation;
        aggregation.op = db::AggregateOp::Max;
        aggregation.field = "date";
        aggregation.group_by = {"account_id"};
        const db::ResultSet result = db->session->aggregate(table, aggregation);
        for (const db::Row& row : result.rows()) {
            if (row.size() != 2 || std::holds_alternative<std::monostate>(row.values()[1])) {
                continue;
            }
            const std::string account = row.get<std::string>(0);
            const std::string day = row.get<std::string>(1);
            if (account.empty() || day.empty()) {
                continue;
            }
            const auto it = anchors.find(account);
            if (it == anchors.end() || day > it->second) {
                anchors[account] = day;
            }
        }
    }
    return anchors;
}

DbQueryResult to_db_query_result(db::ResultSet result) {
    DbQueryResult out;
    out.columns.reserve(result.columns().size());
    for (const auto& column : result.columns()) {
        out.columns.push_back(ColumnMeta{db_value_type_to_dz(column.type), column.name});
    }
    out.rows.reserve(result.rows().size());
    for (const auto& row : result.rows()) {
        out.rows.emplace_back(row.values().begin(), row.values().end());
    }
    return out;
}

DbQueryResult db_query_order_trade(DzDatabase* db,
                                   const std::string& account_id,
                                   const std::string& instrument_id,
                                   const std::string& table,
                                   DzDate date) {
    return db_query_by_day(db, table, account_id, instrument_id, date);
}

DbQueryResult db_query_position(DzDatabase* db,
                                const std::string& account_id,
                                const std::string& instrument_id,
                                DzDate date) {
    return db_query_by_day(db, "positions", account_id, instrument_id, date);
}

DbQueryResult db_query_trading_account(DzDatabase* db, const std::string& account_id, DzDate date) {
    return db_query_by_day(db, "trading_accounts", account_id, "", date);
}

DbQueryResult db_query_seq_range(DzDatabase* db,
                                 const std::string& resource,
                                 const std::string& account_id,
                                 uint64_t from,
                                 uint64_t to) {
    const db::Filter filter = {db::filters::eq("account_id", account_id),
                               db::filters::gte("seq", static_cast<int64_t>(from)),
                               db::filters::lt("seq", static_cast<int64_t>(to + 1))};
    return to_db_query_result(
        db->session->find(resource_collection(resource), filter,
                          db::FindOptions{.sort = {{"seq", db::SortOrder::Ascending}}}));
}

std::unordered_map<std::string, uint64_t> db_query_max_seq_by_account(DzDatabase* db,
                                                                      const std::string& resource) {
    db::Aggregation aggregation;
    aggregation.op = db::AggregateOp::Max;
    aggregation.field = "seq";
    aggregation.group_by = {"account_id"};
    const db::ResultSet result = db->session->aggregate(resource_collection(resource), aggregation);
    std::unordered_map<std::string, uint64_t> out;
    for (const auto& row : result.rows()) {
        if (row.size() != 2 || std::holds_alternative<std::monostate>(row.values()[1])) continue;
        const std::string account = row.get<std::string>(0);
        if (!account.empty()) {
            out[account] = static_cast<uint64_t>(row.get<int64_t>(1));
        }
    }
    return out;
}

uint64_t db_query_account_max_seq(DzDatabase* db,
                                  const std::string& resource,
                                  const std::string& account_id) {
    db::Aggregation aggregation;
    aggregation.op = db::AggregateOp::Max;
    aggregation.field = "seq";
    aggregation.filter = db::filters::eq("account_id", account_id);
    const db::ResultSet result = db->session->aggregate(resource_collection(resource), aggregation);
    if (result.empty() || result.rows().front().size() == 0) return 0;
    const auto& value = result.rows().front().values()[0];
    return std::holds_alternative<std::monostate>(value)
               ? 0
               : static_cast<uint64_t>(std::get<int64_t>(value));
}

}  // namespace dztrader::strategy_api_internal
