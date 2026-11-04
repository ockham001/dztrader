/**
 * @file schema_catalog.h
 * @brief 交易库（trade.db）schema v1 的平台自有声明（唯一真相源；ADR 0041）
 *
 * 本文件只提供平台自有类型与表声明，不依赖 `dzdb` 公共端口：建库 DDL 由声明
 * 机械生成（`build_schema_ddl()`），版本号即 SQLite `PRAGMA user_version`。
 * 无迁移链——版本不符/旧库由 master 启动时改名重建（ADR 0041）。
 *
 * 过渡期说明：td 写端与策略 SDK 仍经 `dzdb` 端口读写本 schema（`schema_dzdb.h`
 * 将其投影为 `db::ResourceSchema`）；#245（td 写端直连 SQLiteCpp）与 #252（SDK DB
 * 私有化）落地后删除该投影，本文件保持为唯一声明源。
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

/// 交易库默认文件名（`$DZTRADER_HOME/db/trade.db`）
inline constexpr std::string_view kTradeDbFileName = "trade.db";

/// 列存储类型（SQLite 亲和性）
enum class ColumnType : uint8_t { Integer, Real, Text };

/// 表列声明。`default_sql` 为 DDL 中的 DEFAULT 表达式原文（空 = 无默认值）。
struct ColumnSchema {
    std::string_view name;
    ColumnType type = ColumnType::Text;
    bool not_null = false;
    bool primary_key = false;
    std::string_view default_sql;
};

/// 索引声明（`unique = true` 表达业务唯一键；字段序敏感）
struct IndexSchema {
    std::string_view name;
    std::span<const std::string_view> columns;
    bool unique = false;
};

/// 表声明：列序 = 建表列序 = dzdb 投影的 Row 位置序
struct TableSchema {
    std::string_view name;
    std::span<const ColumnSchema> columns;
    std::span<const IndexSchema> indexes;
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

}  // namespace dztrader::tdstore

#endif  // DZTRADER_TDSTORE_SCHEMA_CATALOG_H_
