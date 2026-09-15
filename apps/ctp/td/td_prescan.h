#ifndef DZTRADER_CTP_TD_PRESCAN_H_
#define DZTRADER_CTP_TD_PRESCAN_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <SQLiteCpp/Database.h>

#include "td/td_persist_records.h"  // OrderRecord / TradeRecord

namespace dztrader::ctp {

/// 账户启动装载数据 (Task 5 §4.3): connect 时随 AccountSession 初始化.
/// start_seq 为该账户 DB 已提交最大 seq (调用方取 MAX+1 作为 seq 计数器起点).
/// orders/trades 为该账户全量历史行, 作重放过滤器基准.
struct SessionBootData {
    uint64_t start_seq = 0;
    std::vector<OrderRecord> orders;
    std::vector<TradeRecord> trades;
};

/// 四表取大 (orders/trades/positions/trading_accounts 按 account_id 过滤).
/// 空账户返回 0. db 为独立只读连接 (不占 PersistWriter 的 db_).
uint64_t query_max_seq(SQLite::Database& db, const std::string& account_id);

/// 全量装载该账户 orders (过滤器基准). 行序 = seq 序.
std::vector<OrderRecord> load_orders(SQLite::Database& db, const std::string& account_id);

/// 全量装载该账户 trades (过滤器基准).
std::vector<TradeRecord> load_trades(SQLite::Database& db, const std::string& account_id);

/// 增量装载: seq > since_seq 的 orders (重连重建基准, §4.3).
std::vector<OrderRecord> load_orders_since(SQLite::Database& db, const std::string& account_id,
                                           uint64_t since_seq);

/// 增量装载: seq > since_seq 的 trades (重连重建基准, §4.3).
std::vector<TradeRecord> load_trades_since(SQLite::Database& db, const std::string& account_id,
                                           uint64_t since_seq);

/// 对 accounts 列表全量预扫, 返回 account_id -> SessionBootData (start_seq = 实际 MAX).
/// 无历史账户得到空基准 (start_seq=0). 供 TdApi::set_configs 预扫缓存.
std::map<std::string, SessionBootData> prescan_accounts(
    SQLite::Database& db, const std::vector<std::string>& accounts);

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_PRESCAN_H_
