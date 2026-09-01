#ifndef DZTRADER_CTP_TD_CTP_MAPPING_H_
#define DZTRADER_CTP_TD_CTP_MAPPING_H_

#include <cstdint>
#include <string>

#include <ThostFtdcUserApiStruct.h>

#include <dztrader/core/core_struct.h>  // DzOrderReq
#include <dztrader/data_type.h>         // DzOrderStatus/DzPriceType
#include <dztrader/struct.h>            // DzInstrumentInfo/DzTradingAccount

#include "td/td_schema.h"  // OrderRecord/TradeRecord (组合扩展 strategy_api POD)

namespace dztrader::ctp {

/// CTP 价格类型三元组 (LimitPriceType + TimeCondition + VolumeCondition).
/// 三个字段共同决定 CTP 下单语义, 不能单独使用.
struct OrderTypeTriplet {
    char limit_price_type;   ///< 'LimitPrice' / 'AnyPrice' / 'BestPrice' 等
    char time_condition;     ///< 'GFD' / 'IOC' 等
    char volume_condition;   ///< 'AnyVolume' / 'MinVolume' / 'CV' 等
};

/// 下单时构建 CTP InputOrderField 所需的上下文.
/// AccountSession 维护 order_ref 递增序列, 转换时通过此结构传入.
struct OrderBuildContext {
    std::string account_id;   ///< 投资者代码 (CTP InvestorID)
    int64_t order_ref = 0;    ///< CTP OrderRef (12 位数字字符串)
    int32_t request_id = 0;   ///< CTP RequestID
    std::string exchange_id;  ///< 交易所代码 (CFFEX/SHFE/...)
};

// ============================================================================
// 纯函数: 时间/日期解析, 品种归一化
// ============================================================================

/// 解析 CTP 时间 "HH:MM:SS" 为距午夜秒数. 失败返回 -1.
int32_t parse_ctp_time(const char* hh_mm_ss) noexcept;

/// 解析 CTP 日期 "YYYYMMDD" 为距纪元天数 (DzDate). 失败返回 -1.
int32_t parse_ctp_date(const char* yyyymmdd) noexcept;

/// 品种归一化: 从合约 ID 提取品种代码 (如 "IF2506" -> "IF", "rb2510" -> "rb").
/// 规则: 取首个数字之前的前缀. 无数字则原样返回. 期权如 "SR509C4800" -> "SR".
std::string normalize_to_product(const std::string& instrument_id);

// ============================================================================
// 纯函数: 枚举映射
// ============================================================================

/// CTP OrderStatus -> DzOrderStatus 映射 (设计 §12.3).
/// 未知状态返回 DZ_ORDER_SUBMITTING (安全兜底, 等待下次回报).
DzOrderStatus STATUS_CTP2VT(char ctp_status) noexcept;

/// DzPriceType -> CTP 价格类型三元组.
/// DZ_PRICE_LIMIT  -> LimitPrice + GFD + MinVolume
/// DZ_PRICE_MARKET -> AnyPrice  + IOC + AnyVolume
/// DZ_PRICE_FAK    -> LimitPrice + IOC + MinVolume
/// DZ_PRICE_FOK    -> LimitPrice + IOC + CV (MinVolume=VolumeTotalOriginal, 由 to_input_order_field 填)
/// 其他类型 (STOP/RFQ) 期货不支持, 返回 LimitPrice 兜底.
OrderTypeTriplet ORDERTYPE_VT2CTP(DzPriceType t) noexcept;

// ============================================================================
// 纯函数: 结构体转换
// ============================================================================

/// DZ 下单请求 -> CTP InputOrderField (设计 §12.3).
/// - CombOffsetFlag[0] 由 req.position_effect 映射
/// - Direction 由 req.direction 映射
/// - OrderPriceType/TimeCondition/VolumeCondition 由 ORDERTYPE_VT2CTP 决定
/// - OrderRef 由 ctx.order_ref 转为 12 位字符串
/// - FOK 模拟: MinVolume = req.volume (配合 CV 实现"全部成交否则撤销")
/// - LimitPrice 直接拷贝 req.price (市价单时 CTP 忽略此字段)
CThostFtdcInputOrderField to_input_order_field(const DzOrderReq& req,
                                                const OrderBuildContext& ctx) noexcept;

/// CTP OrderField -> OrderRecord (含 DzOrderReport base + CTP 扩展字段).
/// trading_day 为 DzDate (距纪元天数), 由 AccountSession 传入.
/// order_id 留 0, 由 AccountSession 查 OrderRefMap 后填到 base.order_id.
/// is_external 留 0, 由 AccountSession 根据 OrderRefMap 命中情况覆写.
/// volume_canceled 留 0 (CThostFtdcOrderField 无此字段), AccountSession 在
/// on_rtn_order 中根据 OrderStatus 推导: CANCELLED 状态时 = volume - volume_traded.
OrderRecord to_order_record(const CThostFtdcOrderField& o,
                             const std::string& account_id,
                             int32_t trading_day) noexcept;

/// CTP TradeField -> TradeRecord (含 DzTradeReport base + 扩展字段).
/// commission 留 0, 由 AccountSession 后续查询 CommissionRate 后填.
TradeRecord to_trade_record(const CThostFtdcTradeField& t,
                             const std::string& account_id,
                             int32_t trading_day) noexcept;

/// CTP InstrumentField -> DzInstrumentInfo (字段一一映射).
/// 注意: CTP InstrumentName 为 GBK 编码, 此处原样拷贝 (后续如需 UTF-8 由调用方转换).
/// CTP 无 ListedDate 字段, 用 OpenDate (上市日) 映射到 option_listed.
DzInstrumentInfo to_dz_instrument(const CThostFtdcInstrumentField& f) noexcept;

/// CTP TradingAccountField -> DzTradingAccount.
/// trading_day 为 DzDate (距纪元天数).
DzTradingAccount to_dz_trading_account(const CThostFtdcTradingAccountField& a,
                                        const std::string& account_id,
                                        int32_t trading_day) noexcept;

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_CTP_MAPPING_H_
