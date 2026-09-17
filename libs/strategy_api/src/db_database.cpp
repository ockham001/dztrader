#include "db_database.h"

#include <string>
#include <utility>
#include <variant>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>
#include <dztrader/tdstore/schema_catalog.h>

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

}  // namespace

std::unique_ptr<DzDatabase> db_open_readonly(const std::string& path) {
    auto handle = std::make_unique<DzDatabase>();
    db::Config config{.backend = "sqlite",
                      .options = {{"path", path}, {"busy_timeout_ms", "5000"}}};
    handle->database = db::Database::open(config, tdstore::schemas());
    handle->session = handle->database->session(/*read_only=*/true);
    return handle;
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
