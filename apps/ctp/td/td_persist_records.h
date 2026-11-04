#ifndef DZTRADER_CTP_TD_PERSIST_RECORDS_H_
#define DZTRADER_CTP_TD_PERSIST_RECORDS_H_

#include <cstdint>

#include <dztrader/struct.h>  // DzOrderReport/DzTradeReport

namespace dztrader::ctp {

// ============================================================================
// orders.ext JSON 键（schema v1 柜台扩展列；写端 td_persist_rows.cpp 与读端
// td_prescan.cpp 共用同一组常量，禁止两处各写一份字面量）
// ============================================================================
inline constexpr char kOrderExtOrderRef[] = "order_ref";
inline constexpr char kOrderExtOrderSysId[] = "order_sys_id";
inline constexpr char kOrderExtVolumeCanceled[] = "volume_canceled";
inline constexpr char kOrderExtUpdateTime[] = "update_time";
inline constexpr char kOrderExtErrorId[] = "error_id";
inline constexpr char kOrderExtErrorMessage[] = "error_msg";

// ============================================================================
// SQL-ready POD 记录: 复用 strategy_api 结构体 + 组合扩展 SQL 特有字段
// 设计: strategy_api 的 DzOrderReport/DzTradeReport 等是 SHM 帧格式, 缺少
// CTP 特有字段 (order_ref/external_order_id/error_msg 等). 用组合方式扩展,
// 避免字段重复定义, 便于 SHM 帧与 SQL 记录互转.
// 合约记录 (InstrumentRecord) 由 libs/tdstore 提供 (后端无关规范记录).
// ============================================================================

/// 委托记录 (对应 orders 表, 去重 key = account_id + order_id).
/// DzOrderReport 是 SHM 帧格式, 缺少 CTP 特有字段.
struct OrderRecord {
    DzOrderReport base;          ///< SHM 帧字段 (order_id/strategy_id/instrument_id/...)
    char trading_day[9];         ///< "YYYYMMDD" 文本 (schema v1 date 列, base.date 是 DzDate)
    char order_ref[13];          ///< CTP OrderRef (12 位 + null)
    char external_order_id[21];  ///< CTP OrderSysID (20 位 + null)
    int8_t is_external;          ///< 是否外部订单 (CTP 返回的, 非本系统发出)
    char reserved[7];            ///< 对齐 int32_t volume_canceled 到 8 字节边界
    int32_t volume_canceled;     ///< 已撤数量
    int64_t insert_time;         ///< epoch seconds (CTP InsertTime)
    int64_t update_time;         ///< epoch seconds (CTP UpdateTime)
    int32_t error_id;            ///< CTP ErrorID
    char error_msg[128];         ///< CTP ErrorMsg (GBK->UTF8)
};

/// 成交记录 (对应 trades 表, 去重 key = account_id + date + exchange_id + trade_id).
/// 注: trade_time / trade_date / commission 在 schema v1 已无对应列（持仓/重放基准只用
/// trading_day + trade_id）；保留字段供 td_ctp_mapping 未迁移路径使用，#245 登录日装载
/// 重构时随 td_prescan 一并删除。
struct TradeRecord {
    DzTradeReport base;   ///< SHM 帧字段
    char trading_day[9];  ///< "YYYYMMDD" 文本 (schema v1 date 列)
    int64_t trade_time;   ///< epoch seconds (schema v1 无列; 待 #245 删除)
    int64_t trade_date;   ///< YYYYMMDD as int (schema v1 无列; 待 #245 删除)
    double commission;    ///< 手续费 (schema v1 无列; 待 #245 删除)
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_PERSIST_RECORDS_H_
