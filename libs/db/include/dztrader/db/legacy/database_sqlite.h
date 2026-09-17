/**
 * @file database_sqlite.h
 * @brief SQLite 适配器 (拥有型 / 引用包装型)
 */
#ifndef DZTRADER_DB_LEGACY_DATABASE_SQLITE_H_
#define DZTRADER_DB_LEGACY_DATABASE_SQLITE_H_

#include <dztrader/db/legacy/database.h>
#include <dztrader/db/sqlite.h>

namespace dztrader::db::legacy {

class SqliteStatement final : public Statement {
public:
    explicit SqliteStatement(SQLite::Statement& stmt) : stmt_(stmt) {}
    void bind(int index, const BindValue& value) override;
    void execute() override;
    void reset() override;

private:
    SQLite::Statement& stmt_;
};

class SqliteTransaction final : public Transaction {
public:
    explicit SqliteTransaction(SQLite::Database& db, bool immediate);
    void commit() override;

private:
    SQLite::Transaction txn_;
};

class SqliteDatabase final : public Database {
public:
    explicit SqliteDatabase(const std::string& path, int flags);
    void exec(std::string_view sql) override;
    QueryResult query(std::string_view sql, std::span<const BindValue> params = {}) override;
    std::unique_ptr<Statement> prepare(std::string_view sql) override;
    std::unique_ptr<Transaction> begin(bool immediate = false) override;

private:
    SQLite::Database db_;
};

/// 包装既有连接 (不拥有; PersistWriter 过渡期共用同一连接)
class SqliteDatabaseRef final : public Database {
public:
    explicit SqliteDatabaseRef(SQLite::Database& db) : db_(db) {}
    void exec(std::string_view sql) override;
    QueryResult query(std::string_view sql, std::span<const BindValue> params = {}) override;
    std::unique_ptr<Statement> prepare(std::string_view sql) override;
    std::unique_ptr<Transaction> begin(bool immediate = false) override;

private:
    SQLite::Database& db_;
};

}  // namespace dztrader::db::legacy

#endif  // DZTRADER_DB_LEGACY_DATABASE_SQLITE_H_
