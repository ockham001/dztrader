#ifndef DZTRADER_CTP_TD_RISK_GATE_H_
#define DZTRADER_CTP_TD_RISK_GATE_H_

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include <dztrader/core/core_struct.h>  // DzOrderReq
#include <dztrader/data_type.h>         // DzVolume/DzDirection

namespace dztrader::ctp {

/// 风控配置 (设计 §8.1, 仅基本规则)
struct RiskConfig {
    int32_t max_order_volume = 0;   ///< 单笔数量上限, 0=不检查
    bool check_price_tick = true;   ///< 价格必须是 price_tick 整数倍
};

/// 账户上下文 (风控检查时由 AccountSession 传入快照)
struct AccountContext {
    std::string account_id;
    double price_tick = 0.0;        ///< 最小变动价位
    int32_t long_pos = 0;           ///< 多头持仓 (今+昨)
    int32_t short_pos = 0;          ///< 空头持仓
};

/// 风控拒绝信息 (与 SHM 帧的 DzRiskReject 区分, 此处用 std::string 便于日志)
struct RiskReject {
    std::string rule_name;          ///< 规则名 (如 "max_order_volume")
    std::string reason;             ///< 拒绝原因 (中文)
};

/// 可开关的风控钩子 (设计 §8)
///
/// 线程安全: enabled_ 用 atomic 保证跨线程开关安全, 可在任意线程 set_enabled,
/// 主线程 check_order (place_order 调用, td_account_session.cpp:298).
/// 关闭时 [[likely]] 分支预测, 几乎零开销.
class RiskGate {
public:
    explicit RiskGate(bool enabled = false, RiskConfig config = {});

    void set_enabled(bool enabled) noexcept;
    bool enabled() const noexcept;

    /// 下单风控检查. 通过返回 nullopt, 拒绝返回 RiskReject.
    std::optional<RiskReject> check_order(const DzOrderReq& req,
                                           const AccountContext& ctx) noexcept;

    /// 撤单风控检查 (当前总是通过, 留作未来扩展)
    std::optional<RiskReject> check_cancel(const DzOrderReq& req,
                                           const AccountContext& ctx) noexcept;

private:
    std::atomic<bool> enabled_;
    RiskConfig config_;
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_RISK_GATE_H_
