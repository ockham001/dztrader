/**
 * @file sqlite_sql.h
 * @brief SQLite 方言 SQL 编译（Filter/投影/排序/分页/聚合 → SQL + 绑定参数）
 */
#ifndef DZTRADER_DB_SRC_SQLITE_SQL_H_
#define DZTRADER_DB_SRC_SQLITE_SQL_H_

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/db/types.h>

namespace dztrader::db::internal {

/// 编译后的过滤条件：where 不含 "WHERE" 关键字；params 与 ? 占位符同序
struct CompiledFilter {
    std::string where;
    std::vector<Value> params;
};

/// schema 字段白名单查找；未注册字段返回 nullptr
[[nodiscard]] const FieldSchema* find_field(const ResourceSchema& schema, std::string_view name);

/// 逐条件白名单校验并编译为 "f op ?" / "f IN (?,...)"；条件间以 AND 连接
[[nodiscard]] CompiledFilter compile_filter(std::span<const Condition> conditions,
                                            const ResourceSchema& schema);

/// INSERT OR REPLACE INTO <name> (<schema 字段序>) VALUES (?,...)
[[nodiscard]] std::string build_upsert_sql(const ResourceSchema& schema);

/// DELETE FROM <name> [WHERE <where>]
[[nodiscard]] std::string build_delete_sql(const ResourceSchema& schema,
                                           const CompiledFilter& filter);

/// SELECT <schema 字段序> FROM <name> [WHERE] [ORDER BY] [LIMIT] [OFFSET]
[[nodiscard]] std::string build_select_sql(const ResourceSchema& schema, const CompiledFilter& filter,
                                           std::span<const SortSpec> sort, int64_t offset,
                                           int64_t limit);

/// SELECT <group_by 序>, <op>(<field>) FROM <name> [WHERE] [GROUP BY] [ORDER BY]
[[nodiscard]] std::string build_aggregate_sql(const ResourceSchema& schema, AggregateOp op,
                                              std::string_view field,
                                              std::span<const std::string> group_by,
                                              const CompiledFilter& filter,
                                              std::span<const SortSpec> sort);

}  // namespace dztrader::db::internal

#endif  // DZTRADER_DB_SRC_SQLITE_SQL_H_
