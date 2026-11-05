#include "db_database.h"

#include <algorithm>
#include <map>
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

/// 全列结果 -> 业务列 + `seq`（排除声明为平台内部的列）。
/// 哪些列不对外**单源**在 `tdstore::ColumnSchema::platform_internal`（经
/// `db::strategy_visible_column_names` 读出），本层不维护第二份清单。
DbQueryResult project_business_columns(std::string_view table,
                                       const db::ResourceSchema& schema,
                                       db::ResultSet all) {
    const std::vector<std::string> visible = db::strategy_visible_column_names(table);
    std::vector<size_t> keep;
    keep.reserve(visible.size());
    DbQueryResult out;
    out.columns.reserve(visible.size());
    for (const std::string& name : visible) {
        const db::FieldSchema* field = nullptr;
        for (const db::FieldSchema& candidate : schema.fields) {
            if (candidate.name == name) {
                field = &candidate;
                break;
            }
        }
        if (field == nullptr) {
            throw Exception(DZ_EC_INTERNAL, "visible column missing from projection | field={}",
                            name);
        }
        out.columns.push_back(ColumnMeta{db_value_type_to_dz(field->type), name});
        if (all.columns().empty()) {
            continue;  // 哨兵空结果（未执行查询）：只回溯列元信息，无行可投影
        }
        const std::optional<size_t> index = all.column_index(field->name);
        if (!index.has_value()) {
            throw Exception(DZ_EC_INTERNAL, "declared column missing from result | field={}",
                            field->name);
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

/// `dz_db_open_default` 的进程级**入口**缓存。
///
/// 只缓存 `db::Database`（无状态入口，文档明确为"线程安全，进程内共享"）；**每次调用
/// 新建 Session**——`Session` 文档要求"一线程一 Session"、非线程安全，且 SQLite 会话
/// 各自持有独立连接。缓存键 = 库路径: `DZTRADER_HOME` 变化（测试换 home）时重开,
/// 避免读到上一个 home 的库。
struct DefaultDbCache {
    std::mutex mutex;
    std::optional<std::string> path;
    std::shared_ptr<db::Database> database;
    uint64_t open_attempts = 0;  ///< 真实打开尝试次数（含失败; 测试断言"初始化未打开"用）
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

/// 每账户锚点日查询：按锚点日**分组**，每组一条
/// `date = <anchor> AND account_id IN (<该锚点日的账户>)` 查询，最后按 `seq` 归并。
///
/// 为什么不"先全表取回再内存过滤"：`orders`/`trades` 是多日追加流，全表取回等于把整段
/// 历史物化后在内存逐行丢弃（违反 7×24 核心路径姿态，且随库龄线性恶化）。锚点日取值一般
/// 只有一两个，查询数 = 不同锚点日数，与账户数无关；条件全部下推到 SQL（走
/// `idx_*_account_date`），只物化真正要返回的行。
///
/// 无任何锚点行（全部账户都没有快照）时返回列元信息为空的哨兵结果，由投影补列。
db::ResultSet find_by_per_account_anchor(
    DzDatabase* db,
    std::string_view table,
    const db::Filter& base_filter,
    const std::unordered_map<std::string, std::string>& anchors) {
    std::map<std::string, std::vector<std::string>> accounts_by_day;
    for (const auto& [account, day] : anchors) {
        accounts_by_day[day].push_back(account);
    }
    std::vector<db::Column> columns;
    std::vector<db::Row> rows;
    for (const auto& [day, accounts] : accounts_by_day) {
        db::Filter filter = base_filter;
        filter.add(db::filters::eq("date", day));
        filter.add(db::filters::in("account_id", accounts));
        const db::ResultSet part = db->session->find(
            table, filter, db::FindOptions{.sort = {{"seq", db::SortOrder::Ascending}}});
        if (columns.empty()) {
            columns.assign(part.columns().begin(), part.columns().end());
        }
        rows.insert(rows.end(), part.rows().begin(), part.rows().end());
    }
    // 分组内已按 seq 升序；跨组归并成全局 seq 升序（C API 既有行序承诺）
    if (const auto seq_index = db::ResultSet(columns, {}).column_index("seq");
        seq_index.has_value()) {
        std::stable_sort(rows.begin(), rows.end(),
                         [&seq_index](const db::Row& lhs, const db::Row& rhs) {
                             const auto* l = std::get_if<int64_t>(&lhs.values()[*seq_index]);
                             const auto* r = std::get_if<int64_t>(&rhs.values()[*seq_index]);
                             if (l == nullptr || r == nullptr) {
                                 return false;
                             }
                             return *l < *r;
                         });
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
    if (date != DZ_DATE_NA) {
        add_date_filter(&filter, date);
        return project_business_columns(
            table, schema,
            db->session->find(table, filter,
                              db::FindOptions{.sort = {{"seq", db::SortOrder::Ascending}}}));
    }

    const std::unordered_map<std::string, std::string> anchors = account_anchor_days(db);
    if (!account_id.empty()) {
        // 指定账户 + 锚点日: 锚点直接下推成 date 等值条件
        const auto anchor = anchors.find(account_id);
        if (anchor == anchors.end()) {
            // 该账户无任何快照行 → 锚点日 null → 空结果（列元信息仍按声明填充）
            return project_business_columns(
                table, schema, db::ResultSet(std::vector<db::Column>(), std::vector<db::Row>()));
        }
        filter.add(db::filters::eq("date", anchor->second));
        return project_business_columns(
            table, schema,
            db->session->find(table, filter,
                              db::FindOptions{.sort = {{"seq", db::SortOrder::Ascending}}}));
    }
    // 每账户各自锚点日
    return project_business_columns(table, schema,
                                    find_by_per_account_anchor(db, table, filter, anchors));
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
    std::shared_ptr<db::Database> database;
    {
        const std::lock_guard<std::mutex> lock(cache.mutex);
        // 懒打开: 首次调用才建入口（SDK 初始化不打开默认库，ADR 0044 决策 3）;
        // 路径变化（换 DZTRADER_HOME）时重开, 不读到上一个 home 的库。
        if (cache.database == nullptr || !cache.path.has_value() || *cache.path != path) {
            cache.database = db::Database::open(
                db::Config{.backend = "sqlite",
                           .options = {{"path", path}, {"busy_timeout_ms", "5000"}}},
                db::td_schemas());
            cache.path = path;
        }
        database = cache.database;
        ++cache.open_attempts;  // 建 Session（真正打开库）的尝试次数, 失败也计入
    }
    // 每次调用独立 Session（一线程一 Session）: 库不存在/不可读在这里抛, 调用方转 LastError
    auto handle = std::make_unique<DzDatabase>();
    handle->database = std::move(database);
    handle->session = handle->database->session(/*read_only=*/true);
    return handle;
}

void reset_default_db_cache_for_test() {
    DefaultDbCache& cache = default_db_cache();
    const std::lock_guard<std::mutex> lock(cache.mutex);
    cache.path.reset();
    cache.database.reset();
    cache.open_attempts = 0;
}

uint64_t default_db_open_attempts_for_test() {
    DefaultDbCache& cache = default_db_cache();
    const std::lock_guard<std::mutex> lock(cache.mutex);
    return cache.open_attempts;
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
