#ifndef DZTRADER_CTP_TD_PERSIST_ROWS_H_
#define DZTRADER_CTP_TD_PERSIST_ROWS_H_

#include <dztrader/db/types.h>

#include "td/td_persist_records.h"

namespace dztrader::ctp {

/// OrderRecord -> orders 表行 (字段序 = tdstore schema_catalog, id 为 NULL 自增).
[[nodiscard]] dztrader::db::Row make_order_row(const OrderRecord& record);

/// TradeRecord -> trades 表行 (字段序 = tdstore schema_catalog, id 为 NULL 自增).
[[nodiscard]] dztrader::db::Row make_trade_row(const TradeRecord& record);

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_PERSIST_ROWS_H_
