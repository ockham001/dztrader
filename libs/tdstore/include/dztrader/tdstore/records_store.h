/**
 * @file records_store.h
 * @brief td 记录 <-> db::Row 编解码 + instruments Session facade
 */
#ifndef DZTRADER_TDSTORE_RECORDS_STORE_H_
#define DZTRADER_TDSTORE_RECORDS_STORE_H_

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/db/database.h>
#include <dztrader/struct.h>
#include <dztrader/tdstore/records.h>

namespace dztrader::tdstore {

/// NULL 值
[[nodiscard]] dztrader::db::Value null_value();

/// 字符串字段（char 数组 -> std::string，显式避免 const char* -> bool 误选）
[[nodiscard]] dztrader::db::Value to_value(std::string_view text);
[[nodiscard]] dztrader::db::Value to_value(int32_t value);
[[nodiscard]] dztrader::db::Value to_value(int64_t value);
[[nodiscard]] dztrader::db::Value to_value(double value);
[[nodiscard]] dztrader::db::Value to_value(char value);  ///< 枚举字符 -> int64 字符码（保持现状）

db::Row make_instrument_row(const InstrumentRecord& record);
db::Row make_position_row(const DzPositionInfo& record, std::string_view trading_day);
db::Row make_trading_account_row(const DzTradingAccount& record, std::string_view trading_day);

void upsert_instruments(dztrader::db::Session& session,
                        std::span<const InstrumentRecord> records);
dztrader::db::ResultSet query_instruments(dztrader::db::Session& session,
                                          std::string_view instrument_id,
                                          std::span<const std::string> fields);

/// dz_db_query_instruments fields 为空时返回的承诺列 (契约 docs/frame_contracts/instrument.md §7,
/// 声明序共 25 列)。query_instruments 的空投影 = schema 全列 (28, 含 3 个保留列
/// settlement_method/option_exercise_style/option_series), 与 C API 既有返回列不符 —
/// C 边界显式传承诺列, 保持结果列序/列名零变化。
[[nodiscard]] const std::vector<std::string>& instrument_promised_fields();

}  // namespace dztrader::tdstore

#endif  // DZTRADER_TDSTORE_RECORDS_STORE_H_
