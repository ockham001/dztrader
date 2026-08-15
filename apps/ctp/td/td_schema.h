#ifndef DZTRADER_CTP_TD_SCHEMA_H_
#define DZTRADER_CTP_TD_SCHEMA_H_

#include <cstdint>

#include <dztrader/db/migration.h>
#include <dztrader/struct.h>  // DzOrderReport/DzTradeReport/DzMarginRate/DzCommissionRate/DzContract

namespace dztrader::ctp {

/// TD schema 当前版本 (每次表结构变更递增).
/// v1: 初始版本 (orders/trades/margin_rates/commission_rates/instruments).
constexpr int kTdSchemaVersion = 1;

// ============================================================================
// SQL-ready POD 记录: 复用 strategy_api 结构体 + 组合扩展 SQL 特有字段
// 设计: strategy_api 的 DzOrderReport/DzTradeReport 等是 SHM 帧格式, 缺少
// CTP 特有字段 (order_ref/external_order_id/error_msg 等). 用组合方式扩展,
// 避免字段重复定义, 便于 SHM 帧与 SQL 记录互转.
// ============================================================================

/// 委托记录 (对应 orders 表, 去重 key = account_id + order_id).
/// DzOrderReport 是 SHM 帧格式, 缺少 CTP 特有字段.
struct OrderRecord {
    DzOrderReport base;            ///< SHM 帧字段 (order_id/strategy_id/instrument_id/...)
    char trading_day[9];           ///< "YYYYMMDD" 文本 (SQL 列, base.date 是 DzDate)
    char order_ref[13];            ///< CTP OrderRef (12 位 + null)
    char external_order_id[21];    ///< CTP OrderSysID (20 位 + null)
    int8_t is_external;            ///< 是否外部订单 (CTP 返回的, 非本系统发出)
    char reserved[7];              ///< 对齐 int32_t volume_canceled 到 8 字节边界
    int32_t volume_canceled;       ///< 已撤数量
    int64_t insert_time;           ///< epoch seconds (CTP InsertTime)
    int64_t update_time;           ///< epoch seconds (CTP UpdateTime)
    int32_t error_id;              ///< CTP ErrorID
    char error_msg[128];           ///< CTP ErrorMsg (GBK->UTF8)
};

/// 成交记录 (对应 trades 表, 去重 key = account_id + trade_id).
/// DzTradeReport 缺少 commission/trade_time(秒)/trade_date(YYYYMMDD int).
struct TradeRecord {
    DzTradeReport base;            ///< SHM 帧字段
    char trading_day[9];           ///< "YYYYMMDD" 文本
    int64_t trade_time;            ///< epoch seconds
    int64_t trade_date;            ///< YYYYMMDD as int
    double commission;             ///< 手续费
};

/// 保证金率记录 (直接复用 DzMarginRate, 字段完全匹配).
using MarginRateRecord = DzMarginRate;

/// 手续费率记录 (直接复用 DzCommissionRate, 字段完全匹配).
using CommissionRateRecord = DzCommissionRate;

/// 合约信息记录 (对应 instruments 表).
/// DzContract 缺少 update_day 字段.
struct InstrumentRecord {
    DzContract base;               ///< SHM 帧字段
    char update_day[9];            ///< "YYYYMMDD" 文本 (合约信息更新日)
};

/// 注册所有 TD migration 到 MigrationManager (v1 起步).
/// 调用方: PersistWriter::open() 中, 先 mgr.apply(db) 再使用表.
void apply_td_migrations(dztrader::db::MigrationManager& mgr);

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_SCHEMA_H_
