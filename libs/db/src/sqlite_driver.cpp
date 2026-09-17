#include "sqlite_driver.h"

#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include <SQLiteCpp/Statement.h>
#include <spdlog/spdlog.h>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>

#include "sqlite_migrations.h"
#include "sqlite_sql.h"

namespace dztrader::db {

namespace {

std::string option_or(const std::map<std::string, std::string, std::less<>>& options,
                      std::string_view key, std::string fallback) {
    const auto it = options.find(key);
    return it == options.end() ? std::move(fallback) : it->second;
}

/// 应用连接级 PRAGMA（迁移连接与 Session 连接共用）
void apply_pragmas(SQLite::Database& db,
                   const std::map<std::string, std::string, std::less<>>& options) {
    db.exec("PRAGMA synchronous=" + option_or(options, "synchronous", "full"));
    const std::string journal_mode = option_or(options, "journal_mode", "wal");
    bool wal_ready = false;
    for (int attempt = 0; attempt < 3 && !wal_ready; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        try {
            wal_ready =
                db.execAndGet("PRAGMA journal_mode=" + journal_mode).getString() == journal_mode;
        } catch (const std::exception&) {
            // 独占锁竞争: 有界重试
        }
    }
    if (!wal_ready) {
        SPDLOG_WARN("td db WAL conversion not applied, continue in current mode");
    }
    db.exec("PRAGMA busy_timeout=" + option_or(options, "busy_timeout_ms", "5000"));
    db.exec("PRAGMA cache_size=-" + option_or(options, "cache_size_kb", "8000"));
    db.exec("PRAGMA temp_store=" + option_or(options, "temp_store", "memory"));
}

/// 将 Value 绑定到 1-based 参数位；monostate 绑定 NULL
void bind_value(SQLite::Statement& stmt, int index, const Value& value) {
    std::visit(
        [&stmt, index](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                stmt.bind(index);  // SQLiteCpp 无值重载 = NULL
            } else if constexpr (std::is_same_v<T, bool>) {
                stmt.bind(index, static_cast<int>(v));
            } else {
                stmt.bind(index, v);
            }
        },
        value);
}

std::string_view aggregate_column_name(AggregateOp op) {
    switch (op) {
        case AggregateOp::Count: return "count";
        case AggregateOp::Max: return "max";
        case AggregateOp::Min: return "min";
        case AggregateOp::Sum: return "sum";
        case AggregateOp::Avg: return "avg";
    }
    return "count";
}

}  // namespace

SqliteDatabaseImpl::SqliteDatabaseImpl(
    std::string path, std::map<std::string, std::string, std::less<>> options,
    std::span<const ResourceSchema> schemas)
    : path_(std::move(path)), options_(std::move(options)), schemas_(schemas.begin(), schemas.end()) {
    for (const ResourceSchema& schema : schemas_) {
        schema_by_name_.emplace(schema.name, &schema);
    }
}

const ResourceSchema* SqliteDatabaseImpl::find_schema(std::string_view name) const {
    const auto it = schema_by_name_.find(name);
    return it == schema_by_name_.end() ? nullptr : it->second;
}

Capability SqliteDatabaseImpl::capabilities() const noexcept {
    return Capability::Transactions | Capability::SnapshotRead | Capability::Upsert |
           Capability::OrderedRangeScan;
}

std::unique_ptr<Session> SqliteDatabaseImpl::session(bool read_only) {
    try {
        const int flags = read_only ? SQLite::OPEN_READONLY
                                    : (SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        auto db = std::make_unique<SQLite::Database>(path_, flags);
        apply_pragmas(*db, options_);
        return std::make_unique<SqliteSession>(this, std::move(db), read_only);
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_DB_OPEN_FAILED, "sqlite session open failed: {}", e.what());
    }
}

void SqliteDatabaseImpl::migrate() {
    try {
        SQLite::Database db(path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        apply_pragmas(db, options_);
        internal::apply_td_migrations(db);
        internal::create_missing_collections(db, schemas_);
    } catch (const Exception&) {
        throw;
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_DB_MIGRATE_FAILED, "sqlite migrate failed: {}", e.what());
    }
}

SqliteSession::SqliteSession(const SqliteDatabaseImpl* owner, std::unique_ptr<SQLite::Database> db,
                             bool read_only)
    : owner_(owner), db_(std::move(db)), read_only_(read_only) {}

const ResourceSchema& SqliteSession::require_schema(std::string_view collection) const {
    const ResourceSchema* schema = owner_->find_schema(collection);
    if (schema == nullptr) {
        throw Exception(DZ_EC_DB_QUERY_FAILED, "collection not registered | collection={}",
                        collection);
    }
    return *schema;
}

Value SqliteSession::read_column(const SQLite::Column& column, ValueType type) {
    if (column.isNull()) {
        return std::monostate{};
    }
    switch (type) {
        case ValueType::Bool:
            return column.getInt() != 0;
        case ValueType::Int64:
            return static_cast<int64_t>(column.getInt64());
        case ValueType::Float64:
            return column.getType() == SQLite::INTEGER ? static_cast<double>(column.getInt64())
                                                       : column.getDouble();
        case ValueType::String:
            return column.getString();
        case ValueType::Null:
            return std::monostate{};
    }
    return std::monostate{};
}

void SqliteSession::begin_scope(Scope kind) {
    if (scope_ != Scope::None) {
        throw Exception(DZ_EC_DB_TRANSACTION_FAILED, "scope already active");
    }
    db_->exec(kind == Scope::Transaction ? "BEGIN IMMEDIATE" : "BEGIN DEFERRED");
    scope_ = kind;
}

void SqliteSession::end_scope(bool commit) {
    if (scope_ == Scope::None) {
        return;
    }
    const Scope kind = scope_;
    if (commit || kind == Scope::Snapshot) {
        db_->exec("COMMIT");
        scope_ = Scope::None;
        return;
    }
    try {
        db_->exec("ROLLBACK");  // 失败路径: 不抛 (调用方已在异常处理中)
    } catch (...) {
    }
    scope_ = Scope::None;
}

void SqliteSession::upsert(std::string_view collection, std::span<const Row> rows) {
    if (read_only_) {
        throw Exception(DZ_EC_PERMISSION_DENIED, "session is read-only");
    }
    if (scope_ == Scope::Snapshot) {
        throw Exception(DZ_EC_PERMISSION_DENIED, "upsert inside snapshot is not allowed");
    }
    if (rows.empty()) {
        return;
    }
    const ResourceSchema& schema = require_schema(collection);
    const bool owned_scope = scope_ == Scope::None;
    try {
        if (owned_scope) {
            begin_scope(Scope::Transaction);  // BEGIN IMMEDIATE
        }
        SQLite::Statement stmt(*db_, internal::build_upsert_sql(schema));
        for (const Row& row : rows) {
            if (row.size() != schema.fields.size()) {
                throw Exception(DZ_EC_DB_WRITE_FAILED, "row field count mismatch: collection={}",
                                std::string(collection));
            }
            for (size_t i = 0; i < row.size(); ++i) {
                bind_value(stmt, static_cast<int>(i + 1), row.values()[i]);
            }
            stmt.exec();
            stmt.reset();
        }
        if (owned_scope) {
            end_scope(/*commit=*/true);
        }
    } catch (const Exception&) {
        if (owned_scope) {
            end_scope(/*commit=*/false);
        }
        throw;
    } catch (const std::exception& e) {
        if (owned_scope) {
            end_scope(/*commit=*/false);
        }
        throw Exception(DZ_EC_DB_WRITE_FAILED, "sqlite upsert failed: {}", e.what());
    }
}

uint64_t SqliteSession::remove(std::string_view collection, const Filter& filter) {
    if (read_only_) {
        throw Exception(DZ_EC_PERMISSION_DENIED, "session is read-only");
    }
    if (scope_ == Scope::Snapshot) {
        throw Exception(DZ_EC_PERMISSION_DENIED, "remove inside snapshot is not allowed");
    }
    const ResourceSchema& schema = require_schema(collection);
    const internal::CompiledFilter compiled = internal::compile_filter(filter.conditions(), schema);
    try {
        SQLite::Statement stmt(*db_, internal::build_delete_sql(schema, compiled));
        for (size_t i = 0; i < compiled.params.size(); ++i) {
            bind_value(stmt, static_cast<int>(i + 1), compiled.params[i]);
        }
        return static_cast<uint64_t>(stmt.exec());
    } catch (const Exception&) {
        throw;
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_DB_WRITE_FAILED, "sqlite remove failed: {}", e.what());
    }
}

ResultSet SqliteSession::find(std::string_view collection, const Filter& filter,
                              const FindOptions& options) {
    const ResourceSchema& schema = require_schema(collection);
    const internal::CompiledFilter compiled = internal::compile_filter(filter.conditions(), schema);
    std::vector<Column> columns;
    columns.reserve(schema.fields.size());
    for (const FieldSchema& field : schema.fields) {
        columns.push_back({std::string(field.name), field.type});
    }
    try {
        SQLite::Statement stmt(
            *db_, internal::build_select_sql(schema, compiled, options.sort, options.offset,
                                             options.limit));
        for (size_t i = 0; i < compiled.params.size(); ++i) {
            bind_value(stmt, static_cast<int>(i + 1), compiled.params[i]);
        }
        std::vector<Row> rows;
        while (stmt.executeStep()) {
            std::vector<Value> values;
            values.reserve(schema.fields.size());
            for (size_t i = 0; i < schema.fields.size(); ++i) {
                values.push_back(
                    read_column(stmt.getColumn(static_cast<int>(i)), schema.fields[i].type));
            }
            rows.emplace_back(std::move(values));
        }
        return ResultSet(std::move(columns), std::move(rows));
    } catch (const Exception&) {
        throw;
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_DB_QUERY_FAILED, "sqlite find failed: {}", e.what());
    }
}

ResultSet SqliteSession::aggregate(std::string_view collection, const Aggregation& aggregation) {
    const ResourceSchema& schema = require_schema(collection);
    const internal::CompiledFilter compiled =
        internal::compile_filter(aggregation.filter.conditions(), schema);
    std::vector<Column> columns;
    std::vector<ValueType> value_types;
    columns.reserve(aggregation.group_by.size() + 1);
    value_types.reserve(aggregation.group_by.size() + 1);
    for (const std::string& field_name : aggregation.group_by) {
        const FieldSchema* field = internal::find_field(schema, field_name);
        if (field == nullptr) {
            throw Exception(DZ_EC_DB_QUERY_FAILED, "unknown group field | field={} collection={}",
                            field_name, schema.name);
        }
        columns.push_back({std::string(field->name), field->type});
        value_types.push_back(field->type);
    }
    ValueType aggregate_type = ValueType::Int64;
    if (aggregation.op != AggregateOp::Count) {
        const FieldSchema* field = internal::find_field(schema, aggregation.field);
        if (field == nullptr) {
            throw Exception(DZ_EC_DB_QUERY_FAILED,
                            "unknown aggregate field | field={} collection={}", aggregation.field,
                            schema.name);
        }
        aggregate_type = aggregation.op == AggregateOp::Avg ? ValueType::Float64 : field->type;
    }
    columns.push_back({std::string(aggregate_column_name(aggregation.op)), aggregate_type});
    value_types.push_back(aggregate_type);
    try {
        SQLite::Statement stmt(*db_, internal::build_aggregate_sql(
                                         schema, aggregation.op, aggregation.field,
                                         aggregation.group_by, compiled, aggregation.sort));
        for (size_t i = 0; i < compiled.params.size(); ++i) {
            bind_value(stmt, static_cast<int>(i + 1), compiled.params[i]);
        }
        std::vector<Row> rows;
        while (stmt.executeStep()) {
            std::vector<Value> values;
            values.reserve(value_types.size());
            for (size_t i = 0; i < value_types.size(); ++i) {
                values.push_back(read_column(stmt.getColumn(static_cast<int>(i)), value_types[i]));
            }
            rows.emplace_back(std::move(values));
        }
        return ResultSet(std::move(columns), std::move(rows));
    } catch (const Exception&) {
        throw;
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_DB_QUERY_FAILED, "sqlite aggregate failed: {}", e.what());
    }
}

std::unique_ptr<Transaction> SqliteSession::begin_transaction() {
    // Task 6 实现
    throw Exception(DZ_EC_DB_TRANSACTION_FAILED, "sqlite transaction not implemented yet");
}

std::unique_ptr<Snapshot> SqliteSession::begin_snapshot() {
    // Task 6 实现
    throw Exception(DZ_EC_DB_UNSUPPORTED_CAPABILITY, "sqlite snapshot not implemented yet");
}

std::unique_ptr<Database> Database::open(const Config& config,
                                         std::span<const ResourceSchema> schemas) {
    if (config.backend == "sqlite") {
        const auto it = config.options.find("path");
        if (it == config.options.end() || it->second.empty()) {
            throw Exception(DZ_EC_DB_OPEN_FAILED, "sqlite path option is required");
        }
        return std::make_unique<SqliteDatabaseImpl>(it->second, config.options, schemas);
    }
    throw Exception(DZ_EC_DB_UNSUPPORTED_BACKEND, "unsupported backend: backend={}", config.backend);
}

}  // namespace dztrader::db
