#include "td/td_risk_gate.h"

#include <gtest/gtest.h>

using namespace dztrader::ctp;

namespace {

/// 辅助: 构造一个最小可用的 DzOrderReq
DzOrderReq make_req(double price, DzVolume volume,
                    DzDirection dir = DZ_DIRECTION_LONG) {
    DzOrderReq r{};
    r.price = price;
    r.volume = volume;
    r.direction = dir;
    return r;
}

}  // namespace

// ============================================================================
// enabled / set_enabled
// ============================================================================

TEST(RiskGateTest, DisabledReturnsNullopt) {
    RiskGate g(false);
    AccountContext ctx;
    EXPECT_FALSE(g.check_order(make_req(100.0, 10), ctx).has_value());
}

TEST(RiskGateTest, EnabledReturnsNulloptWhenNoViolation) {
    RiskConfig cfg;
    cfg.max_order_volume = 100;
    RiskGate g(true, cfg);
    AccountContext ctx;
    ctx.price_tick = 0.1;
    EXPECT_FALSE(g.check_order(make_req(100.0, 10), ctx).has_value());
}

TEST(RiskGateTest, SetEnabledToggle) {
    RiskConfig cfg;
    cfg.max_order_volume = 100;
    RiskGate g(false, cfg);
    AccountContext ctx;
    EXPECT_FALSE(g.enabled());
    EXPECT_FALSE(g.check_order(make_req(100.0, 999), ctx).has_value());

    g.set_enabled(true);
    EXPECT_TRUE(g.enabled());
    EXPECT_TRUE(g.check_order(make_req(100.0, 999), ctx).has_value());

    g.set_enabled(false);
    EXPECT_FALSE(g.enabled());
    EXPECT_FALSE(g.check_order(make_req(100.0, 999), ctx).has_value());
}

// ============================================================================
// max_order_volume
// ============================================================================

TEST(RiskGateTest, MaxOrderVolumeExceeded) {
    RiskConfig cfg;
    cfg.max_order_volume = 100;
    RiskGate g(true, cfg);
    AccountContext ctx;
    auto r = g.check_order(make_req(100.0, 101), ctx);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->rule_name, "max_order_volume");
}

TEST(RiskGateTest, MaxOrderVolumeBoundary) {
    RiskConfig cfg;
    cfg.max_order_volume = 100;
    RiskGate g(true, cfg);
    AccountContext ctx;
    // 等于上限应该通过
    EXPECT_FALSE(g.check_order(make_req(100.0, 100), ctx).has_value());
}

TEST(RiskGateTest, MaxOrderVolumeZeroSkipsCheck) {
    RiskConfig cfg;
    cfg.max_order_volume = 0;  // 不检查
    RiskGate g(true, cfg);
    AccountContext ctx;
    EXPECT_FALSE(g.check_order(make_req(100.0, 999999), ctx).has_value());
}

// ============================================================================
// price_tick
// ============================================================================

TEST(RiskGateTest, PriceTickAligned) {
    RiskConfig cfg;
    cfg.check_price_tick = true;
    RiskGate g(true, cfg);
    AccountContext ctx;
    ctx.price_tick = 0.1;
    EXPECT_FALSE(g.check_order(make_req(100.1, 10), ctx).has_value());
    EXPECT_FALSE(g.check_order(make_req(100.0, 10), ctx).has_value());
}

TEST(RiskGateTest, PriceTickNotAligned) {
    RiskConfig cfg;
    cfg.check_price_tick = true;
    RiskGate g(true, cfg);
    AccountContext ctx;
    ctx.price_tick = 0.1;
    auto r = g.check_order(make_req(100.05, 10), ctx);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->rule_name, "price_tick");
}

TEST(RiskGateTest, PriceTickZeroSkipsCheck) {
    RiskConfig cfg;
    cfg.check_price_tick = true;
    RiskGate g(true, cfg);
    AccountContext ctx;
    ctx.price_tick = 0.0;  // 不检查
    EXPECT_FALSE(g.check_order(make_req(100.05, 10), ctx).has_value());
}

TEST(RiskGateTest, PriceTickCheckDisabled) {
    RiskConfig cfg;
    cfg.check_price_tick = false;  // 关闭检查
    RiskGate g(true, cfg);
    AccountContext ctx;
    ctx.price_tick = 0.1;
    EXPECT_FALSE(g.check_order(make_req(100.05, 10), ctx).has_value());
}

// ============================================================================
// check_cancel (当前总是通过)
// ============================================================================

TEST(RiskGateTest, CheckCancelAlwaysPass) {
    RiskGate g(true);
    AccountContext ctx;
    DzOrderReq r{};
    EXPECT_FALSE(g.check_cancel(r, ctx).has_value());
}
