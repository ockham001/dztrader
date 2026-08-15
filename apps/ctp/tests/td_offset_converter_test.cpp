#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "td/td_offset_converter.h"

using namespace dztrader::ctp;

namespace {

/// 辅助: 构造一个空的 DzPositionDetail (零初始化 + 填充关键字段)
DzPositionDetail make_detail(const char* trade_id, DzDirection dir,
                              int64_t volume, int32_t open_date) {
    DzPositionDetail d{};
    std::strcpy(d.instrument_id, "IF2506");
    std::strcpy(d.exchange_id, "CFFEX");
    std::strcpy(d.account_id, "acc1");
    d.direction = dir;
    d.hedge_flag = 'S';
    d.open_date = open_date;
    std::strcpy(d.trade_id, trade_id);
    d.volume = volume;
    d.open_price = 3900.0;
    return d;
}

/// 辅助: 构造一个 DzTradeReport
DzTradeReport make_trade(DzDirection dir, DzPositionEffect off,
                          int32_t volume, const char* trade_id = "T001") {
    DzTradeReport t{};
    std::strcpy(t.instrument_id, "IF2506");
    std::strcpy(t.exchange_id, "CFFEX");
    std::strcpy(t.account_id, "acc1");
    std::strcpy(t.trade_id, trade_id);
    t.direction = dir;
    t.position_effect = off;
    t.volume = volume;
    t.price = 3900.0;
    return t;
}

/// 辅助: 构造一个 DzOrderReq
DzOrderReq make_order_req(DzDirection dir, DzPositionEffect off,
                           int32_t volume, double price = 3900.0) {
    DzOrderReq r{};
    std::strcpy(r.instrument_id, "IF2506");
    std::strcpy(r.account_id, "acc1");
    r.direction = dir;
    r.price_type = DZ_PRICE_LIMIT;
    r.position_effect = off;
    r.volume = volume;
    r.price = price;
    r.order_id = 1;
    return r;
}

constexpr int32_t kToday = 20260726;  // 测试用交易日 (DzDate 格式: YYYYMMDD)
constexpr int32_t kYesterday = 20260725;

// ============================================================================
// Exchange 解析
// ============================================================================

TEST(ParseExchangeIdTest, KnownExchanges) {
    EXPECT_EQ(parse_exchange_id("SHFE"), Exchange::SHFE);
    EXPECT_EQ(parse_exchange_id("INE"), Exchange::INE);
    EXPECT_EQ(parse_exchange_id("CFFEX"), Exchange::CFFEX);
    EXPECT_EQ(parse_exchange_id("DCE"), Exchange::DCE);
    EXPECT_EQ(parse_exchange_id("CZCE"), Exchange::CZCE);
    EXPECT_EQ(parse_exchange_id("GFEX"), Exchange::GFEX);
}

TEST(ParseExchangeIdTest, UnknownExchange) {
    EXPECT_EQ(parse_exchange_id("UNKNOWN"), Exchange::Unknown);
    EXPECT_EQ(parse_exchange_id(""), Exchange::Unknown);
    EXPECT_EQ(parse_exchange_id("NYMEX"), Exchange::Unknown);
}

// ============================================================================
// PositionHolding 基础
// ============================================================================

class PositionHoldingTest : public ::testing::Test {
protected:
    PositionHolding h{"IF2506", Exchange::CFFEX, kToday};
};

TEST_F(PositionHoldingTest, InitialStateIsZero) {
    EXPECT_EQ(h.long_today(), 0);
    EXPECT_EQ(h.long_yesterday(), 0);
    EXPECT_EQ(h.short_today(), 0);
    EXPECT_EQ(h.short_yesterday(), 0);
    EXPECT_EQ(h.long_frozen(), 0);
    EXPECT_EQ(h.short_frozen(), 0);
    EXPECT_EQ(h.long_available_today(), 0);
    EXPECT_EQ(h.short_available_today(), 0);
    EXPECT_EQ(h.instrument_id(), "IF2506");
    EXPECT_EQ(h.exchange(), Exchange::CFFEX);
    EXPECT_EQ(h.trading_day(), kToday);
}

TEST_F(PositionHoldingTest, UpdatePositionDetailLongToday) {
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 5, kToday));
    EXPECT_EQ(h.long_today(), 5);
    EXPECT_EQ(h.long_yesterday(), 0);
    EXPECT_EQ(h.short_today(), 0);
    EXPECT_EQ(h.short_yesterday(), 0);
}

TEST_F(PositionHoldingTest, UpdatePositionDetailLongYesterday) {
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 5, kYesterday));
    EXPECT_EQ(h.long_today(), 0);
    EXPECT_EQ(h.long_yesterday(), 5);
    EXPECT_EQ(h.short_today(), 0);
    EXPECT_EQ(h.short_yesterday(), 0);
}

TEST_F(PositionHoldingTest, UpdatePositionDetailShortToday) {
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_SHORT, 3, kToday));
    EXPECT_EQ(h.long_today(), 0);
    EXPECT_EQ(h.long_yesterday(), 0);
    EXPECT_EQ(h.short_today(), 3);
    EXPECT_EQ(h.short_yesterday(), 0);
}

TEST_F(PositionHoldingTest, UpdatePositionDetailShortYesterday) {
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_SHORT, 3, kYesterday));
    EXPECT_EQ(h.short_today(), 0);
    EXPECT_EQ(h.short_yesterday(), 3);
}

TEST_F(PositionHoldingTest, MultipleDetailsAccumulate) {
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 5, kToday));
    h.update_position_detail(make_detail("T002", DZ_DIRECTION_LONG, 3, kToday));
    h.update_position_detail(make_detail("T003", DZ_DIRECTION_LONG, 2, kYesterday));
    h.update_position_detail(make_detail("T004", DZ_DIRECTION_SHORT, 4, kToday));
    EXPECT_EQ(h.long_today(), 8);       // 5 + 3
    EXPECT_EQ(h.long_yesterday(), 2);   // 2
    EXPECT_EQ(h.short_today(), 4);      // 4
    EXPECT_EQ(h.short_yesterday(), 0);
}

TEST_F(PositionHoldingTest, UpdateSameTradeIdReplaces) {
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 5, kToday));
    EXPECT_EQ(h.long_today(), 5);
    // 同 trade_id 更新 (volume 变化): 覆盖旧值
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 10, kToday));
    EXPECT_EQ(h.long_today(), 10);  // 不是 15, 是替换
}

TEST_F(PositionHoldingTest, ClearResetsAll) {
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 5, kToday));
    h.update_position_detail(make_detail("T002", DZ_DIRECTION_SHORT, 3, kYesterday));
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 2), true);

    h.clear();

    EXPECT_EQ(h.long_today(), 0);
    EXPECT_EQ(h.long_yesterday(), 0);
    EXPECT_EQ(h.short_today(), 0);
    EXPECT_EQ(h.short_yesterday(), 0);
    EXPECT_EQ(h.long_frozen(), 0);
    EXPECT_EQ(h.short_frozen(), 0);
}

// ============================================================================
// update_from_trade
// ============================================================================

TEST_F(PositionHoldingTest, TradeOpenAddsLongToday) {
    h.update_from_trade(make_trade(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 5));
    EXPECT_EQ(h.long_today(), 5);
    EXPECT_EQ(h.long_yesterday(), 0);
}

TEST_F(PositionHoldingTest, TradeOpenAddsShortToday) {
    h.update_from_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_OPEN, 3));
    EXPECT_EQ(h.short_today(), 3);
    EXPECT_EQ(h.short_yesterday(), 0);
}

TEST_F(PositionHoldingTest, TradeCloseDecrementsFrozen) {
    // 先冻结: 发出平多单 5 手
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5), true);
    EXPECT_EQ(h.long_frozen(), 5);

    // 成交 3 手 (部分成交): frozen -= 3
    h.update_from_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 3));
    EXPECT_EQ(h.long_frozen(), 2);  // 5 - 3
}

TEST_F(PositionHoldingTest, TradeCloseTodayDecrementsFrozen) {
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 5), true);
    EXPECT_EQ(h.long_frozen(), 5);

    h.update_from_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 5));
    EXPECT_EQ(h.long_frozen(), 0);
}

TEST_F(PositionHoldingTest, TradeCloseYesterdayDecrementsFrozen) {
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_YESTDAY, 5), true);
    EXPECT_EQ(h.long_frozen(), 5);

    h.update_from_trade(make_trade(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_YESTDAY, 5));
    EXPECT_EQ(h.long_frozen(), 0);
}

TEST_F(PositionHoldingTest, TradeOpenDoesNotAffectFrozen) {
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5), true);
    EXPECT_EQ(h.long_frozen(), 5);

    // 开仓不影响 frozen
    h.update_from_trade(make_trade(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 10));
    EXPECT_EQ(h.long_frozen(), 5);  // 不变
    EXPECT_EQ(h.long_today(), 10);  // 开仓增加今仓
}

// ============================================================================
// update_order_frozen
// ============================================================================

TEST_F(PositionHoldingTest, FrozenIncrementOnCloseSent) {
    // 平多 (卖出平仓): long_frozen += 5
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5), true);
    EXPECT_EQ(h.long_frozen(), 5);
    EXPECT_EQ(h.short_frozen(), 0);
}

TEST_F(PositionHoldingTest, FrozenIncrementOnShortCloseSent) {
    // 平空 (买入平仓): short_frozen += 3
    h.update_order_frozen(make_order_req(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_CLOSE, 3), true);
    EXPECT_EQ(h.short_frozen(), 3);
    EXPECT_EQ(h.long_frozen(), 0);
}

TEST_F(PositionHoldingTest, FrozenDecrementOnCancel) {
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5), true);
    EXPECT_EQ(h.long_frozen(), 5);

    // 撤单: frozen -= 5
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5), false);
    EXPECT_EQ(h.long_frozen(), 0);
}

TEST_F(PositionHoldingTest, FrozenIgnoredForOpen) {
    // 开仓不影响 frozen
    h.update_order_frozen(make_order_req(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 5), true);
    EXPECT_EQ(h.long_frozen(), 0);
    EXPECT_EQ(h.short_frozen(), 0);
}

TEST_F(PositionHoldingTest, AvailableTodayExcludesFrozen) {
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 10, kToday));
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 3), true);
    EXPECT_EQ(h.long_today(), 10);
    EXPECT_EQ(h.long_frozen(), 3);
    EXPECT_EQ(h.long_available_today(), 7);  // 10 - 3
}

// ============================================================================
// set_trading_day (日切)
// ============================================================================

TEST_F(PositionHoldingTest, SetTradingDayDoesNotRecompute) {
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 5, kToday));
    EXPECT_EQ(h.long_today(), 5);

    // 日切: trading_day 变更, 但不立即重算 (待下次 update_position_detail)
    h.set_trading_day(kToday + 1);
    EXPECT_EQ(h.trading_day(), kToday + 1);
    EXPECT_EQ(h.long_today(), 5);  // 仍为旧值, 未重算
}

// ============================================================================
// OffsetConverter: None 模式
// ============================================================================

TEST(OffsetConverterNoneTest, OpenOrderPassThrough) {
    PositionHolding h{"IF2506", Exchange::CFFEX, kToday};
    auto req = make_order_req(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 5);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::None);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_OPEN);
    EXPECT_EQ(result[0].volume, 5);
}

TEST(OffsetConverterNoneTest, CloseOrderPassThrough) {
    PositionHolding h{"IF2506", Exchange::CFFEX, kToday};
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 5, kToday));
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 3);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::None);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE);
    EXPECT_EQ(result[0].volume, 3);
}

TEST(OffsetConverterNoneTest, CloseTodayPassThrough) {
    PositionHolding h{"IF2506", Exchange::CFFEX, kToday};
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 5, kToday));
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 3);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::None);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_TODAY);
}

// ============================================================================
// OffsetConverter: Net 模式 (行为同 None)
// ============================================================================

TEST(OffsetConverterNetTest, BehavesLikeNone) {
    PositionHolding h{"IF2506", Exchange::CFFEX, kToday};
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 5, kToday));
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 3);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Net);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE);
    EXPECT_EQ(result[0].volume, 3);
}

// ============================================================================
// OffsetConverter: Lock 模式 (Close -> Open 反向)
// ============================================================================

TEST(OffsetConverterLockTest, CloseConvertsToOpen) {
    PositionHolding h{"IF2506", Exchange::CFFEX, kToday};
    // 用户发出平多 (SHORT CLOSE), Lock 模式转为 SHORT OPEN (反向开仓)
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Lock);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].direction, DZ_DIRECTION_SHORT);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_OPEN);  // 转为开仓
    EXPECT_EQ(result[0].volume, 5);
}

TEST(OffsetConverterLockTest, CloseTodayConvertsToOpen) {
    PositionHolding h{"IF2506", Exchange::CFFEX, kToday};
    auto req = make_order_req(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_CLOSE_TODAY, 3);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Lock);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_OPEN);
    EXPECT_EQ(result[0].volume, 3);
}

TEST(OffsetConverterLockTest, OpenPassThrough) {
    PositionHolding h{"IF2506", Exchange::CFFEX, kToday};
    // 开仓不受 Lock 模式影响
    auto req = make_order_req(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 5);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Lock);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_OPEN);
    EXPECT_EQ(result[0].volume, 5);
}

// ============================================================================
// OffsetConverter: Shfe 模式 (SHFE/INE 平今昨拆分)
// ============================================================================

class OffsetConverterShfeTest : public ::testing::Test {
protected:
    PositionHolding h{"rb2510", Exchange::SHFE, kToday};
};

TEST_F(OffsetConverterShfeTest, CloseLongAllFromToday) {
    // 持仓: 今多 10, 昨多 0
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 10, kToday));
    // 平多 5 手 (卖出平仓)
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_TODAY);
    EXPECT_EQ(result[0].volume, 5);
    EXPECT_EQ(result[0].direction, DZ_DIRECTION_SHORT);
}

TEST_F(OffsetConverterShfeTest, CloseLongAllFromYesterday) {
    // 持仓: 今多 0, 昨多 10
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 10, kYesterday));
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_YESTDAY);
    EXPECT_EQ(result[0].volume, 5);
}

TEST_F(OffsetConverterShfeTest, CloseLongSplitTodayAndYesterday) {
    // 持仓: 今多 3, 昨多 5
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 3, kToday));
    h.update_position_detail(make_detail("T002", DZ_DIRECTION_LONG, 5, kYesterday));
    // 平多 6 手: 今 3 + 昨 3
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 6);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_TODAY);
    EXPECT_EQ(result[0].volume, 3);
    EXPECT_EQ(result[1].position_effect, DZ_POSITION_EFFECT_CLOSE_YESTDAY);
    EXPECT_EQ(result[1].volume, 3);
}

TEST_F(OffsetConverterShfeTest, CloseShortSplitTodayAndYesterday) {
    // 持仓: 今空 4, 昨空 6
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_SHORT, 4, kToday));
    h.update_position_detail(make_detail("T002", DZ_DIRECTION_SHORT, 6, kYesterday));
    // 平空 8 手 (买入平仓): 今 4 + 昨 4
    auto req = make_order_req(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_CLOSE, 8);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_TODAY);
    EXPECT_EQ(result[0].volume, 4);
    EXPECT_EQ(result[0].direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(result[1].position_effect, DZ_POSITION_EFFECT_CLOSE_YESTDAY);
    EXPECT_EQ(result[1].volume, 4);
}

TEST_F(OffsetConverterShfeTest, CloseExcludesFrozenFromToday) {
    // 持仓: 今多 10, frozen 3 (已挂起平仓单)
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 10, kToday));
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 3), true);
    EXPECT_EQ(h.long_available_today(), 7);  // 10 - 3
    // 新平多 5 手: 今可用 7 >= 5, 全从今仓
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_TODAY);
    EXPECT_EQ(result[0].volume, 5);
}

TEST_F(OffsetConverterShfeTest, CloseSpillsToYesterdayWhenTodayInsufficient) {
    // 持仓: 今多 2 (frozen 1, 可用 1), 昨多 5
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 2, kToday));
    h.update_position_detail(make_detail("T002", DZ_DIRECTION_LONG, 5, kYesterday));
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 1), true);
    EXPECT_EQ(h.long_available_today(), 1);  // 2 - 1
    // 平多 4 手: 今 1 + 昨 3
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 4);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_TODAY);
    EXPECT_EQ(result[0].volume, 1);
    EXPECT_EQ(result[1].position_effect, DZ_POSITION_EFFECT_CLOSE_YESTDAY);
    EXPECT_EQ(result[1].volume, 3);
}

// I2: Shfe 模式昨仓可用量必须减冻结 (修复前 holding.long_yesterday() 未减 frozen)
// 场景: long_yd_=5, long_frozen_yd_=3 (已挂起平昨单 3 手),
//       CLOSE 6 手时昨仓可平仅 2 手 (5 - 3), 不应把冻结部分再次算作可平.
TEST_F(OffsetConverterShfeTest, CloseYesterdayExcludesFrozen) {
    // 持仓: 今多 0, 昨多 5
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 5, kYesterday));
    // 挂起平昨单 3 手: long_frozen_yd = 3
    h.update_order_frozen(make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_YESTDAY, 3),
                          true);
    EXPECT_EQ(h.long_yesterday(), 5);
    EXPECT_EQ(h.long_frozen_yesterday(), 3);
    EXPECT_EQ(h.long_available_yesterday(), 2);  // 5 - 3

    // 平多 6 手: 今仓 0, 昨仓可平 2 手 (修复前会取 long_yesterday=5, 错误地拆 5 手)
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 6);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    ASSERT_EQ(result.size(), 1u);  // 仅昨仓可平 2 手
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_YESTDAY);
    EXPECT_EQ(result[0].volume, 2);  // 修复后: 5 - 3 = 2
}

// I2: 反向场景验证 (平空, short_yd_ - short_frozen_yd_)
TEST_F(OffsetConverterShfeTest, CloseShortYesterdayExcludesFrozen) {
    // 持仓: 今空 0, 昨空 5
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_SHORT, 5, kYesterday));
    // 挂起平昨单 3 手: short_frozen_yd = 3
    h.update_order_frozen(make_order_req(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_CLOSE_YESTDAY, 3),
                          true);
    EXPECT_EQ(h.short_yesterday(), 5);
    EXPECT_EQ(h.short_frozen_yesterday(), 3);
    EXPECT_EQ(h.short_available_yesterday(), 2);

    // 平空 6 手: 昨仓可平仅 2 手
    auto req = make_order_req(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_CLOSE, 6);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_YESTDAY);
    EXPECT_EQ(result[0].volume, 2);
}

TEST_F(OffsetConverterShfeTest, CloseTodayRequestNotSplit) {
    // 用户显式 CLOSE_TODAY, 不拆分, 直接透传
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 10, kToday));
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 5);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_TODAY);
    EXPECT_EQ(result[0].volume, 5);
}

TEST_F(OffsetConverterShfeTest, CloseYesterdayRequestNotSplit) {
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 10, kYesterday));
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_YESTDAY, 5);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_YESTDAY);
    EXPECT_EQ(result[0].volume, 5);
}

TEST_F(OffsetConverterShfeTest, OpenNotSplit) {
    // 开仓不拆分
    auto req = make_order_req(DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 5);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_OPEN);
    EXPECT_EQ(result[0].volume, 5);
}

TEST_F(OffsetConverterShfeTest, InsufficientPositionReturnsPartial) {
    // 持仓: 今多 2, 昨多 0
    h.update_position_detail(make_detail("T001", DZ_DIRECTION_LONG, 2, kToday));
    // 平多 5 手, 但只有 2 手可用 -> 返回 2 手 (部分)
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    EXPECT_EQ(result.size(), 1u);  // 只能平 2 手
    EXPECT_EQ(result[0].volume, 2);
    EXPECT_EQ(result[0].position_effect, DZ_POSITION_EFFECT_CLOSE_TODAY);
}

TEST_F(OffsetConverterShfeTest, NoPositionReturnsEmpty) {
    // 无持仓, 平仓返回空 (调用方应拒绝)
    auto req = make_order_req(DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 5);
    auto result = OffsetConverter::convert_order_request(req, h, OffsetConvertMode::Shfe);
    EXPECT_TRUE(result.empty());
}

}  // namespace
