#ifndef DZTRADER_DB_CONNECTION_H_
#define DZTRADER_DB_CONNECTION_H_

#include <memory>
#include <string>

#include "dztrader/db/sqlite.h"

namespace dztrader::db {

/// RAII SQLite 连接封装.
/// 单线程独占 (dztd_ctp 的 PersistWriter 线程独占, 无并发).
class Connection {
public:
    /// 打开数据库 (path=":memory:" 为内存库). 失败抛 std::runtime_error.
    explicit Connection(const std::string& path);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;

    /// 执行 SQL (无返回值, 失败抛).
    void exec(const std::string& sql);

    /// 查询单值 (标量). T 必须支持从 SQLite::Column 隐式转换.
    /// 注: 用 return col 隐式转换 — 不可用 T{col} (brace 会触发 std::string(initializer_list<char>)
    ///     经由 Column::operator char() 产生单字节字符串), 亦不可用 static_cast<T>(col)
    ///     (Column 同时有 operator const char* 和 operator std::string, 二义性).
    template <typename T>
    T scalar(const std::string& sql) {
        SQLite::Statement q(*db_, sql);
        if (q.executeStep()) {
            const auto& col = q.getColumn(0);
            if (col.isNull()) {
                return T{};
            }
            return col;  // 隐式转换: 调用 Column::operator T()
        }
        return T{};
    }

    /// 读取 PRAGMA 值.
    template <typename T>
    T pragma(const std::string& name) {
        return scalar<T>("PRAGMA " + name);
    }

    /// 开启事务. 返回 RAII Transaction, 析构时若未 commit 则回滚.
    [[nodiscard]] SQLite::Transaction begin_transaction();

    /// 获取原生 SQLite::Database (供 prepared statement 使用).
    SQLite::Database& db() noexcept { return *db_; }

private:
    std::unique_ptr<SQLite::Database> db_;
};

}  // namespace dztrader::db

#endif  // DZTRADER_DB_CONNECTION_H_
