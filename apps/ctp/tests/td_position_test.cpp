#include <gtest/gtest.h>

#include <vector>

#include "td/td_position.h"

using namespace dztrader::ctp;

namespace {

ActiveOrderUpdate make_close(const char* ref,
                             DzDirection dir,
                             DzPositionEffect effect,
                             int64_t volume,
                             int64_t traded = 0,
                             DzOrderStatus status = DZ_ORDER_NOT_TRADED,
                             const char* instrument = "IF2506") {
    ActiveOrderUpdate o{};
    o.instrument_id = instrument;
    o.order_ref = ref;
    o.direction = dir;
    o.effect = effect;
    o.status = status;
    o.volume = volume;
    o.volume_traded = traded;
    return o;
}

DzTradeReport make_trade(DzDirection dir, DzPositionEffect effect, DzVolume volume, double price) {
    DzTradeReport t{};
    t.direction = dir;
    t.position_effect = effect;
    t.volume = volume;
    t.price = price;
    return t;
}

}  // namespace

TEST(ParseExchangeIdTest, KnownAndUnknown) {
    EXPECT_EQ(parse_exchange_id("SHFE"), Exchange::SHFE);
    EXPECT_EQ(parse_exchange_id("INE"), Exchange::INE);
    EXPECT_EQ(parse_exchange_id("CFFEX"), Exchange::CFFEX);
    EXPECT_EQ(parse_exchange_id("UNKNOWN"), Exchange::Unknown);
    EXPECT_EQ(parse_exchange_id(""), Exchange::Unknown);
}

TEST(TerminalStatusTest, Classify) {
    EXPECT_TRUE(is_terminal_order_status(DZ_ORDER_ALL_TRADED));
    EXPECT_TRUE(is_terminal_order_status(DZ_ORDER_CANCELLED));
    EXPECT_TRUE(is_terminal_order_status(DZ_ORDER_REJECTED));
    EXPECT_FALSE(is_terminal_order_status(DZ_ORDER_NOT_TRADED));
    EXPECT_FALSE(is_terminal_order_status(DZ_ORDER_PART_TRADED));
}

TEST(PositionHoldingQueryTest, ApplyAndNormalize) {
    PositionHolding h{"IF2506", "CFFEX"};
    auto ch = h.apply_query_side(DZ_DIRECTION_LONG, 10, 4, 3900.0);
    EXPECT_TRUE(ch.long_changed);
    EXPECT_FALSE(ch.short_changed);
    EXPECT_EQ(h.long_today(), 6);
    EXPECT_EQ(h.long_yesterday(), 4);
    EXPECT_DOUBLE_EQ(h.side(DZ_DIRECTION_LONG).price, 3900.0);
    // 幂等: 相同快照无变化
    EXPECT_FALSE(h.apply_query_side(DZ_DIRECTION_LONG, 10, 4, 3900.0).any());
    // yd > volume 归一化
    h.apply_query_side(DZ_DIRECTION_LONG, 3, 9, 0.0);
    EXPECT_EQ(h.long_today(), 0);
    EXPECT_EQ(h.long_yesterday(), 3);
    EXPECT_DOUBLE_EQ(h.side(DZ_DIRECTION_LONG).price, 0.0);
}

TEST(PositionHoldingTradeTest, OpenWeightedPriceAndSides) {
    PositionHolding h{"IF2506", "CFFEX"};
    EXPECT_TRUE(h.apply_trade(make_trade(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 2, 3900.0))
                    .long_changed);
    h.apply_trade(make_trade(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 2, 3910.0));
    EXPECT_EQ(h.long_today(), 4);
    EXPECT_DOUBLE_EQ(h.side(DZ_DIRECTION_LONG).price, 3905.0);
    h.apply_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_OPEN, 3, 4000.0));
    EXPECT_EQ(h.short_today(), 3);
    EXPECT_EQ(h.short_yesterday(), 0);
}

TEST(PositionHoldingTradeTest, CloseTodayAndYesterday) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 3, 3900.0);  // today=2, yd=3
    h.apply_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 2, 3950.0));
    EXPECT_EQ(h.long_today(), 0);
    EXPECT_EQ(h.long_yesterday(), 3);
    h.apply_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_YESTDAY, 1, 3950.0));
    EXPECT_EQ(h.long_yesterday(), 2);
}

TEST(PositionHoldingTradeTest, GenericCloseSpillsToYesterday) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 3, 3900.0);  // today=2, yd=3
    h.apply_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 4, 3950.0));
    EXPECT_EQ(h.long_today(), 0);
    EXPECT_EQ(h.long_yesterday(), 1);
}

TEST(PositionHoldingTradeTest, ShfeGenericCloseHitsYesterday) {
    PositionHolding h{"rb2510", "SHFE"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 3, 3900.0);  // today=2, yd=3
    h.apply_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 1, 3950.0));
    EXPECT_EQ(h.long_today(), 2);
    EXPECT_EQ(h.long_yesterday(), 2);
}

TEST(PositionHoldingTradeTest, TradeClampsFrozenUntilOrderUpdate) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 0, 3900.0);
    h.apply_order(make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5));
    EXPECT_EQ(h.long_frozen(), 5);
    h.apply_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 2, 3950.0));
    EXPECT_EQ(h.long_today(), 3);
    // 夹取后的精确值: 冻结 = min(原冻结 5, 现持仓 3), 可用 = 0
    EXPECT_EQ(h.long_frozen(), 3);
    EXPECT_EQ(h.long_available_today(), 0);
}

TEST(PositionHoldingTradeTest, ShfeGenericCloseBeyondYdClamps) {
    PositionHolding h{"rb2510", "SHFE"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 3, 3900.0);  // today=2, yd=3
    h.apply_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5, 3950.0));
    EXPECT_EQ(h.long_today(), 2);  // SHFE generic 只平昨, 不溢出到今
    EXPECT_EQ(h.long_yesterday(), 0);
    EXPECT_EQ(h.long_today() + h.long_yesterday(), 2);
}

TEST(PositionHoldingTradeTest, NegativeClampedAndPriceReset) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 1, 0, 3900.0);
    h.apply_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 3, 3950.0));
    EXPECT_EQ(h.long_today(), 0);
    EXPECT_EQ(h.long_yesterday(), 0);
    EXPECT_DOUBLE_EQ(h.side(DZ_DIRECTION_LONG).price, 0.0);
}

TEST(PositionHoldingFrozenTest, GenericFreezesTodayThenSpills) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 3, 3900.0);  // today=2, yd=3
    auto ch =
        h.rebuild_active_orders({make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 4)});
    EXPECT_TRUE(ch.long_changed);
    EXPECT_EQ(h.long_frozen_today(), 2);
    EXPECT_EQ(h.long_frozen_yesterday(), 2);
    // 撤单释放
    ch = h.apply_order(
        make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 4, 0, DZ_ORDER_CANCELLED));
    EXPECT_TRUE(ch.long_changed);
    EXPECT_EQ(h.long_frozen(), 0);
}

TEST(PositionHoldingFrozenTest, TradeAdjustsRemainingFrozen) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 0, 3900.0);
    h.apply_order(make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 5));
    EXPECT_EQ(h.long_frozen_today(), 5);
    h.apply_order(make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 5, 2,
                             DZ_ORDER_PART_TRADED));
    EXPECT_EQ(h.long_frozen_today(), 3);
    EXPECT_TRUE(h.apply_order(make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 5,
                                         5, DZ_ORDER_ALL_TRADED))
                    .long_changed);
    EXPECT_EQ(h.long_frozen(), 0);
}

TEST(PositionHoldingFrozenTest, OpenOrderIgnoredAndClamped) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_SHORT, 1, 0, 3900.0);
    h.apply_order(make_close("1", DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 9));
    EXPECT_EQ(h.long_frozen(), 0);
    h.apply_order(make_close("2", DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_CLOSE_TODAY, 9));
    EXPECT_EQ(h.short_frozen_today(), 1);  // 夹取到持仓
}

TEST(PositionHoldingDaySwitchTest, TodayBecomesYesterdayAndFrozenCleared) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 1, 3900.0);
    h.apply_order(make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 4));
    auto ch = h.on_day_switch();
    EXPECT_TRUE(ch.long_changed);
    EXPECT_EQ(h.long_today(), 0);
    EXPECT_EQ(h.long_yesterday(), 5);
    EXPECT_EQ(h.long_frozen(), 0);
    EXPECT_DOUBLE_EQ(h.side(DZ_DIRECTION_LONG).price, 3900.0);
}

TEST(PositionHoldingFrozenTest, CrossInstrumentIsolation) {
    PositionHolding a{"IF2506", "CFFEX"};
    PositionHolding b{"rb2510", "SHFE"};
    a.apply_query_side(DZ_DIRECTION_LONG, 5, 0, 3900.0);
    b.apply_query_side(DZ_DIRECTION_LONG, 5, 0, 3900.0);
    auto other = make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 3, 0,
                            DZ_ORDER_NOT_TRADED, "rb2510");
    EXPECT_FALSE(a.rebuild_active_orders({other}).any());  // 他合约挂单不得污染
    EXPECT_EQ(a.long_frozen(), 0);
    EXPECT_TRUE(b.apply_order(other).long_changed);
    EXPECT_EQ(b.long_frozen(), 3);
}

TEST(PositionHoldingFrozenTest, RebuildIsIdempotent) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 0, 3900.0);
    std::vector<ActiveOrderUpdate> orders{
        make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 2)};
    EXPECT_TRUE(h.rebuild_active_orders(orders).long_changed);
    EXPECT_FALSE(h.rebuild_active_orders(orders).any());  // 周期查询重复种入不刷帧
}

TEST(PositionHoldingFrozenTest, RebuildRemovesStaleOrders) {
    // 整体重建必须移除快照中已消失的挂单, 否则残留挂单继续冻结持仓.
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 10, 0, 3900.0);
    std::vector<ActiveOrderUpdate> two{
        make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 2),
        make_close("2", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 3)};
    EXPECT_TRUE(h.rebuild_active_orders(two).long_changed);
    EXPECT_EQ(h.long_frozen(), 5);

    // 新快照只剩 "2": "1" 的 2 手冻结必须释放
    std::vector<ActiveOrderUpdate> one{
        make_close("2", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 3)};
    EXPECT_TRUE(h.rebuild_active_orders(one).long_changed);
    EXPECT_EQ(h.long_frozen(), 3);
    EXPECT_EQ(h.long_frozen_today(), 3);
    EXPECT_EQ(h.long_available_today(), 7);
}

TEST(PositionHoldingQueryTest, ShrinkClampsFrozen) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 10, 0, 3900.0);
    h.apply_order(make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5));
    EXPECT_EQ(h.long_frozen(), 5);
    h.apply_query_side(DZ_DIRECTION_LONG, 2, 0, 4000.0);
    // 缩量到 2: 冻结精确夹取为 2, 可用 = 0
    EXPECT_EQ(h.long_frozen(), 2);
    EXPECT_EQ(h.long_available_today(), 0);
}

TEST(PositionHoldingTradeTest, OpenPriceNotDilutedByZeroCost) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 2, 0, 0.0);  // 异常: cost=0
    h.apply_trade(make_trade(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 2, 3900.0));
    EXPECT_DOUBLE_EQ(h.side(DZ_DIRECTION_LONG).price, 3900.0);
}

TEST(PositionHoldingInputGuardTest, NetDirectionAndZeroVolumeIgnored) {
    PositionHolding h{"IF2506", "CFFEX"};
    EXPECT_FALSE(
        h.apply_trade(make_trade(DZ_DIRECTION_NET, DZ_POSITION_EFFECT_OPEN, 1, 3900.0)).any());
    EXPECT_FALSE(
        h.apply_trade(make_trade(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 0, 3900.0)).any());
    EXPECT_FALSE(
        h.apply_order(make_close("1", DZ_DIRECTION_NET, DZ_POSITION_EFFECT_CLOSE, 1)).any());
}

TEST(PositionHoldingInputGuardTest, UnknownPositionEffectDoesNotMovePositions) {
    // 未知开平标志既不能当开仓 (凭空放大敞口) 也不能落进 close_side (平掉对方真实持仓).
    // 先建好双边持仓: 多 5 空 3.
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 2, 3900.0);
    h.apply_query_side(DZ_DIRECTION_SHORT, 3, 3, 3910.0);

    const auto ch =
        h.apply_trade(make_trade(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_UNKNOWN, 2, 3900.0));
    EXPECT_FALSE(ch.any());
    EXPECT_EQ(h.side(DZ_DIRECTION_LONG).volume(), 5);
    EXPECT_EQ(h.side(DZ_DIRECTION_SHORT).volume(), 3)
        << "an unreadable offset flag must not close the opposite side";

    const auto ch2 =
        h.apply_trade(make_trade(DZ_DIRECTION_LONG, static_cast<DzPositionEffect>(99), 2, 3900.0));
    EXPECT_FALSE(ch2.any());
    EXPECT_EQ(h.side(DZ_DIRECTION_LONG).volume(), 5);
}

TEST(PositionHoldingInputGuardTest, UnknownEffectOrderNeverFreezesAnySide) {
    // 未知开平且未终态: 不得进活动委托表 —— 否则按「非开仓」给对侧记冻结,
    // 等于拿猜测的语义占用可用持仓 (issue #367 审查 F4).
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 2, 3900.0);
    h.apply_query_side(DZ_DIRECTION_SHORT, 3, 3, 3910.0);
    ASSERT_EQ(h.long_frozen(), 0);
    ASSERT_EQ(h.short_frozen(), 0);

    const auto ch = h.apply_order(make_close("u1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_UNKNOWN,
                                             4, 0, DZ_ORDER_NOT_TRADED));
    EXPECT_FALSE(ch.any());
    EXPECT_EQ(h.long_frozen(), 0) << "an unreadable offset flag must not freeze the long side";
    EXPECT_EQ(h.short_frozen(), 0);

    // 同一张单后续变成终态时仍要清掉 (状态可读, 不允许残留冻结).
    EXPECT_FALSE(h.apply_order(make_close("u1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_UNKNOWN, 4,
                                          0, DZ_ORDER_CANCELLED))
                     .any());
    EXPECT_EQ(h.long_frozen(), 0);
}
