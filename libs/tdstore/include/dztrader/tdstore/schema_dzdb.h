/**
 * @file schema_dzdb.h
 * @brief 交易库 schema v1 的 `dzdb` 投影（**过渡层**）
 *
 * 真相源是 `schema_catalog.h`（平台自有声明）。策略 SDK 仍经 `dzdb` 端口读写交易库，
 * 本投影把同一份声明转成 `db::ResourceSchema`，避免第二份列清单。td 写端自 #245 起
 * 直连 SQLiteCpp、不再依赖本投影；#252（SDK DB 私有化）落地后删除本文件。
 */
#ifndef DZTRADER_TDSTORE_SCHEMA_DZDB_H_
#define DZTRADER_TDSTORE_SCHEMA_DZDB_H_

#include <span>

#include <dztrader/db/types.h>

namespace dztrader::tdstore {

/// 全部表声明的 dzdb 投影（序 = `tdstore::tables()`；字段序 = Row 位置序）
std::span<const dztrader::db::ResourceSchema> schemas();

/// instruments 表声明的 dzdb 投影（records_store 的按列投影查询用）
const dztrader::db::ResourceSchema& instruments_schema_db();

}  // namespace dztrader::tdstore

#endif  // DZTRADER_TDSTORE_SCHEMA_DZDB_H_
