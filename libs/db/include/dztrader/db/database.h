/**
 * @file database.h
 * @brief 数据库后端无关接口（本轮 SQLite 适配; MySQL 见后续项目）
 */
#ifndef DZTRADER_DB_DATABASE_H_
#define DZTRADER_DB_DATABASE_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dztrader::db {

/// 绑定值 / 结果值 (NULL 用 monostate)
using BindValue = std::variant<std::monostate, bool, int64_t, double, std::string>;

/// 列类型: 自有枚举, 不引用 strategy_api 的 DzColumnType (core 已 PUBLIC 转发 strategy_api_headers,
/// 此处刻意不扩大耦合面); SDK 侧 (api.cpp/db_database.cpp) 负责映射为 DzColumnType。
enum class ColumnType { Null, Bool, Int64, Float64, String };

struct ColumnMeta {
    ColumnType type;
    std::string name;
};

using Row = std::vector<BindValue>;

struct QueryResult {
    std::vector<ColumnMeta> columns;
    std::vector<Row> rows;
};

/// 预处理语句 (persist 批处理复用; index 1-based)
class Statement {
public:
    virtual ~Statement() = default;
    virtual void bind(int index, const BindValue& value) = 0;
    virtual void execute() = 0;
    virtual void reset() = 0;
};

/// 事务 (RAII: 析构未 commit 则 rollback)
class Transaction {
public:
    virtual ~Transaction() = default;
    virtual void commit() = 0;
};

class Database {
public:
    virtual ~Database() = default;

    virtual void exec(std::string_view sql) = 0;
    virtual QueryResult query(std::string_view sql,
                              std::span<const BindValue> params = {}) = 0;
    virtual std::unique_ptr<Statement> prepare(std::string_view sql) = 0;
    virtual std::unique_ptr<Transaction> begin(bool immediate = false) = 0;
};

}  // namespace dztrader::db

#endif  // DZTRADER_DB_DATABASE_H_
