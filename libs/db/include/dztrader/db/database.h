/**
 * @file database.h
 * @brief 数据库统一接口（端口）：Database/Session/Transaction/Snapshot/能力与配置
 */
#ifndef DZTRADER_DB_DATABASE_H_
#define DZTRADER_DB_DATABASE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <dztrader/db/types.h>

namespace dztrader::db {

/// 能力位标志（启动准入校验用）
enum class Capability : uint32_t {
    None = 0,
    Transactions = 1u << 0,
    SnapshotRead = 1u << 1,
    Upsert = 1u << 2,
    OrderedRangeScan = 1u << 3,
};

[[nodiscard]] constexpr Capability operator|(Capability a, Capability b) noexcept {
    return static_cast<Capability>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

/// 全位语义：set 必须包含 probe 的全部位（事务域准入 "缺一拒绝" 依赖此语义）
[[nodiscard]] constexpr bool has(Capability set, Capability probe) noexcept {
    const uint32_t probe_bits = static_cast<uint32_t>(probe);
    return probe_bits != 0 && (static_cast<uint32_t>(set) & probe_bits) == probe_bits;
}

struct Config {
    std::string backend;  ///< "sqlite" / ...
    std::map<std::string, std::string, std::less<>> options;
};

/// 写事务（RAII：析构未 commit 则 rollback）
class Transaction {
public:
    virtual ~Transaction() = default;
    virtual void commit() = 0;
    virtual void rollback() noexcept = 0;
};

/// 只读一致性视图（RAII）
class Snapshot {
public:
    virtual ~Snapshot() = default;
};

/// 会话（非线程安全；一线程一 Session；生命周期不得晚于 Database）
class Session {
public:
    virtual ~Session() = default;

    virtual void upsert(std::string_view collection, std::span<const Row> rows) = 0;
    virtual uint64_t remove(std::string_view collection, const Filter& filter) = 0;
    [[nodiscard]] virtual ResultSet find(std::string_view collection, const Filter& filter = {},
                                         const FindOptions& options = {}) = 0;
    [[nodiscard]] virtual ResultSet aggregate(std::string_view collection,
                                              const Aggregation& aggregation) = 0;

    [[nodiscard]] virtual std::unique_ptr<Transaction> begin_transaction() = 0;
    [[nodiscard]] virtual std::unique_ptr<Snapshot> begin_snapshot() = 0;
};

/// 连接/驱动入口（线程安全，进程内共享；唯一知道"什么库"的地方）
class Database {
public:
    [[nodiscard]] static std::unique_ptr<Database> open(
        const Config& config, std::span<const ResourceSchema> schemas = {});

    virtual ~Database() = default;

    [[nodiscard]] virtual std::unique_ptr<Session> session(bool read_only = false) = 0;
    [[nodiscard]] virtual Capability capabilities() const noexcept = 0;

    /// 按 open 注册的 schema 建表/索引并应用版本迁移（幂等；写路径调用）
    virtual void migrate() = 0;
};

}  // namespace dztrader::db

#endif  // DZTRADER_DB_DATABASE_H_
