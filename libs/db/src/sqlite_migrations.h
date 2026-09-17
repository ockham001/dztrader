/**
 * @file sqlite_migrations.h
 * @brief SQLite 驱动内部迁移（td v1..v5 + ResourceSchema 自动建表）
 */
#ifndef DZTRADER_DB_SRC_SQLITE_MIGRATIONS_H_
#define DZTRADER_DB_SRC_SQLITE_MIGRATIONS_H_

#include <span>

#include <SQLiteCpp/Database.h>

#include <dztrader/db/types.h>

namespace dztrader::db::internal {

/// td schema v1..v5（版本表 schema_version；单 IMMEDIATE 事务；幂等；失败抛 DZ_EC_DB_MIGRATE_FAILED）
void apply_td_migrations(SQLite::Database& db);

/// 对 sqlite_master 中不存在的 collection 按 ResourceSchema 建表 + 索引（IF NOT EXISTS，幂等）
void create_missing_collections(SQLite::Database& db, std::span<const ResourceSchema> schemas);

}  // namespace dztrader::db::internal

#endif  // DZTRADER_DB_SRC_SQLITE_MIGRATIONS_H_
