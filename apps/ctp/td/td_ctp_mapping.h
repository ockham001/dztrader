#ifndef DZTRADER_CTP_TD_CTP_MAPPING_H_
#define DZTRADER_CTP_TD_CTP_MAPPING_H_

#include <cstdint>
#include <string>
#include <string_view>

#include <ThostFtdcUserApiStruct.h>

#include <dztrader/core/core_struct.h>  // DzOrderReq
#include <dztrader/data_type.h>         // DzOrderStatus/DzPriceType
#include <dztrader/struct.h>            // DzTradingAccount
#include <dztrader/tdstore/records.h>   // tdstore::InstrumentRecord

#include "td/td_persist_records.h"  // OrderRecord/TradeRecord (组合扩展 strategy_api POD)

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

/// 解析 CTP 日期 "YYYYMMDD" 为距纪元天数 (DzDate). 非法/空输入返回 DZ_DATE_NA (0).
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
/// commission 留 0（平台不查询费率，ADR 0013）.
TradeRecord to_trade_record(const CThostFtdcTradeField& t,
                             const std::string& account_id,
                             int32_t trading_day) noexcept;

/// CTP InstrumentField -> tdstore 规范记录 (name 转 UTF-8; update_day/updated_at 由调用方填).
/// CTP 无 ListedDate 字段, 用 OpenDate (上市日) 映射到 listed_date; ExpireDate 映射到
/// delisted_date (非法/空输入经 parse_ctp_date 回退为 DZ_DATE_NA).
tdstore::InstrumentRecord to_instrument_record(const CThostFtdcInstrumentField& f,
                                               const std::string& update_day) noexcept;

/// symbol -> CTP 单合约查询字段 (仅 InstrumentID 非空, 其余零初始化).
/// 用于 DZ_FRAME_TD_QUERY_INSTRUMENT 定向刷新: DB 行的 symbol (CZCE 人工消歧) 优先.
CThostFtdcQryInstrumentField to_qry_instrument_field(std::string_view symbol) noexcept;

/// CTP TradingAccountField -> DzTradingAccount.
/// trading_day 为 DzDate (距纪元天数).
DzTradingAccount to_dz_trading_account(const CThostFtdcTradingAccountField& a,
                                        const std::string& account_id,
                                        int32_t trading_day) noexcept;

/// CTP InvestorPositionField -> DzPositionInfo (spec §4.2 2002 写端).
/// trading_day 为 DzDate (距纪元天数).
/// PosiDirection: 净('1') 归为多头 (持仓方向仅多/空两态); 多('2') -> LONG, 空('3') -> SHORT.
/// 持仓量取 Position (今日持仓; CTP 该字段在持仓查询响应中为总持仓口径).
/// volume_multiple 为合约乘数 (CTP PositionCost 是金额, 非单价):
/// 均价 = PositionCost / (Position × volume_multiple); Position 或乘数 <= 0 时留 0.
DzPositionInfo to_dz_position(const CThostFtdcInvestorPositionField& p,
                               const std::string& account_id,
                               int32_t trading_day,
                               double volume_multiple) noexcept;

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_CTP_MAPPING_H_
