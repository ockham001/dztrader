/**
 * @file trade_db.h
 * @brief master 启动期交易库初始化：建库/重建 + 共享 order_id 计数器播种（ADR 0041/0042）
 */
#ifndef DZTRADER_MASTER_TRADE_DB_H_
#define DZTRADER_MASTER_TRADE_DB_H_

#include <cstdint>

#include <dztrader/data_type.h>
#include <dztrader/tdstore/trade_db.h>

namespace dztrader::master {

/// master 启动期交易库初始化结果（日志用；失败抛异常由调用方降级）
struct TradeDbStartupResult {
    tdstore::TradeDbInitReport init;   ///< 建库/重建报告
    std::int64_t db_max_order_id = 0;  ///< 库内 orders MAX(order_id)
    DzOrderId counter_before = 0;      ///< 播种前共享计数器值
    DzOrderId counter_after = 0;       ///< 播种后共享计数器值
};

/// 建库/重建 trade.db 并按库内 MAX(order_id) 播种共享 shm 计数器（幂等）。
/// 库不可用（磁盘/权限/损坏且改名失败）时抛异常——调用方记录 ERROR 并降级启动。
/// 成功返回后共享计数器 ≥ db_max_order_id + 1（ensure_at_least 单调不减）。
TradeDbStartupResult initialize_trade_db();

}  // namespace dztrader::master

#endif  // DZTRADER_MASTER_TRADE_DB_H_
