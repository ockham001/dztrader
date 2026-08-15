#ifndef DZTRADER_DB_MIGRATION_H_
#define DZTRADER_DB_MIGRATION_H_

#include <functional>
#include <map>
#include <vector>

#include "dztrader/db/sqlite.h"

namespace dztrader::db {

/// 通用 SQLite schema 版本管理.
/// 维护 schema_version(version, applied_at) 表, 按编号顺序应用迁移函数.
/// 每个迁移在独立事务中执行, 失败抛异常 (整个 apply 是原子的).
class MigrationManager {
public:
    /// 迁移函数: 接收 Database 引用, 执行 DDL/DML.
    using MigrationFn = std::function<void(SQLite::Database&)>;

    /// 注册一个迁移 (version 从 1 开始, 单调递增).
    void add(int version, MigrationFn fn);

    /// 应用所有未应用的迁移 (按 version 升序).
    /// 自动创建 schema_version 表 (若不存在).
    /// 每个迁移在独立事务中执行; 任一失败抛 std::runtime_error, 已应用的保留.
    /// 重复应用相同版本是 no-op.
    /// 返回本次新应用的版本号列表 (空表示无需迁移).
    std::vector<int> apply(SQLite::Database& db);

private:
    std::map<int, MigrationFn> migrations_;
};

}  // namespace dztrader::db

#endif  // DZTRADER_DB_MIGRATION_H_
