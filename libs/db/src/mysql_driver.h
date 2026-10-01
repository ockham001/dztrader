/**
 * @file mysql_driver.h
 * @brief MySQL 驱动（Boost.MySQL 同步 API）：Database/Session/Transaction/Snapshot 实现
 *
 * ADR 0016：语义与 sqlite 驱动对齐；Database 不持有连接，session() 时每次新建一条连接
 * （会话一线程一个、非线程安全）。Boost.MySQL 头只在 mysql_driver.cpp 内可见，
 * 本头保持轻量（内置驱动注册只依赖 open_mysql 声明）。
 */
#ifndef DZTRADER_DB_SRC_MYSQL_DRIVER_H_
#define DZTRADER_DB_SRC_MYSQL_DRIVER_H_

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <dztrader/db/database.h>

namespace dztrader::db {

namespace internal {

/// 连接实现（定义在 mysql_driver.cpp；隔离 Boost.MySQL 头与 io_context）
class MysqlConnection;

/// TLS 模式（Boost.MySQL 1.90 的 ssl_mode 只有 disable/enable/require，证书校验经 SSL context）：
/// - disabled：从不使用 TLS
/// - preferred：服务端支持则用 TLS，否则明文（缺省）
/// - required：必须 TLS，不做证书校验
/// - verify_ca：必须 TLS + 校验证书链（系统 CA）
/// - verify_identity：必须 TLS + 校验证书链与主机名
/// 跨主机部署应显式 required/verify_ca/verify_identity，不要依赖缺省
enum class MysqlSslMode {
    Disabled,
    Preferred,
    Required,
    VerifyCa,
    VerifyIdentity,
};

/// MySQL 连接级选项（Config.options 解析结果，键与 spec §6.3 一致）
struct MysqlOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3306;
    std::string user;
    std::string password;
    std::string database = "dztrader";
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds read_timeout{0};   ///< 0 = 不限时
    std::chrono::milliseconds write_timeout{0};  ///< 0 = 不限时
    MysqlSslMode ssl_mode = MysqlSslMode::Preferred;
    /// 期望的 schema 版本；仅 migrate() 需要，缺失时 migrate 抛 DZ_EC_INVALID_PARAM
    std::optional<std::int64_t> schema_version;
};

/// 解析 Config.options；数值/枚举非法抛 Exception(DZ_EC_INVALID_PARAM)；未知键忽略
[[nodiscard]] MysqlOptions parse_mysql_options(
    const std::map<std::string, std::string, std::less<>>& options);

}  // namespace internal

/// MySQL 驱动入口（builtin_drivers.cpp 在 DZ_WITH_MYSQL 下注册为 "mysql"）
[[nodiscard]] std::unique_ptr<Database> open_mysql(const Config& config,
                                                   std::span<const ResourceSchema> schemas);

class MysqlDatabaseImpl final : public Database {
public:
    MysqlDatabaseImpl(internal::MysqlOptions options, std::span<const ResourceSchema> schemas);

    std::unique_ptr<Session> session(bool read_only = false) override;
    [[nodiscard]] Capability capabilities() const noexcept override;
    void migrate() override;

    [[nodiscard]] const ResourceSchema* find_schema(std::string_view name) const;
    [[nodiscard]] const internal::MysqlOptions& options() const noexcept { return options_; }

private:
    internal::MysqlOptions options_;
    std::vector<ResourceSchema> schemas_;
    std::unordered_map<std::string_view, const ResourceSchema*> schema_by_name_;
};

/// MySQL 会话：schema 类型化读写；不得晚于其 Database 析构
class MysqlSession final : public Session {
public:
    MysqlSession(const MysqlDatabaseImpl* owner,
                 std::unique_ptr<internal::MysqlConnection> connection, bool read_only);
    ~MysqlSession() override;

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
    void begin_scope(Scope kind);
    void end_scope(bool commit);

    friend class MysqlTransaction;
    friend class MysqlSnapshot;

    const MysqlDatabaseImpl* owner_;
    std::unique_ptr<internal::MysqlConnection> conn_;
    bool read_only_ = false;
    Scope scope_ = Scope::None;
};

/// 写事务句柄：析构未 commit 则回滚；commit 失败时作用域保持, 由析构兜底
class MysqlTransaction final : public Transaction {
public:
    explicit MysqlTransaction(MysqlSession& session) : session_(&session) {}
    ~MysqlTransaction() override;

    void commit() override;
    void rollback() noexcept override;

private:
    MysqlSession* session_;
    bool finished_ = false;
};

/// 只读快照句柄：析构结束只读作用域 (异常吞掉)
class MysqlSnapshot final : public Snapshot {
public:
    explicit MysqlSnapshot(MysqlSession& session) : session_(&session) {}
    ~MysqlSnapshot() override;

private:
    MysqlSession* session_;
};

}  // namespace dztrader::db

#endif  // DZTRADER_DB_SRC_MYSQL_DRIVER_H_
