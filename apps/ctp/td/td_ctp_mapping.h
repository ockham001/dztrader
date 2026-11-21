#ifndef DZTRADER_CTP_TD_CTP_MAPPING_H_
#define DZTRADER_CTP_TD_CTP_MAPPING_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

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
    char limit_price_type;  ///< 'LimitPrice' / 'AnyPrice' / 'BestPrice' 等
    char time_condition;    ///< 'GFD' / 'IOC' 等
    char volume_condition;  ///< 'AnyVolume' / 'MinVolume' / 'CV' 等
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
// 纯函数: 时间/日期解析
// ============================================================================

/// 解析 CTP 时间 "HH:MM:SS" 为距午夜秒数. 失败返回 -1.
int32_t parse_ctp_time(const char* hh_mm_ss) noexcept;

/// 解析 CTP 日期 "YYYYMMDD" 为距纪元天数 (DzDate). 非法/空输入返回 DZ_DATE_NA (0).
int32_t parse_ctp_date(const char* yyyymmdd) noexcept;

// ============================================================================
// 纯函数: 枚举映射
// ============================================================================

/// 未知枚举的分类标签 (日志 k=v 与状态帧 ext 计数器键名的唯一拼写来源).
/// 改动会同时改到《帧契约：交易网关状态》§2.3 的 ext 字段名, 不是随手可改的字符串.
inline constexpr const char* kUnknownCategoryOrderStatus = "unknown_order_status";
inline constexpr const char* kUnknownCategoryDirection = "unknown_direction";
inline constexpr const char* kUnknownCategoryPriceType = "unknown_price_type";
inline constexpr const char* kUnknownCategoryPositionEffect = "unknown_position_effect";

/// 一条未知枚举的取证信息 (报告方向专用): 哪一类 + 柜台原始字符.
/// `category` 是分类标签的唯一拼写来源, 落日志与状态帧计数器键名都用它.
struct UnknownEnumInfo {
    const char* category = nullptr;  ///< unknown_order_status / unknown_direction / ...
    char raw_char = '\0';            ///< 柜台原始字符 (CTP 侧枚举是 char 字面量)
};

/// 一条回报里可能同时出现多个未知枚举 (方向 + 开平 + 价格类型 + 状态), 定长收集避免
/// noexcept 路径分配. **超出容量的取证丢弃且不计数**(report_unknown_enums 的计数正是遍历
/// 本 sink): 当前 kCapacity=4 已覆盖单条回报的最坏情况 (to_order_record 最多同时产出
/// direction + position_effect + price_type + status 四条), 溢出不发生.
struct UnknownEnumSink {
    static constexpr size_t kCapacity = 4;

    UnknownEnumInfo items[kCapacity]{};
    size_t count = 0;

    /// 登记一条未知枚举 (容量满则忽略).
    void add(const char* category, char raw_char) noexcept {
        if (count < kCapacity) {
            items[count++] = UnknownEnumInfo{category, raw_char};
        }
    }

    [[nodiscard]] bool empty() const noexcept { return count == 0; }
};

/// CTP OrderStatus -> DzOrderStatus 映射 (设计 §12.3).
/// 未知状态返回 DZ_ORDER_UNKNOWN (不猜成任何真实状态: 猜成 SUBMITTING 会让委托永久停在
/// 非终态、冻结持仓不释放), 并把原始字符登记到 `unknown`.
DzOrderStatus STATUS_CTP2VT(char ctp_status, UnknownEnumSink* unknown = nullptr) noexcept;

/// DzPriceType -> CTP 价格类型三元组. 不支持的类型返回 nullopt —— **绝不兜底**:
/// 兜底等于把一张止损/询价单当普通限价单发给交易所.
/// DZ_PRICE_LIMIT  -> LimitPrice + GFD + MinVolume
/// DZ_PRICE_MARKET -> AnyPrice  + IOC + AnyVolume
/// DZ_PRICE_FAK    -> LimitPrice + IOC + MinVolume
/// DZ_PRICE_FOK    -> LimitPrice + IOC + CV (MinVolume=VolumeTotalOriginal, 由 to_input_order_field
/// 填)
std::optional<OrderTypeTriplet> ORDERTYPE_VT2CTP(DzPriceType t) noexcept;

// ============================================================================
// 纯函数: 结构体转换
// ============================================================================

/// DZ 下单请求 -> CTP InputOrderField (设计 §12.3).
/// 不支持的 price_type / position_effect / direction 返回 nullopt —— 出账方向**绝不兜底**
/// (兜底等于把错误指令发给交易所), 由调用方本地拒单.
/// - CombOffsetFlag[0] 由 req.position_effect 映射
/// - Direction 由 req.direction 映射 (只接受 LONG/SHORT; NET 属内部持仓侧标签, 不是报单方向)
/// - OrderPriceType/TimeCondition/VolumeCondition 由 ORDERTYPE_VT2CTP 决定
/// - OrderRef 由 ctx.order_ref 转为 12 位字符串
/// - FOK 模拟: MinVolume = req.volume (配合 CV 实现"全部成交否则撤销")
/// - LimitPrice 直接拷贝 req.price (市价单时 CTP 忽略此字段)
std::optional<CThostFtdcInputOrderField> to_input_order_field(
    const DzOrderReq& req, const OrderBuildContext& ctx) noexcept;

/// CTP OrderField -> OrderRecord (含 DzOrderReport base + CTP 扩展字段).
/// trading_day 为 DzDate (距纪元天数), 由 AccountSession 传入.
/// order_id 留 0, 由 AccountSession 查 OrderRefMap 后填到 base.order_id.
/// is_external 留 0, 由 AccountSession 根据 OrderRefMap 命中情况覆写.
/// volume_canceled 留 0 (CThostFtdcOrderField 无此字段), AccountSession 在
/// on_rtn_order 中根据 OrderStatus 推导: CANCELLED 状态时 = volume - volume_traded.
/// 未知的方向/开平/价格类型/状态一律落成对应的 UNKNOWN 枚举, 原始字符登记到 `unknown`.
OrderRecord to_order_record(const CThostFtdcOrderField& o,
                            const std::string& account_id,
                            int32_t trading_day,
                            UnknownEnumSink* unknown = nullptr) noexcept;

/// CTP TradeField -> TradeRecord (含 DzTradeReport base + 扩展字段).
/// commission 留 0（平台不查询费率，ADR 0013）.
/// 未知的开平标志落成 DZ_POSITION_EFFECT_UNKNOWN, 原始字符登记到 `unknown`.
TradeRecord to_trade_record(const CThostFtdcTradeField& t,
                            const std::string& account_id,
                            int32_t trading_day,
                            UnknownEnumSink* unknown = nullptr) noexcept;

/// CTP InstrumentField -> tdstore 规范记录 (name 转 UTF-8; update_day/updated_at 由调用方填).
/// CTP 无 ListedDate 字段, 用 OpenDate (上市日) 映射到 listed_date; ExpireDate 映射到
/// delisted_date (非法/空输入经 parse_ctp_date 回退为 DZ_DATE_NA).
tdstore::InstrumentRecord to_instrument_record(const CThostFtdcInstrumentField& f,
                                               const std::string& update_day) noexcept;

/// CTP TradingAccountField -> DzTradingAccount.
/// trading_day 为 DzDate (距纪元天数).
DzTradingAccount to_dz_trading_account(const CThostFtdcTradingAccountField& a,
                                       const std::string& account_id,
                                       int32_t trading_day) noexcept;

/// CTP InvestorPositionField -> DzPositionInfo (spec §4.2 2002 写端).
/// trading_day 为 DzDate (距纪元天数).
/// PosiDirection: 净('1') 归为多头 (持仓方向仅多/空两态); 多('2') -> LONG, 空('3') -> SHORT;
/// 未知方向 -> DZ_DIRECTION_UNKNOWN 并把原始字符登记到 `unknown` (绝不猜成多头——那会把
/// 空头写成多头, 净敞口符号反向).
/// 持仓量取 Position (该行的当前总持仓, 含今昨; 负值坏数据夹到 0).
/// 今昨口径 (ADR 0049 / 契约 td-data-sync §3): today = TodayPosition (夹到 [0, Position]),
/// yd = Position - today; YdPosition 是交易日开始时的静态昨仓 (平昨后不减少), 不参与计算.
/// volume_multiple 为合约乘数 (CTP PositionCost 是金额, 非单价):
/// 均价 = PositionCost / (Position × volume_multiple); Position 或乘数 <= 0 时留 0.
DzPositionInfo to_dz_position(const CThostFtdcInvestorPositionField& p,
                              const std::string& account_id,
                              int32_t trading_day,
                              double volume_multiple,
                              UnknownEnumSink* unknown = nullptr) noexcept;

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_CTP_MAPPING_H_
