#ifndef DZTRADER_CTP_TD_OFFSET_CONVERTER_H_
#define DZTRADER_CTP_TD_OFFSET_CONVERTER_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <dztrader/core/core_struct.h>  // DzOrderReq
#include <dztrader/data_type.h>          // DzDirection/DzPositionEffect/DzDate
#include <dztrader/struct.h>             // DzPositionDetail/DzTradeReport

namespace dztrader::ctp {

// ============================================================================
// 交易所枚举 (OffsetConverter 用以决定转换模式)
// ============================================================================

/// 国内期货交易所枚举. 仅列出与 OffsetConverter 决策相关的交易所.
enum class Exchange : int8_t {
    Unknown,  // 未知/未识别
    SHFE,     // 上海期货交易所 (需平今昨拆分)
    INE,      // 上海国际能源交易中心 (需平今昨拆分)
    CFFEX,    // 中国金融期货交易所
    DCE,      // 大连商品交易所
    CZCE,     // 郑州商品交易所
    GFEX,     // 广州期货交易所
};

/// 从 DzExchangeId (char[16]) 解析交易所枚举.
/// 输入应为 null-terminated 字符串, 如 "SHFE"/"INE"/"CFFEX".
/// 未识别返回 Exchange::Unknown.
Exchange parse_exchange_id(const char* id);

// ============================================================================
// PositionHolding: 单合约持仓明细 + 冻结追踪
// ============================================================================

/// 单合约持仓持有状态. AccountSession 持有 map<instrument_id, PositionHolding>.
///
/// 数据来源:
/// - update_position_detail: CTP ReqQryInvestorPositionDetail 回调 (权威)
/// - update_from_trade: OnRtnTrade 回调 (OPEN 增今仓, CLOSE 减 frozen)
/// - update_order_frozen: 发单/撤单时更新 frozen (仅 CLOSE 类)
///
/// 持仓分今昨: open_date == trading_day_ 为今仓, 否则为昨仓.
class PositionHolding {
public:
    /// 构造. trading_day 为当前交易日 (DzDate, 即距纪元天数).
    /// AccountSession 在日切时调 set_trading_day 更新.
    PositionHolding(std::string instrument_id, Exchange exchange, int32_t trading_day);

    /// 更新持仓明细 (来自 CTP ReqQryInvestorPositionDetail 回调).
    /// 按 trade_id 索引存储, 重复更新同一 trade_id 覆盖旧值.
    void update_position_detail(const DzPositionDetail& detail);

    /// 从成交回报更新持仓 (来自 OnRtnTrade).
    /// - OPEN: 新增今仓 (合成 PositionDetail 加入 details_)
    /// - CLOSE/CLOSE_TODAY/CLOSE_YESTERDAY: 递减 frozen (挂起平仓单已成交)
    ///   注意: 实际持仓减少由下一次 ReqQryInvestorPositionDetail 回调反映, 不在此扣减.
    void update_from_trade(const DzTradeReport& trade);

    /// 更新冻结量 (发单/撤单时调用).
    /// 仅对 CLOSE/CLOSE_TODAY/CLOSE_YESTERDAY 类订单生效.
    /// is_sent=true: frozen += volume (发单); is_sent=false: frozen -= volume (撤单).
    void update_order_frozen(const DzOrderReq& req, bool is_sent);

    /// 清空所有持仓和冻结 (断线重连时调用, 重连后主动查询重建).
    void clear();

    /// 设置当前交易日 (日切时调用). 不自动重新计算今昨归属, 待下次 update_position_detail.
    void set_trading_day(int32_t trading_day) { trading_day_ = trading_day; }

    // --- 查询接口 ---
    const std::string& instrument_id() const { return instrument_id_; }
    Exchange exchange() const { return exchange_; }
    int32_t trading_day() const { return trading_day_; }

    int64_t long_today() const { return long_td_; }
    int64_t long_yesterday() const { return long_yd_; }
    int64_t short_today() const { return short_td_; }
    int64_t short_yesterday() const { return short_yd_; }

    /// 多头冻结总量 (今 + 昨), 用于查询/展示
    int64_t long_frozen() const { return long_frozen_today_ + long_frozen_yd_; }
    /// 空头冻结总量 (今 + 昨)
    int64_t short_frozen() const { return short_frozen_today_ + short_frozen_yd_; }
    /// 多头今仓冻结 (用于 SHFE 拆分)
    int64_t long_frozen_today() const { return long_frozen_today_; }
    /// 多头昨仓冻结
    int64_t long_frozen_yesterday() const { return long_frozen_yd_; }
    /// 空头今仓冻结
    int64_t short_frozen_today() const { return short_frozen_today_; }
    /// 空头昨仓冻结
    int64_t short_frozen_yesterday() const { return short_frozen_yd_; }

    /// 今多头可用 = long_today - long_frozen_today (用于 SHFE 拆分)
    int64_t long_available_today() const { return long_td_ - long_frozen_today_; }
    /// 今空头可用 = short_today - short_frozen_today
    int64_t short_available_today() const { return short_td_ - short_frozen_today_; }
    /// 昨多头可用 = long_yesterday - long_frozen_yesterday
    int64_t long_available_yesterday() const { return long_yd_ - long_frozen_yd_; }
    /// 昨空头可用 = short_yesterday - short_frozen_yesterday
    int64_t short_available_yesterday() const { return short_yd_ - short_frozen_yd_; }

private:
    /// 重新累加 details_ 计算今昨持仓. 在 update_position_detail 后调用.
    void recompute_totals();

    std::string instrument_id_;
    Exchange exchange_;
    int32_t trading_day_;

    /// 按 trade_id 索引的持仓明细. key = trade_id (string, 从 DzTradeId 转).
    std::unordered_map<std::string, DzPositionDetail> details_;

    int64_t long_td_ = 0;              // 今多头
    int64_t long_yd_ = 0;              // 昨多头
    int64_t short_td_ = 0;             // 今空头
    int64_t short_yd_ = 0;             // 昨空头
    int64_t long_frozen_today_ = 0;    // 多头今仓冻结 (CLOSE_TODAY 挂起单)
    int64_t long_frozen_yd_ = 0;       // 多头昨仓冻结 (CLOSE_YESTERDAY 挂起单)
    int64_t short_frozen_today_ = 0;   // 空头今仓冻结
    int64_t short_frozen_yd_ = 0;      // 空头昨仓冻结
};

// ============================================================================
// OffsetConverter + OffsetConvertMode + OrderSubRequest
// ============================================================================

/// 偏移转换模式 (决定 convert_order_request 的行为).
enum class OffsetConvertMode : int8_t {
    None,  // 不转换, 直接透传 (CFFEX/DCE/CZCE/GFEX 默认)
    Shfe,  // SHFE/INE 平今昨拆分
    Lock,  // 锁仓模式 (Close -> 反向开仓)
    Net,   // 净持仓模式 (行为同 None, 语义标记供未来扩展)
};

/// 拆分后的子订单请求 (AccountSession 转换为 CTP InputOrderField).
struct OrderSubRequest {
    DzDirection direction;
    DzPositionEffect position_effect;
    DzVolume volume;
    double price;
};

/// OffsetConverter: 根据持仓状态和模式转换订单请求.
/// 无状态 (仅依赖 PositionHolding 的当前快照), 线程安全 (只读).
class OffsetConverter {
public:
    /// 转换订单请求为子订单列表.
    /// - None/Net: 原样返回单条子订单
    /// - Shfe: CLOSE 拆分为 CLOSE_TODAY + CLOSE_YESTERDAY (按可用持仓)
    /// - Lock: CLOSE 转为 OPEN (反向开仓), 其他原样返回
    /// 返回空 vector 表示无法拆分 (持仓不足). 调用方应记录并通知.
    static std::vector<OrderSubRequest> convert_order_request(
        const DzOrderReq& req, const PositionHolding& holding, OffsetConvertMode mode);
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_OFFSET_CONVERTER_H_
