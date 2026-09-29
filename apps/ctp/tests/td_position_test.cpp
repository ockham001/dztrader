#include <gtest/gtest.h>

#include <vector>

#include "td/td_position.h"

using namespace dztrader::ctp;

namespace {

ActiveOrderUpdate make_close(const char* ref, DzDirection dir, DzPositionEffect effect,
                             int64_t volume, int64_t traded = 0,
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
    EXPECT_TRUE(h.apply_trade(make_trade(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 2, 3900.0)).long_changed);
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
    EXPECT_LE(h.long_frozen(), 3);           // 成交先到也要夹取
    EXPECT_GE(h.long_available_today(), 0);
}

TEST(PositionHoldingTradeTest, ShfeGenericCloseBeyondYdClamps) {
    PositionHolding h{"rb2510", "SHFE"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 3, 3900.0);  // today=2, yd=3
    h.apply_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5, 3950.0));
    EXPECT_EQ(h.long_today(), 2);   // SHFE generic 只平昨, 不溢出到今
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
    auto ch = h.rebuild_active_orders({make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 4)});
    EXPECT_TRUE(ch.long_changed);
    EXPECT_EQ(h.long_frozen_today(), 2);
    EXPECT_EQ(h.long_frozen_yesterday(), 2);
    // 撤单释放
    ch = h.apply_order(make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 4, 0, DZ_ORDER_CANCELLED));
    EXPECT_TRUE(ch.long_changed);
    EXPECT_EQ(h.long_frozen(), 0);
}

TEST(PositionHoldingFrozenTest, TradeAdjustsRemainingFrozen) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 5, 0, 3900.0);
    h.apply_order(make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 5));
    EXPECT_EQ(h.long_frozen_today(), 5);
    h.apply_order(make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 5, 2, DZ_ORDER_PART_TRADED));
    EXPECT_EQ(h.long_frozen_today(), 3);
    EXPECT_TRUE(h.apply_order(make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 5, 5, DZ_ORDER_ALL_TRADED)).long_changed);
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

TEST(PositionHoldingQueryTest, ShrinkClampsFrozen) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 10, 0, 3900.0);
    h.apply_order(make_close("1", DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5));
    EXPECT_EQ(h.long_frozen(), 5);
    h.apply_query_side(DZ_DIRECTION_LONG, 2, 0, 4000.0);
    EXPECT_LE(h.long_frozen(), 2);
    EXPECT_GE(h.long_available_today(), 0);
}

TEST(PositionHoldingTradeTest, OpenPriceNotDilutedByZeroCost) {
    PositionHolding h{"IF2506", "CFFEX"};
    h.apply_query_side(DZ_DIRECTION_LONG, 2, 0, 0.0);  // 异常: cost=0
    h.apply_trade(make_trade(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 2, 3900.0));
    EXPECT_DOUBLE_EQ(h.side(DZ_DIRECTION_LONG).price, 3900.0);
}

TEST(PositionHoldingInputGuardTest, NetDirectionAndZeroVolumeIgnored) {
    PositionHolding h{"IF2506", "CFFEX"};
    EXPECT_FALSE(h.apply_trade(make_trade(DZ_DIRECTION_NET, DZ_POSITION_EFFECT_OPEN, 1, 3900.0)).any());
    EXPECT_FALSE(h.apply_trade(make_trade(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 0, 3900.0)).any());
    EXPECT_FALSE(h.apply_order(make_close("1", DZ_DIRECTION_NET, DZ_POSITION_EFFECT_CLOSE, 1)).any());
}
