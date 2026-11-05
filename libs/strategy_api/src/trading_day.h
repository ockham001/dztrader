/**
 * @file trading_day.h
 * @brief 交易日两表示互转：`DzDate`（距纪元天数）<-> DB/REST 文本 `YYYYMMDD`
 *
 * 表示层单源（ADR 0041 决策 4）：帧与 SDK 参数用 `DzDate`，交易库 `date` 列是 TEXT
 * `YYYYMMDD`。SDK 内两处需要该编解码——交易库查询（`db_database.cpp`）与 ingest 成交
 * 去重段键（`api.cpp`）——故收在一处，避免两份实现漂移。
 */
#ifndef DZTRADER_STRATEGY_API_TRADING_DAY_H_
#define DZTRADER_STRATEGY_API_TRADING_DAY_H_

#include <string>
#include <string_view>

#include <dztrader/data_type.h>

namespace dztrader::strategy_api_internal {

/**
 * @brief `DzDate` -> "YYYYMMDD"（8 位）
 *
 * @param days 距纪元天数；`<= 0`（含 `DZ_DATE_NA`）或非法日期返回空串
 * @return 8 位交易日文本；空串 = 无有效交易日
 */
[[nodiscard]] std::string format_trading_day(DzDate days);

/**
 * @brief "YYYYMMDD" -> `DzDate`
 *
 * @param text 8 位交易日文本
 * @return 距纪元天数；非 8 位/非法回落 `DZ_DATE_NA`
 */
[[nodiscard]] DzDate parse_trading_day(std::string_view text) noexcept;

}  // namespace dztrader::strategy_api_internal

#endif  // DZTRADER_STRATEGY_API_TRADING_DAY_H_
