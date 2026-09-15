/**
 * @file database_sqlite.cpp
 * @brief SQLite 后端适配实现
 */
#include <dztrader/db/database_sqlite.h>

#include <type_traits>
#include <utility>

namespace dztrader::db {
namespace {

/// SQLiteCpp 列类型常量 -> 后端无关枚举。
/// SQLite::INTEGER/FLOAT/TEXT/Null/BLOB 是 extern const int 非常量表达式,
/// 只能用比较分派; BLOB 无对应 BindValue, 防御性归 Null (本轮查询均为标量列)。
ColumnType map_column_type(int sqlite_type) {
    if (sqlite_type == SQLite::INTEGER) {
        return ColumnType::Int64;
    }
    if (sqlite_type == SQLite::FLOAT) {
        return ColumnType::Float64;
    }
    if (sqlite_type == SQLite::TEXT) {
        return ColumnType::String;
    }
    return ColumnType::Null;
}

/// BindValue 分派到 SQLiteCpp bind; monostate 走无值重载 (= 绑 NULL)。
void bind_value(SQLite::Statement& stmt, int index, const BindValue& value) {
    std::visit(
        [&stmt, index](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                stmt.bind(index);  // SQLiteCpp 无值重载即绑 NULL
            } else if constexpr (std::is_same_v<T, bool>) {
                stmt.bind(index, static_cast<int>(v));
            } else {
                stmt.bind(index, v);
            }
        },
        value);
}

/// 执行已绑定的 SELECT: 首行决定列元数据 (SQLite 列类型按值动态),
/// 后续行只取值。
QueryResult run_query(SQLite::Statement& stmt) {
    QueryResult result;
    while (stmt.executeStep()) {
        const int column_count = stmt.getColumnCount();
        if (result.columns.empty()) {
            result.columns.reserve(static_cast<size_t>(column_count));
            for (int i = 0; i < column_count; ++i) {
                const ColumnType type = map_column_type(stmt.getColumn(i).getType());
                result.columns.push_back(ColumnMeta{type, stmt.getColumnName(i)});
            }
        }
        Row row;
        row.reserve(static_cast<size_t>(column_count));
        for (int i = 0; i < column_count; ++i) {
            const SQLite::Column col = stmt.getColumn(i);
            const int type = col.getType();
            if (type == SQLite::INTEGER) {
                row.emplace_back(col.getInt64());
            } else if (type == SQLite::FLOAT) {
                row.emplace_back(col.getDouble());
            } else if (type == SQLite::TEXT) {
                row.emplace_back(col.getString());
            } else {
                row.emplace_back(std::monostate{});
            }
        }
        result.rows.push_back(std::move(row));
    }
    return result;
}

/// prepare 返回值: 拥有底层 SQLite::Statement
/// (SqliteStatement 仅持引用, 生命周期由本类保证)。
class OwnedSqliteStatement final : public Statement {
public:
    OwnedSqliteStatement(SQLite::Database& db, std::string_view sql)
        : stmt_(std::make_unique<SQLite::Statement>(db, std::string(sql))), wrapper_(*stmt_) {}

    void bind(int index, const BindValue& value) override { wrapper_.bind(index, value); }
    void execute() override { wrapper_.execute(); }
    void reset() override { wrapper_.reset(); }

private:
    std::unique_ptr<SQLite::Statement> stmt_;
    SqliteStatement wrapper_;
};

QueryResult query_impl(SQLite::Database& db, std::string_view sql,
                       std::span<const BindValue> params) {
    SQLite::Statement stmt(db, std::string(sql));
    for (size_t i = 0; i < params.size(); ++i) {
        bind_value(stmt, static_cast<int>(i + 1), params[i]);
    }
    return run_query(stmt);
}

std::unique_ptr<Statement> prepare_impl(SQLite::Database& db, std::string_view sql) {
    return std::make_unique<OwnedSqliteStatement>(db, sql);
}

std::unique_ptr<Transaction> begin_impl(SQLite::Database& db, bool immediate) {
    return std::make_unique<SqliteTransaction>(db, immediate);
}

}  // namespace

void SqliteStatement::bind(int index, const BindValue& value) { bind_value(stmt_, index, value); }

void SqliteStatement::execute() { stmt_.exec(); }

void SqliteStatement::reset() { stmt_.reset(); }

SqliteTransaction::SqliteTransaction(SQLite::Database& db, bool immediate)
    : txn_(db, immediate ? SQLite::TransactionBehavior::IMMEDIATE
                         : SQLite::TransactionBehavior::DEFERRED) {}

void SqliteTransaction::commit() { txn_.commit(); }

SqliteDatabase::SqliteDatabase(const std::string& path, int flags) : db_(path, flags) {}

void SqliteDatabase::exec(std::string_view sql) { db_.exec(std::string(sql)); }

QueryResult SqliteDatabase::query(std::string_view sql, std::span<const BindValue> params) {
    return query_impl(db_, sql, params);
}

std::unique_ptr<Statement> SqliteDatabase::prepare(std::string_view sql) {
    return prepare_impl(db_, sql);
}

std::unique_ptr<Transaction> SqliteDatabase::begin(bool immediate) {
    return begin_impl(db_, immediate);
}

void SqliteDatabaseRef::exec(std::string_view sql) { db_.exec(std::string(sql)); }

QueryResult SqliteDatabaseRef::query(std::string_view sql, std::span<const BindValue> params) {
    return query_impl(db_, sql, params);
}

std::unique_ptr<Statement> SqliteDatabaseRef::prepare(std::string_view sql) {
    return prepare_impl(db_, sql);
}

std::unique_ptr<Transaction> SqliteDatabaseRef::begin(bool immediate) {
    return begin_impl(db_, immediate);
}

}  // namespace dztrader::db
