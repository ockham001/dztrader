/**
 * @file schema_catalog.h
 * @brief 交易库（trade.db）schema v1 的平台自有声明（唯一真相源；ADR 0041）
 *
 * 本文件只提供平台自有类型与表声明，不依赖任何 DB 端口：建库 DDL 由声明
 * 机械生成（`build_schema_ddl()`），版本号即 SQLite `PRAGMA user_version`。
 * 无迁移链——版本不符/旧库由 master 启动时改名重建（ADR 0041）。
 *
 * 消费方向（ADR 0044 决策 1）：策略 SDK 私有的 DB 层把本声明机械投影为
 * `db::ResourceSchema`（`libs/strategy_api/src/td_schema.h`），依赖方向是
 * SDK DB 层 → 本声明，平台侧不依赖端口/驱动。td 写端与 dzweb 直连 SQLiteCpp。
 */
#ifndef DZTRADER_TDSTORE_SCHEMA_CATALOG_H_
#define DZTRADER_TDSTORE_SCHEMA_CATALOG_H_

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dztrader::tdstore {

/// 交易库 schema 版本（写入 SQLite `PRAGMA user_version`；只增不改，无迁移链）
inline constexpr int kTdSchemaVersion = 1;

/// 列存储类型（SQLite 亲和性）
enum class ColumnType : uint8_t { Integer, Real, Text };

/// 表列声明。`default_sql` 为 DDL 中的 DEFAULT 表达式原文（空 = 无默认值）。
struct ColumnSchema {
    std::string_view name;
    ColumnType type = ColumnType::Text;
    bool not_null = false;
    bool primary_key = false;
    std::string_view default_sql;
    /// 平台内部列：诊断/持久化用，**不进策略可见投影**（ADR 0044 决策 2 的 `gateway`/`ext`）。
    /// 作为列声明的一部分单源表达，避免"哪些列不对外"在 SDK 侧另立第二份清单。
    bool platform_internal = false;
};

/// 索引声明（`unique = true` 表达业务唯一键；字段序敏感）
struct IndexSchema {
    std::string_view name;
    std::span<const std::string_view> columns;
    bool unique = false;
};

/// 表达式索引声明（列无法表达；DDL 由 `sql` 原文建）
struct ExpressionIndexSchema {
    std::string_view name;
    std::string_view sql;  ///< 完整 `CREATE INDEX ...` 语句（含 IF NOT EXISTS）
};

/// 表声明：列序 = 建表列序 = 端口侧 `db::Row` 的位置序
struct TableSchema {
    std::string_view name;
    std::span<const ColumnSchema> columns;
    std::span<const IndexSchema> indexes;
    std::span<const ExpressionIndexSchema> expression_indexes;
};

/// @name 四张交易表 + 暂留的 instruments 表（ADR 0041 §1）
/// @{
const TableSchema& orders_schema();
const TableSchema& trades_schema();
const TableSchema& positions_schema();
const TableSchema& trading_accounts_schema();
const TableSchema& instruments_schema();
/// @}

/// 全部表声明（序 = 建表序）
std::span<const TableSchema> tables();

/// 全部建表 + 建索引 + 表达式索引 DDL（幂等 `IF NOT EXISTS`；建库唯一入口）
std::string build_schema_ddl();

/// 某表对策略可见的列（`platform_internal == false`，声明序）；C API 投影的唯一依据
[[nodiscard]] std::span<const ColumnSchema> strategy_visible_columns(const TableSchema& table);

/// 表业务键列：列级 PRIMARY KEY 优先（按列声明序），否则唯一索引列（按索引声明序）；
/// 无键返回空 span。upsert 冲突目标与业务唯一键共用同一份声明（禁止第二份列清单）。
[[nodiscard]] std::span<const std::string_view> business_key_columns(const TableSchema& table);

/// 幂等 upsert SQL（命名参数 `:<列名>`，标识符双引号）：
/// `INSERT INTO t (cols) VALUES (:cols) ON CONFLICT (键列) DO UPDATE SET 非键列=excluded.非键列`。
/// 无业务键时退化为纯 INSERT（当前 5 张表都有键）。
[[nodiscard]] std::string build_upsert_sql(const TableSchema& table);

}  // namespace dztrader::tdstore

#endif  // DZTRADER_TDSTORE_SCHEMA_CATALOG_H_
