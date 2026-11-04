/**
 * @file sqlite_migrations.h
 * @brief SQLite 驱动内部 schema 辅助（按 ResourceSchema 建缺失集合）
 *
 * 交易库 schema v1 的声明与建库/重建由 `libs/tdstore` 平台自有实现负责
 * （ADR 0041：无迁移链，master 建库）；本文件只保留通用端口的建表能力，
 * 供仍经 `dzdb` 端口的写端/测试在缺失集合时建表。
 */
#ifndef DZTRADER_DB_SRC_SQLITE_MIGRATIONS_H_
#define DZTRADER_DB_SRC_SQLITE_MIGRATIONS_H_

#include <span>

#include "sqlite.h"

#include <dztrader/db/types.h>

namespace dztrader::db::internal {

/// 对 sqlite_master 中不存在的 collection 按 ResourceSchema 建表 + 索引（IF NOT EXISTS，幂等）
void create_missing_collections(SQLite::Database& db, std::span<const ResourceSchema> schemas);

}  // namespace dztrader::db::internal

#endif  // DZTRADER_DB_SRC_SQLITE_MIGRATIONS_H_
