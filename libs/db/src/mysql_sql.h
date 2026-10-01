/**
 * @file mysql_sql.h
 * @brief MySQL 方言 SQL 文本：DDL（InnoDB/utf8mb4_bin/索引内联）、upsert、版本表
 *
 * 通用编译（Filter/SELECT/DELETE/聚合）在 sql_compiler.{h,cpp}，此处只放 MySQL 专属部分：
 * upsert 与 DDL 的语义与 SQLite 不同，不能共用（ADR 0016 / spec §5、§7.2、§7.4、§7.5）。
 * 占位符统一用 `?`（prepared statement 绑定，两方言一致）。
 */
#ifndef DZTRADER_DB_SRC_MYSQL_SQL_H_
#define DZTRADER_DB_SRC_MYSQL_SQL_H_

#include <string>
#include <string_view>

#include <dztrader/db/types.h>

namespace dztrader::db::internal {

/// 版本表名（与 SQLite 侧 schema_version 同名同形状：version 主键 + applied_at 文本）
inline constexpr std::string_view kMysqlSchemaVersionTable = "schema_version";

/// ValueType -> MySQL 列类型文本；ValueType::Null 不支持（抛 DZ_EC_INVALID_PARAM）
[[nodiscard]] std::string_view mysql_column_type(ValueType type, std::string_view column,
                                                 std::string_view collection);

/// CREATE TABLE IF NOT EXISTS：InnoDB + ROW_FORMAT=DYNAMIC + utf8mb4/utf8mb4_bin，
/// 主键/自增/索引内联
[[nodiscard]] std::string build_mysql_create_table_sql(const ResourceSchema& schema);

/// INSERT INTO ... VALUES (?,...) ON DUPLICATE KEY UPDATE c=VALUES(c)（跳过自增列）
[[nodiscard]] std::string build_mysql_upsert_sql(const ResourceSchema& schema);

/// 版本表 DDL（schema_version(version INT PK, applied_at VARCHAR(32))，与 sqlite 侧同名）
[[nodiscard]] std::string_view mysql_schema_version_ddl() noexcept;

/// 版本表当前版本读取：SELECT MAX(version)（空表返回 NULL）
[[nodiscard]] std::string_view mysql_schema_version_select() noexcept;

/// 版本表写入（version, applied_at 两个 `?` 参数；幂等 upsert）
[[nodiscard]] std::string_view mysql_schema_version_insert() noexcept;

}  // namespace dztrader::db::internal

#endif  // DZTRADER_DB_SRC_MYSQL_SQL_H_
