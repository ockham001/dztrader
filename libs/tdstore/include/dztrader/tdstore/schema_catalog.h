/**
 * @file schema_catalog.h
 * @brief td 库当前 schema 声明（ResourceSchema；字段序 = 建表列序，唯一真相源）
 */
#ifndef DZTRADER_TDSTORE_SCHEMA_CATALOG_H_
#define DZTRADER_TDSTORE_SCHEMA_CATALOG_H_

#include <span>

#include <dztrader/db/types.h>

namespace dztrader::tdstore {

/// td schema 当前版本（与驱动迁移版本对齐；v5 = 删除费率两表, ADR 0013）
constexpr int kTdSchemaVersion = 5;

const dztrader::db::ResourceSchema& orders_schema();
const dztrader::db::ResourceSchema& trades_schema();
const dztrader::db::ResourceSchema& positions_schema();
const dztrader::db::ResourceSchema& trading_accounts_schema();
const dztrader::db::ResourceSchema& instruments_schema();

std::span<const dztrader::db::ResourceSchema> schemas();

}  // namespace dztrader::tdstore

#endif  // DZTRADER_TDSTORE_SCHEMA_CATALOG_H_
