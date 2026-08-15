#include "td/td_risk_gate.h"

#include <cmath>
#include <string>

namespace dztrader::ctp {

RiskGate::RiskGate(bool enabled, RiskConfig config)
    : enabled_(enabled), config_(config) {}

void RiskGate::set_enabled(bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_release);
}

bool RiskGate::enabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
}

std::optional<RiskReject> RiskGate::check_order(const DzOrderReq& req,
                                                 const AccountContext& ctx) noexcept {
    // 关闭时 [[likely]] 分支预测, 几乎零开销
    if (!enabled_.load(std::memory_order_acquire)) [[likely]] {
        return std::nullopt;
    }

    // 规则 1: 单笔数量上限
    if (config_.max_order_volume > 0 && req.volume > config_.max_order_volume) {
        RiskReject r;
        r.rule_name = "max_order_volume";
        r.reason = "单笔数量 " + std::to_string(req.volume) + " 超过上限 " +
                   std::to_string(config_.max_order_volume);
        return r;
    }

    // 规则 2: 价格必须是 price_tick 整数倍
    if (config_.check_price_tick && ctx.price_tick > 0.0) {
        double ratio = req.price / ctx.price_tick;
        double rounded = std::round(ratio);
        // 浮点容差: 1e-9 (price_tick 通常是 0.1/0.2/1.0 等小数)
        if (std::abs(ratio - rounded) > 1e-9) {
            RiskReject r;
            r.rule_name = "price_tick";
            r.reason = "价格 " + std::to_string(req.price) + " 不是最小变动价位 " +
                       std::to_string(ctx.price_tick) + " 的整数倍";
            return r;
        }
    }

    return std::nullopt;
}

std::optional<RiskReject> RiskGate::check_cancel(const DzOrderReq& req,
                                                  const AccountContext& ctx) noexcept {
    (void)req;
    (void)ctx;
    return std::nullopt;
}

}  // namespace dztrader::ctp
