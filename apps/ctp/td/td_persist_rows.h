#ifndef DZTRADER_CTP_TD_PERSIST_ROWS_H_
#define DZTRADER_CTP_TD_PERSIST_ROWS_H_

#include <string_view>

#include <SQLiteCpp/Statement.h>

#include <dztrader/tdstore/records.h>

#include "td/td_persist_records.h"

namespace dztrader::ctp {

// ============================================================================
// 记录 -> trade.db 表行的命名参数绑定（直连 SQLiteCpp）
//
// SQL 由 `tdstore::build_upsert_sql(tdstore::<table>_schema())` 从 schema 声明生成，
// 参数名 = schema_catalog.h 的列名（`:account_id` 等）；绑定函数只负责列值语义
// （ext JSON 键、保留策略标识、date/gateway 表示），列序不敏感。
// ============================================================================

/// OrderRecord -> orders upsert 参数（gateway = 写入来源 td 实例名，审计列，不进唯一键）。
void bind_order(SQLite::Statement& stmt, const OrderRecord& record, std::string_view gateway);

/// TradeRecord -> trades upsert 参数（去重键 account_id + date + exchange_id + trade_id）。
void bind_trade(SQLite::Statement& stmt, const TradeRecord& record, std::string_view gateway);

/// DzPositionInfo -> positions upsert 参数（date = "YYYYMMDD" 交易日；ext 暂空 "{}"）。
void bind_position(SQLite::Statement& stmt,
                   const DzPositionInfo& record,
                   std::string_view date,
                   std::string_view gateway);

/// DzTradingAccount -> trading_accounts upsert 参数（PK account_id + date）。
void bind_trading_account(SQLite::Statement& stmt,
                          const DzTradingAccount& record,
                          std::string_view date,
                          std::string_view gateway);

/// InstrumentRecord -> instruments upsert 参数（合约信息暂留 trade.db，ADR 0041 §1）。
void bind_instrument(SQLite::Statement& stmt, const tdstore::InstrumentRecord& record);

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_PERSIST_ROWS_H_
