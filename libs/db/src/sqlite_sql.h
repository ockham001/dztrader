/**
 * @file sqlite_sql.h
 * @brief SQLite 专有 SQL 编译（upsert = INSERT OR REPLACE）
 *
 * 通用部分（Filter 白名单/SELECT/DELETE/聚合）已收敛到 sql_compiler.h；
 * DDL 与迁移见 sqlite_migrations.h。
 */
#ifndef DZTRADER_DB_SRC_SQLITE_SQL_H_
#define DZTRADER_DB_SRC_SQLITE_SQL_H_

#include <string>

#include <dztrader/db/types.h>

namespace dztrader::db::internal {

/// INSERT OR REPLACE INTO <name> (<schema 字段序>) VALUES (?,...)
[[nodiscard]] std::string build_upsert_sql(const ResourceSchema& schema);

}  // namespace dztrader::db::internal

#endif  // DZTRADER_DB_SRC_SQLITE_SQL_H_
