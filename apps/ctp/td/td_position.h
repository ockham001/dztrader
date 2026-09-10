#ifndef DZTRADER_CTP_TD_POSITION_H_
#define DZTRADER_CTP_TD_POSITION_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <dztrader/data_type.h>  // DzDirection/DzPositionEffect/DzOrderStatus
#include <dztrader/struct.h>     // DzTradeReport

namespace dztrader::ctp {

// ============================================================================
// 交易所枚举
// ============================================================================

/// 国内期货交易所枚举. 用于平今昨拆分与 generic CLOSE 归属决策.
enum class Exchange : int8_t {
    Unknown,  // 未知/未识别
    SHFE,     // 上海期货交易所 (generic CLOSE 平昨)
    INE,      // 上海国际能源交易中心 (generic CLOSE 平昨)
    CFFEX,    // 中国金融期货交易所
    DCE,      // 大连商品交易所
    CZCE,     // 郑州商品交易所
    GFEX,     // 广州期货交易所
};

/// 从交易所代码字符串解析枚举. 未识别或 null 返回 Exchange::Unknown.
Exchange parse_exchange_id(const char* id) noexcept;

/// 委托单是否终态 (全部成交/已撤销/拒单). 终态挂单不再冻结持仓.
bool is_terminal_order_status(DzOrderStatus status) noexcept;

// ============================================================================
// 聚合持仓模型
// ============================================================================

/// 单方向聚合持仓. today/yd 的量价与冻结均按方向独立维护.
struct PositionSide {
    int64_t today = 0;        // 今仓
    int64_t yd = 0;           // 昨仓
    double price = 0.0;       // 持仓均价 (weighted by volume)
    int64_t frozen_td = 0;    // 今仓冻结
    int64_t frozen_yd = 0;    // 昨仓冻结
    uint64_t seq = 0;         // 最近一次该方向持仓变更序号

    /// 总持仓 = today + yd
    int64_t volume() const noexcept { return today + yd; }
    /// 总冻结 = frozen_td + frozen_yd
    int64_t frozen() const noexcept { return frozen_td + frozen_yd; }
};

/// 一次操作引起的方向变更标记. 用于上层按需刷帧.
struct SideChange {
    bool long_changed = false;
    bool short_changed = false;

    bool any() const noexcept { return long_changed || short_changed; }
};

/// 活动委托单 (未终态且非 OPEN) 的持仓冻结视图.
/// instrument_id 必需: 防止跨合约冻结污染.
struct ActiveOrderUpdate {
    std::string instrument_id;                      // 必需: 防止跨合约冻结污染
    std::string order_ref;
    DzDirection direction = DZ_DIRECTION_NET;       // 卖→平多, 买→平空
    DzPositionEffect effect = DZ_POSITION_EFFECT_OPEN;
    DzOrderStatus status = DZ_ORDER_SUBMITTING;
    int64_t volume = 0;
    int64_t volume_traded = 0;
};

/// 单合约聚合持仓. 由查询快照 (绝对态) 与成交/委托回报 (增量) 共同驱动.
///
/// 数据来源:
/// - apply_query_side: 周期持仓查询, 按方向覆盖绝对量
/// - apply_trade: OnRtnTrade, OPEN 加权增今仓 / 平仓递减今昨
/// - apply_order / rebuild_active_orders: 委托回报, 重算挂单冻结 (含夹取)
/// - on_day_switch: 日切, 今仓转昨仓并清空冻结与活动委托
///
/// 纯逻辑模型: 不依赖 dzdb/CTP, 帧推送由 AccountSession 负责.
class PositionHolding {
public:
    PositionHolding(std::string instrument_id, std::string exchange_id);

    /// 查询快照按方向覆盖持仓 (volume/yd 为绝对量, yd 夹取到 [0, volume]).
    /// 查询缩量后立即夹取冻结, 避免推送 frozen > volume.
    SideChange apply_query_side(DzDirection dir, int64_t volume, int64_t yd, double price);

    /// 成交回报驱动持仓增量. OPEN 按成交量加权更新均价并增今仓;
    /// 平仓递减对应方向今昨 (generic CLOSE: SHFE/INE 平昨, 其他先今后昨溢出).
    SideChange apply_trade(const DzTradeReport& trade);

    /// 单笔活动委托单更新 (发单/成交推进/撤单). 终态或 OPEN 则移除冻结.
    SideChange apply_order(const ActiveOrderUpdate& order);

    /// 用快照整体重建活动委托集合 (周期查询/重连后调用, 幂等).
    SideChange rebuild_active_orders(const std::vector<ActiveOrderUpdate>& orders);

    /// 日切: 今仓转昨仓, 清空冻结与活动委托. 价格保留.
    SideChange on_day_switch() noexcept;

    /// 清空全部持仓与冻结 (断线重连后主动查询重建).
    void clear() noexcept;

    /// 设置方向持仓的最近变更序号.
    void set_seq(DzDirection dir, uint64_t seq) noexcept;

    // --- 查询接口 ---
    const std::string& instrument_id() const noexcept { return instrument_id_; }
    const std::string& exchange_id() const noexcept { return exchange_id_; }
    Exchange exchange() const noexcept { return exchange_; }
    const PositionSide& side(DzDirection dir) const noexcept;

    int64_t long_available_today() const noexcept;
    int64_t short_available_today() const noexcept;
    int64_t long_available_yesterday() const noexcept;
    int64_t short_available_yesterday() const noexcept;

    // OffsetConverter 兼容读取
    int64_t long_today() const noexcept;
    int64_t long_yesterday() const noexcept;
    int64_t short_today() const noexcept;
    int64_t short_yesterday() const noexcept;
    int64_t long_frozen() const noexcept;
    int64_t short_frozen() const noexcept;
    int64_t long_frozen_today() const noexcept;
    int64_t long_frozen_yesterday() const noexcept;
    int64_t short_frozen_today() const noexcept;
    int64_t short_frozen_yesterday() const noexcept;

private:
    struct ActiveOrder {
        DzDirection direction = DZ_DIRECTION_NET;
        DzPositionEffect effect = DZ_POSITION_EFFECT_OPEN;
        int64_t volume = 0;
        int64_t traded = 0;
    };

    PositionSide& mutable_side(DzDirection dir) noexcept;
    void recompute_frozen() noexcept;

    std::string instrument_id_;
    std::string exchange_id_;
    Exchange exchange_ = Exchange::Unknown;
    PositionSide long_;
    PositionSide short_;
    std::unordered_map<std::string, ActiveOrder> active_orders_;  // key = order_ref
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_POSITION_H_
