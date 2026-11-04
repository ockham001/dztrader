#ifndef DZTRADER_CTP_TD_PERSIST_ROWS_H_
#define DZTRADER_CTP_TD_PERSIST_ROWS_H_

#include <string_view>

#include <dztrader/db/types.h>

#include "td/td_persist_records.h"

namespace dztrader::ctp {

/// OrderRecord -> orders 表行（列序 = tdstore::orders_schema() schema v1）。
/// 柜台扩展字段编码进 ext JSON；gateway = 写入来源 td 实例名（审计列，不进唯一键）。
[[nodiscard]] dztrader::db::Row make_order_row(const OrderRecord& record, std::string_view gateway);

/// TradeRecord -> trades 表行（列序 = tdstore::trades_schema() schema v1）。
[[nodiscard]] dztrader::db::Row make_trade_row(const TradeRecord& record, std::string_view gateway);

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_PERSIST_ROWS_H_
