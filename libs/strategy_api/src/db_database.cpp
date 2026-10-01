#include "db_database.h"

#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>
#include <dztrader/tdstore/schema_catalog.h>

namespace dztrader::strategy_api_internal {

namespace {

/// 配置全等 (缓存键): backend + options
bool same_config(const db::Config& lhs, const db::Config& rhs) {
    return lhs.backend == rhs.backend && lhs.options == rhs.options;
}

/// ingest 句柄缓存 + 失败诊断去重 + 失败退避 (进程级; FIX-5/FIX-7/FIX-3/WP3b)
struct IngestDbCache {
    std::mutex mutex;
    std::optional<db::Config> config;         ///< 当前句柄对应的配置
    std::unique_ptr<DzDatabase> handle;       ///< 复用的只读句柄 (唯一一份)
    std::optional<db::Config> failed_config;  ///< 最近打开失败的配置
    bool failure_reported = false;            ///< 该配置的失败已诊断过
    std::optional<std::chrono::steady_clock::time_point> retry_not_before;  ///< 退避截止
    std::chrono::milliseconds backoff = kIngestFailureBackoffMs;  ///< 窗口 (测试可覆盖)
    uint64_t open_attempts = 0;  ///< 真实打开尝试次数 (退避跳过不计; 测试断言用)
};

IngestDbCache& ingest_db_cache() {
    static IngestDbCache cache;
    return cache;
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

}  // namespace

std::unique_ptr<DzDatabase> db_open_readonly_config(const db::Config& config) {
    auto handle = std::make_unique<DzDatabase>();
    handle->database = db::Database::open(config, tdstore::schemas());
    handle->session = handle->database->session(/*read_only=*/true);
    return handle;
}

std::unique_ptr<DzDatabase> db_open_readonly(const std::string& path) {
    return db_open_readonly_config(
        db::Config{.backend = "sqlite",
                   .options = {{"path", path}, {"busy_timeout_ms", "5000"}}});
}

IngestDbAcquire acquire_ingest_db(const db::Config& config) {
    IngestDbCache& cache = ingest_db_cache();
    const std::lock_guard<std::mutex> lock(cache.mutex);

    if (cache.handle != nullptr && cache.config.has_value() && same_config(*cache.config, config)) {
        return IngestDbAcquire{.db = cache.handle.get(), .report = false, .error = {}};
    }

    // WP3b 失败退避: 同一配置刚失败过 → 窗口内直接判"库不可用" (不发起连接、不重复
    // 诊断); 换配置 (failed_config 不同) 立即尝试, 保证测试/部署换目标可隔离。
    const auto now = std::chrono::steady_clock::now();
    if (cache.failed_config.has_value() && same_config(*cache.failed_config, config) &&
        cache.retry_not_before.has_value() && now < *cache.retry_not_before) {
        return IngestDbAcquire{.db = nullptr, .report = false, .error = {}};
    }

    // 配置变化 (或首次): 关旧句柄再按新配置打开, 保证同时只有一份句柄
    cache.handle.reset();
    cache.config.reset();

    ++cache.open_attempts;
    std::string error;
    try {
        cache.handle = db_open_readonly_config(config);
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "unknown exception";
    }
    if (cache.handle != nullptr) {
        cache.config = config;
        // 失败状态按配置隔离: 仅当该配置自己恢复时清除退避与诊断去重,
        // 不因其他配置打开成功而重复报告同一配置的旧失败。
        if (cache.failed_config.has_value() && same_config(*cache.failed_config, config)) {
            cache.failed_config.reset();
            cache.failure_reported = false;
            cache.retry_not_before.reset();
        }
        return IngestDbAcquire{.db = cache.handle.get(), .report = false, .error = {}};
    }

    // 失败不缓存: 窗口过后重新尝试; 同一配置只诊断一次 (避免热路径刷屏)
    IngestDbAcquire out;
    out.error = std::move(error);
    const bool already_reported = cache.failed_config.has_value() &&
                                  same_config(*cache.failed_config, config) &&
                                  cache.failure_reported;
    cache.failed_config = config;
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
    cache.config.reset();
    cache.failed_config.reset();
    cache.failure_reported = false;
    cache.retry_not_before.reset();
    cache.backoff = kIngestFailureBackoffMs;
    cache.open_attempts = 0;
}

uint64_t ingest_open_attempts_for_test() {
    IngestDbCache& cache = ingest_db_cache();
    const std::lock_guard<std::mutex> lock(cache.mutex);
    return cache.open_attempts;
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

DbQueryResult db_query_order_trade(DzDatabase* db, const std::string& account_id,
                                   const std::string& instrument_id, const std::string& table,
                                   bool order_by_seq) {
    db::Filter filter;
    if (!account_id.empty()) filter.add(db::filters::eq("account_id", account_id));
    if (!instrument_id.empty()) filter.add(db::filters::eq("instrument_id", instrument_id));
    db::FindOptions options;
    if (order_by_seq) options.sort = {{"seq", db::SortOrder::Ascending}};
    return to_db_query_result(db->session->find(table, filter, options));
}

DbQueryResult db_query_position(DzDatabase* db, const std::string& account_id,
                                const std::string& instrument_id) {
    db::Filter filter;
    if (!account_id.empty()) filter.add(db::filters::eq("account_id", account_id));
    if (!instrument_id.empty()) filter.add(db::filters::eq("instrument_id", instrument_id));
    return to_db_query_result(db->session->find(
        "positions", filter, db::FindOptions{.sort = {{"seq", db::SortOrder::Ascending}}}));
}

DbQueryResult db_query_trading_account(DzDatabase* db, const std::string& account_id) {
    db::Filter filter;
    if (!account_id.empty()) filter.add(db::filters::eq("account_id", account_id));
    return to_db_query_result(db->session->find(
        "trading_accounts", filter, db::FindOptions{.sort = {{"seq", db::SortOrder::Ascending}}}));
}

DbQueryResult db_query_seq_range(DzDatabase* db, const std::string& resource,
                                 const std::string& account_id, uint64_t from, uint64_t to) {
    const db::Filter filter = {db::filters::eq("account_id", account_id),
                               db::filters::gte("seq", static_cast<int64_t>(from)),
                               db::filters::lt("seq", static_cast<int64_t>(to + 1))};
    return to_db_query_result(db->session->find(
        resource_collection(resource), filter,
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

uint64_t db_query_account_max_seq(DzDatabase* db, const std::string& resource,
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
