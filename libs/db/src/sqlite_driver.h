/**
 * @file sqlite_driver.h
 * @brief SQLite 驱动（Database/Session/Transaction/Snapshot 实现）
 */
#ifndef DZTRADER_DB_SRC_SQLITE_DRIVER_H_
#define DZTRADER_DB_SRC_SQLITE_DRIVER_H_

#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <SQLiteCpp/Database.h>

#include <dztrader/db/database.h>

namespace dztrader::db {

class SqliteDatabaseImpl final : public Database {
public:
    SqliteDatabaseImpl(std::string path, std::map<std::string, std::string, std::less<>> options,
                       std::span<const ResourceSchema> schemas);

    std::unique_ptr<Session> session(bool read_only = false) override;
    [[nodiscard]] Capability capabilities() const noexcept override;
    void migrate() override;

    [[nodiscard]] const ResourceSchema* find_schema(std::string_view name) const;
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::map<std::string, std::string, std::less<>>& options() const noexcept {
        return options_;
    }

private:
    std::string path_;
    std::map<std::string, std::string, std::less<>> options_;
    std::vector<ResourceSchema> schemas_;
    std::unordered_map<std::string_view, const ResourceSchema*> schema_by_name_;
};

/// SQLite 会话：schema 类型化读写；不得晚于其 Database 析构
class SqliteSession final : public Session {
public:
    SqliteSession(const SqliteDatabaseImpl* owner, std::unique_ptr<SQLite::Database> db,
                  bool read_only);

    void upsert(std::string_view collection, std::span<const Row> rows) override;
    uint64_t remove(std::string_view collection, const Filter& filter) override;
    [[nodiscard]] ResultSet find(std::string_view collection, const Filter& filter,
                                 const FindOptions& options) override;
    [[nodiscard]] ResultSet aggregate(std::string_view collection,
                                      const Aggregation& aggregation) override;
    [[nodiscard]] std::unique_ptr<Transaction> begin_transaction() override;
    [[nodiscard]] std::unique_ptr<Snapshot> begin_snapshot() override;

private:
    enum class Scope { None, Transaction, Snapshot };

    [[nodiscard]] const ResourceSchema& require_schema(std::string_view collection) const;
    [[nodiscard]] static Value read_column(const SQLite::Column& column, ValueType type);
    void begin_scope(Scope kind);
    void end_scope(bool commit);

    friend class SqliteTransaction;
    friend class SqliteSnapshot;

    const SqliteDatabaseImpl* owner_;
    std::unique_ptr<SQLite::Database> db_;
    bool read_only_ = false;
    Scope scope_ = Scope::None;
};

/// 写事务句柄：析构未 commit 则回滚；commit 失败时作用域保持, 由析构兜底
class SqliteTransaction final : public Transaction {
public:
    explicit SqliteTransaction(SqliteSession& session) : session_(&session) {}
    ~SqliteTransaction() override;

    void commit() override;
    void rollback() noexcept override;

private:
    SqliteSession* session_;
    bool finished_ = false;
};

/// 只读快照句柄：析构结束只读作用域 (异常吞掉)
class SqliteSnapshot final : public Snapshot {
public:
    explicit SqliteSnapshot(SqliteSession& session) : session_(&session) {}
    ~SqliteSnapshot() override;

private:
    SqliteSession* session_;
};

}  // namespace dztrader::db

#endif  // DZTRADER_DB_SRC_SQLITE_DRIVER_H_
