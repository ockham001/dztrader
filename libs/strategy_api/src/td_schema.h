/**
 * @file td_schema.h
 * @brief 交易库 schema v1 在 SDK 私有 DB 层里的投影（真相源 =
 * `<dztrader/tdstore/schema_catalog.h>`）
 *
 * 平台侧（td 写端 / tdstore / dzweb）直连 SQLite，不依赖 DB 端口；策略 SDK 仍经端口-适配器
 * 读写同一份 schema（ADR 0044），本投影把平台自有声明机械转成 `db::ResourceSchema`，
 * **不产生第二份列清单**——列名/列序/主键/索引全部来自 `tdstore::tables()`。
 *
 * 为什么放在 SDK 私有层而不是 tdstore：依赖方向是「SDK DB 层 → 平台 schema 声明」。
 * 反过来（tdstore → DB 端口）会让平台侧重新背上端口依赖，正是 ADR 0041 决策 5 /
 * ADR 0044 决策 1 要消除的形态。本头不在 `strategy_api_headers` 的公开 include 面内。
 */
#ifndef DZTRADER_STRATEGY_API_TD_SCHEMA_H_
#define DZTRADER_STRATEGY_API_TD_SCHEMA_H_

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/db/types.h>

namespace dztrader::db {

/// 全部表声明的投影（序 = `tdstore::tables()`；字段序 = Row 位置序）
[[nodiscard]] std::span<const ResourceSchema> td_schemas();

/// `instruments` 表声明的投影（`dz_db_query_instruments` 的按列投影查询用）
[[nodiscard]] const ResourceSchema& td_instruments_schema();

/// 某表对策略可见的列名（声明序；已排除 `tdstore` 声明为平台内部的列）。
/// C API 投影的唯一依据——"哪些列不对外"单源在 `tdstore::ColumnSchema::platform_internal`，
/// 本层不另立清单（ADR 0044 决策 2/4）。
[[nodiscard]] std::vector<std::string> strategy_visible_column_names(std::string_view table);

}  // namespace dztrader::db

#endif  // DZTRADER_STRATEGY_API_TD_SCHEMA_H_
