/**
 * @file sql_compiler.h
 * @brief 方言无关的 SQL 编译（Filter 白名单校验 / SELECT / DELETE / 聚合）+ 方言描述
 *
 * 取代 sqlite_sql.h 中的通用部分：同一能力不允许 SQLite/MySQL 各写一份（AGENTS.md 收敛规则）。
 * 方言差异以数据（SqlDialect）表达；upsert 与 DDL 因语义不同留在各驱动内。
 * 生成文本必须与既有 sqlite 实现逐字节一致（现有测试与一致性套件为回归门）。
 */
#ifndef DZTRADER_DB_SRC_SQL_COMPILER_H_
#define DZTRADER_DB_SRC_SQL_COMPILER_H_

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/db/types.h>

namespace dztrader::db::internal {

/// 方言描述：标识符引用符 + "不限量" LIMIT 参数文本（其余 SQL 文本两方言一致）
struct SqlDialect {
    char ident_quote = '"';
    /// limit < 0 且 offset > 0 时输出 "LIMIT <该文本> OFFSET n"：
    /// sqlite 用 "-1"（保持重构前逐字节输出）；mysql 不接受负数，用无符号上界
    std::string_view unlimited_limit = "-1";
};

/// 编译后的过滤条件：where 不含 "WHERE" 关键字；params 与 ? 占位符同序
struct CompiledFilter {
    std::string where;
    std::vector<Value> params;
};

/// SQLite 方言常量（供 sqlite 驱动与既有测试使用）
inline constexpr SqlDialect kSqliteDialect{'"', "-1"};
/// MySQL 方言常量
inline constexpr SqlDialect kMysqlDialect{'`', "18446744073709551615"};

/// 标识符引用（仅用于 schema 声明的表名/列名，不接受用户输入）
[[nodiscard]] std::string quote_ident(const SqlDialect& dialect, std::string_view ident);

/// schema 字段白名单查找；未注册字段返回 nullptr
[[nodiscard]] const FieldSchema* find_field(const ResourceSchema& schema, std::string_view name);

/// 逐条件白名单校验并编译为 "f op ?" / "f IN (?,...)"；条件间以 AND 连接
[[nodiscard]] CompiledFilter compile_filter(std::span<const Condition> conditions,
                                            const ResourceSchema& schema,
                                            const SqlDialect& dialect);

/// 引用形式的列清单 "c1, c2, ..."（schema 声明序）
[[nodiscard]] std::string build_column_list(const ResourceSchema& schema, const SqlDialect& dialect);

/// "?, ?, ..." 共 count 个占位符
[[nodiscard]] std::string build_placeholders(size_t count);

/// INSERT INTO <name> (<schema 字段序>) VALUES (?,...)
[[nodiscard]] std::string build_insert_sql(const ResourceSchema& schema, const SqlDialect& dialect);

/// DELETE FROM <name> [WHERE <where>]
[[nodiscard]] std::string build_delete_sql(const ResourceSchema& schema,
                                           const CompiledFilter& filter,
                                           const SqlDialect& dialect);

/// SELECT <schema 字段序> FROM <name> [WHERE] [ORDER BY] [LIMIT] [OFFSET]
[[nodiscard]] std::string build_select_sql(const ResourceSchema& schema, const CompiledFilter& filter,
                                           std::span<const SortSpec> sort, int64_t offset, int64_t limit,
                                           const SqlDialect& dialect);

/// SELECT <group_by 序>, <op>(<field>) FROM <name> [WHERE] [GROUP BY] [ORDER BY]
[[nodiscard]] std::string build_aggregate_sql(const ResourceSchema& schema, AggregateOp op,
                                              std::string_view field,
                                              std::span<const std::string> group_by,
                                              const CompiledFilter& filter,
                                              std::span<const SortSpec> sort,
                                              const SqlDialect& dialect);

}  // namespace dztrader::db::internal

#endif  // DZTRADER_DB_SRC_SQL_COMPILER_H_
