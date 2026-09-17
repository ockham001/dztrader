#ifndef DZTRADER_DB_LEGACY_MIGRATION_H_
#define DZTRADER_DB_LEGACY_MIGRATION_H_

#include <functional>
#include <map>
#include <vector>

#include "dztrader/db/sqlite.h"

namespace dztrader::db::legacy {

/// 通用 SQLite schema 版本管理.
/// 维护 schema_version(version, applied_at) 表, 按编号顺序应用迁移函数.
/// 全部未应用迁移与版本记录在单个 IMMEDIATE 事务中一起提交, 任一失败整体回滚 (幂等重跑).
class MigrationManager {
public:
    /// 迁移函数: 接收 Database 引用, 执行 DDL/DML.
    using MigrationFn = std::function<void(SQLite::Database&)>;

    /// 注册一个迁移 (version 从 1 开始, 单调递增).
    void add(int version, MigrationFn fn);

    /// 应用所有未应用的迁移 (按 version 升序).
    /// 自动创建 schema_version 表 (若不存在).
    /// 全部未应用迁移在单个 IMMEDIATE 事务内执行; 任一失败抛 std::runtime_error,
    /// 全部回滚 (含本次已执行的迁移).
    /// 重复应用相同版本是 no-op.
    /// 返回本次新应用的版本号列表 (空表示无需迁移).
    std::vector<int> apply(SQLite::Database& db);

private:
    std::map<int, MigrationFn> migrations_;
};

}  // namespace dztrader::db::legacy

#endif  // DZTRADER_DB_LEGACY_MIGRATION_H_
