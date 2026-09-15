/**
 * @file instrument_store.h
 * @brief instruments 表 store ops (记录 upsert / 投影查询 / symbol 定向解析)
 */
#ifndef DZTRADER_TDSTORE_INSTRUMENT_STORE_H_
#define DZTRADER_TDSTORE_INSTRUMENT_STORE_H_

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/db/database.h>
#include <dztrader/tdstore/records.h>

namespace dztrader::tdstore {

/// 承诺字段白名单 (schema 序; query 默认返回全部)
const std::vector<std::string>& instrument_columns();

/// upsert 单条合约记录 (INSERT OR REPLACE, PK=instrument_id; updated_at 由调用方填)
void upsert_instrument(dztrader::db::Database& db, const InstrumentRecord& record);

/// 查询合约: fields 为空 = 全部承诺列; 非法/重复列名抛 Exception(DZ_EC_INVALID_PARAM);
/// 行序 instrument_id 升序; instrument_id 空 = 全量。列元信息按请求声明填充 (0 行仍正确)。
dztrader::db::QueryResult query_instruments(dztrader::db::Database& db,
                                            std::string_view instrument_id,
                                            std::span<const std::string> fields);

/// 定向刷新解析: DB 现有行的 symbol; 无行返回 ""。
std::string lookup_symbol(dztrader::db::Database& db, std::string_view instrument_id);

}  // namespace dztrader::tdstore

#endif  // DZTRADER_TDSTORE_INSTRUMENT_STORE_H_
