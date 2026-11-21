#include "td/td_ctp_mapping.h"

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <dztrader/date_time/date.h>

using namespace dztrader::ctp;

namespace {

/// 辅助: 取一个固定的 trading_day (DzDate 距纪元天数) 用于测试.
/// 对应 2026-07-27, 用 Date 类构造, 保证与生产转换路径一致.
constexpr int32_t kTradingDay = dztrader::Date{2026, 7, 27}.days_since_epoch();

/// 辅助: 构造一个最小可用的 CThostFtdcOrderField
CThostFtdcOrderField make_order_field() {
    CThostFtdcOrderField f{};
    std::strcpy(f.BrokerID, "9999");
    std::strcpy(f.InvestorID, "acc1");
    std::strcpy(f.InstrumentID, "IF2506");
    std::strcpy(f.OrderRef, "123");
    std::strcpy(f.OrderSysID, "EXT001");
    std::strcpy(f.ExchangeID, "CFFEX");
    f.Direction = THOST_FTDC_D_Buy;            // 买
    f.CombOffsetFlag[0] = THOST_FTDC_OF_Open;  // 开仓
    f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    f.LimitPrice = 3900.0;
    f.VolumeTotalOriginal = 5;
    f.VolumeTraded = 2;
    f.OrderStatus = THOST_FTDC_OST_PartTradedQueueing;  // 部分成交
    std::strcpy(f.InsertTime, "09:30:45");
    std::strcpy(f.UpdateTime, "09:31:00");
    std::strcpy(f.StatusMsg, "PartTraded");
    return f;
}

/// 辅助: 构造一个最小可用的 CThostFtdcTradeField
CThostFtdcTradeField make_trade_field() {
    CThostFtdcTradeField f{};
    std::strcpy(f.BrokerID, "9999");
    std::strcpy(f.InvestorID, "acc1");
    std::strcpy(f.InstrumentID, "IF2506");
    std::strcpy(f.OrderRef, "123");
    std::strcpy(f.TradeID, "T001");
    std::strcpy(f.ExchangeID, "CFFEX");
    f.Direction = THOST_FTDC_D_Buy;
    f.OffsetFlag = THOST_FTDC_OF_Open;  // 开仓
    f.Price = 3900.0;
    f.Volume = 2;
    std::strcpy(f.TradeTime, "09:30:45");
    return f;
}

/// 辅助: 构造一个最小可用的 CThostFtdcInstrumentField
CThostFtdcInstrumentField make_instrument_field() {
    CThostFtdcInstrumentField f{};
    std::strcpy(f.InstrumentID, "IF2506");
    std::strcpy(f.ExchangeID, "CFFEX");
    std::strcpy(f.InstrumentName, "IF2506");
    std::strcpy(f.ProductID, "IF");
    f.ProductClass = THOST_FTDC_PC_Futures;
    f.VolumeMultiple = 300;
    f.PriceTick = 0.2;
    f.MinLimitOrderVolume = 1;
    f.MaxLimitOrderVolume = 500;
    f.MinMarketOrderVolume = 1;
    f.MaxMarketOrderVolume = 500;
    f.OptionsType = 0;  // 非期权
    f.StrikePrice = 0.0;
    std::strcpy(f.OpenDate, "20260119");
    std::strcpy(f.ExpireDate, "20260619");
    return f;
}

/// 辅助: 构造一个最小可用的 CThostFtdcTradingAccountField
CThostFtdcTradingAccountField make_account_field() {
    CThostFtdcTradingAccountField f{};
    std::strcpy(f.AccountID, "acc1");
    f.Balance = 100000.0;
    f.Available = 80000.0;
    f.FrozenMargin = 5000.0;
    f.Commission = 100.0;
    f.CurrMargin = 15000.0;
    f.WithdrawQuota = 70000.0;
    f.Deposit = 0.0;
    f.Withdraw = 0.0;
    std::strcpy(f.TradingDay, "20260727");
    return f;
}

/// 辅助: 构造一个最小可用的 CThostFtdcInvestorPositionField
CThostFtdcInvestorPositionField make_position_field() {
    CThostFtdcInvestorPositionField f{};
    std::strcpy(f.InstrumentID, "IF2506");
    std::strcpy(f.ExchangeID, "CFFEX");
    f.PosiDirection = THOST_FTDC_PD_Long;
    f.Position = 10;      // 该行当前总持仓 (今 + 昨)
    f.TodayPosition = 6;  // 当前今仓
    f.YdPosition = 6;     // 交易日开始时静态昨仓: 今日已平昨 2 手, 静态值不随之减少
    f.LongFrozen = 2;
    f.ShortFrozen = 1;
    f.PositionCost = 39000.0;  // 10 手 @ 3900
    std::strcpy(f.TradingDay, "20260727");
    return f;
}

/// 辅助: 构造一个 DzOrderReq
DzOrderReq make_order_req(
    DzPriceType pt, DzDirection dir, DzPositionEffect off, int32_t volume, double price = 3900.0) {
    DzOrderReq r{};
    r.order_id = 42;
    std::strcpy(r.strategy_id, "strat1");
    std::strcpy(r.account_id, "acc1");
    std::strcpy(r.instrument_id, "IF2506");
    std::strcpy(r.remark, "test");
    r.price = price;
    r.volume = volume;
    r.direction = dir;
    r.price_type = pt;
    r.position_effect = off;
    return r;
}

}  // namespace

// ============================================================================
// parse_ctp_time
// ============================================================================

TEST(ParseCtpTimeTest, ValidTime) {
    EXPECT_EQ(parse_ctp_time("09:30:00"), 9 * 3600 + 30 * 60);
    EXPECT_EQ(parse_ctp_time("00:00:00"), 0);
    EXPECT_EQ(parse_ctp_time("23:59:59"), 23 * 3600 + 59 * 60 + 59);
    EXPECT_EQ(parse_ctp_time("15:00:00"), 15 * 3600);
}

TEST(ParseCtpTimeTest, NullReturns) {
    EXPECT_EQ(parse_ctp_time(nullptr), -1);
    EXPECT_EQ(parse_ctp_time(""), -1);
}

TEST(ParseCtpTimeTest, InvalidFormat) {
    EXPECT_EQ(parse_ctp_time("abc"), -1);
    EXPECT_EQ(parse_ctp_time("9:30:00"), -1);      // 长度不足
    EXPECT_EQ(parse_ctp_time("09:30"), -1);        // 长度不足
    EXPECT_EQ(parse_ctp_time("09:30:00:00"), -1);  // 长度过长
    EXPECT_EQ(parse_ctp_time("09:30:0a"), -1);     // 非数字
}

TEST(ParseCtpTimeTest, OutOfRange) {
    EXPECT_EQ(parse_ctp_time("24:00:00"), -1);  // 小时越界
    EXPECT_EQ(parse_ctp_time("09:60:00"), -1);  // 分钟越界
    EXPECT_EQ(parse_ctp_time("09:30:60"), -1);  // 秒越界
}

// ============================================================================
// parse_ctp_date
// ============================================================================

TEST(ParseCtpDateTest, ValidDate) {
    EXPECT_EQ(parse_ctp_date("20260727"), (dztrader::Date{2026, 7, 27}.days_since_epoch()));
    EXPECT_EQ(parse_ctp_date("19700101"), 0);  // 纪元日
    EXPECT_EQ(parse_ctp_date("20200101"), (dztrader::Date{2020, 1, 1}.days_since_epoch()));
}

TEST(ParseCtpDateTest, NullReturns) {
    EXPECT_EQ(parse_ctp_date(nullptr), DZ_DATE_NA);
    EXPECT_EQ(parse_ctp_date(""), DZ_DATE_NA);
}

TEST(ParseCtpDateTest, InvalidFormat) {
    EXPECT_EQ(parse_ctp_date("abc"), DZ_DATE_NA);
    EXPECT_EQ(parse_ctp_date("2026072"), DZ_DATE_NA);    // 长度不足
    EXPECT_EQ(parse_ctp_date("202607277"), DZ_DATE_NA);  // 长度过长
    EXPECT_EQ(parse_ctp_date("2026072a"), DZ_DATE_NA);   // 非数字
}

TEST(ParseCtpDateTest, InvalidDate) {
    EXPECT_EQ(parse_ctp_date("20260230"), DZ_DATE_NA);  // 2 月 30 日不存在
    EXPECT_EQ(parse_ctp_date("20261301"), DZ_DATE_NA);  // 月份越界
    EXPECT_EQ(parse_ctp_date("20260001"), DZ_DATE_NA);  // 月份为 0
}

TEST(ParseCtpDateTest, EpochDayCollidesWithNa) {
    // "19700101" 解析结果 0 与 DZ_DATE_NA 重合 — 设计决策: 视同 NA
    // (业务上不存在 1970 年真实交易日期, 见 DZ_DATE_NA 注释)
    EXPECT_EQ(parse_ctp_date("19700101"), DZ_DATE_NA);
}

// ============================================================================
// STATUS_CTP2VT (设计 §12.3 状态映射)
// ============================================================================

TEST(StatusCtp2VtTest, Map) {
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_AllTraded), DZ_ORDER_ALL_TRADED);               // '0'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_PartTradedQueueing), DZ_ORDER_PART_TRADED);     // '1'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_PartTradedNotQueueing), DZ_ORDER_PART_TRADED);  // '2'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_NoTradeQueueing), DZ_ORDER_NOT_TRADED);         // '3'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_NoTradeNotQueueing), DZ_ORDER_CANCELLED);       // '4'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_Canceled), DZ_ORDER_CANCELLED);                 // '5'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_Unknown), DZ_ORDER_SUBMITTING);                 // 'a'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_NotTouched), DZ_ORDER_NOT_TRADED);              // 'b'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_Touched), DZ_ORDER_PART_TRADED);                // 'c'
}

TEST(StatusCtp2VtTest, UnknownDefault) {
    // 未知状态必须暴露为 UNKNOWN, 不得猜成 SUBMITTING:
    // 猜成「提交中」会让委托永久停在非终态, 冻结持仓不释放 (issue #367).
    UnknownEnumSink unknown;
    EXPECT_EQ(STATUS_CTP2VT('z', &unknown), DZ_ORDER_UNKNOWN);
    EXPECT_EQ(STATUS_CTP2VT('\0'), DZ_ORDER_UNKNOWN);
    EXPECT_EQ(STATUS_CTP2VT('X'), DZ_ORDER_UNKNOWN);
    // 原始码被留下作证据 (不丢「对方到底发了什么」).
    ASSERT_EQ(unknown.count, 1u);
    EXPECT_STREQ(unknown.items[0].category, kUnknownCategoryOrderStatus);
    EXPECT_EQ(unknown.items[0].raw_char, 'z');
}

TEST(StatusCtp2VtTest, KnownCodesHaveNoUnknownEvidence) {
    // 已知码路径不得登记任何未知取证 (否则计数器会被正常回报污染).
    UnknownEnumSink unknown;
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_AllTraded, &unknown), DZ_ORDER_ALL_TRADED);
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_Canceled, &unknown), DZ_ORDER_CANCELLED);
    EXPECT_TRUE(unknown.empty());
}

TEST(StatusCtp2VtTest, CounterUnknownKeepsSubmittingSemantics) {
    // 柜台自报 'a'(Unknown) 不是「我们读不懂」, 语义仍是等待下次回报 -> SUBMITTING,
    // 且不计入未知枚举计数 (两类原因不同, 处置也不同).
    UnknownEnumSink unknown;
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_Unknown, &unknown), DZ_ORDER_SUBMITTING);
    EXPECT_TRUE(unknown.empty());
}

// ============================================================================
// ORDERTYPE_VT2CTP
// ============================================================================

TEST(OrderTypeVt2CtpTest, Limit) {
    auto t = ORDERTYPE_VT2CTP(DZ_PRICE_LIMIT);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->limit_price_type, THOST_FTDC_OPT_LimitPrice);
    EXPECT_EQ(t->time_condition, THOST_FTDC_TC_GFD);
    EXPECT_EQ(t->volume_condition, THOST_FTDC_VC_MV);
}

TEST(OrderTypeVt2CtpTest, Market) {
    auto t = ORDERTYPE_VT2CTP(DZ_PRICE_MARKET);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->limit_price_type, THOST_FTDC_OPT_AnyPrice);
    EXPECT_EQ(t->time_condition, THOST_FTDC_TC_IOC);
    EXPECT_EQ(t->volume_condition, THOST_FTDC_VC_AV);
}

TEST(OrderTypeVt2CtpTest, Fak) {
    auto t = ORDERTYPE_VT2CTP(DZ_PRICE_FAK);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->limit_price_type, THOST_FTDC_OPT_LimitPrice);
    EXPECT_EQ(t->time_condition, THOST_FTDC_TC_IOC);
    EXPECT_EQ(t->volume_condition, THOST_FTDC_VC_MV);
}

TEST(OrderTypeVt2CtpTest, Fok) {
    // CTP 无 FOK 时间条件, 用 IOC + CV 模拟 (MinVolume 由 to_input_order_field 设为 volume)
    auto t = ORDERTYPE_VT2CTP(DZ_PRICE_FOK);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->limit_price_type, THOST_FTDC_OPT_LimitPrice);
    EXPECT_EQ(t->time_condition, THOST_FTDC_TC_IOC);
    EXPECT_EQ(t->volume_condition, THOST_FTDC_VC_CV);
}

TEST(OrderTypeVt2CtpTest, UnsupportedReturnsNullopt) {
    // STOP/RFQ 期货不支持 -> 失败. 反转旧断言「兜底返回 LimitPrice + GFD」:
    // 兜底会把止损/询价单当普通限价单发到交易所 (issue #367 最严重的一类错误).
    EXPECT_FALSE(ORDERTYPE_VT2CTP(DZ_PRICE_STOP).has_value());
    EXPECT_FALSE(ORDERTYPE_VT2CTP(DZ_PRICE_RFQ).has_value());
    // 未知取值同样失败, 不退化成限价.
    EXPECT_FALSE(ORDERTYPE_VT2CTP(DZ_PRICE_UNKNOWN).has_value());
    EXPECT_FALSE(ORDERTYPE_VT2CTP(static_cast<DzPriceType>(99)).has_value());
}

// ============================================================================
// to_input_order_field
// ============================================================================

TEST(ToInputOrderFieldTest, LimitBuyOpen) {
    auto req = make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 5);
    OrderBuildContext ctx{"acc1", 123, 1, "CFFEX"};
    auto mapped = to_input_order_field(req, ctx);
    ASSERT_TRUE(mapped.has_value());
    const auto& f = *mapped;

    EXPECT_STREQ(f.InvestorID, "acc1");
    EXPECT_STREQ(f.InstrumentID, "IF2506");
    EXPECT_STREQ(f.OrderRef, "000000000123");  // 12 位补零
    EXPECT_EQ(f.Direction, THOST_FTDC_D_Buy);
    EXPECT_EQ(f.CombOffsetFlag[0], THOST_FTDC_OF_Open);
    EXPECT_EQ(f.OrderPriceType, THOST_FTDC_OPT_LimitPrice);
    EXPECT_EQ(f.TimeCondition, THOST_FTDC_TC_GFD);
    EXPECT_EQ(f.VolumeCondition, THOST_FTDC_VC_MV);
    EXPECT_EQ(f.MinVolume, 1);
    EXPECT_DOUBLE_EQ(f.LimitPrice, 3900.0);
    EXPECT_EQ(f.VolumeTotalOriginal, 5);
    EXPECT_EQ(f.RequestID, 1);
    EXPECT_STREQ(f.ExchangeID, "CFFEX");
}

TEST(ToInputOrderFieldTest, SellCloseToday) {
    auto req =
        make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE_TODAY, 3);
    OrderBuildContext ctx{"acc1", 999, 2, "SHFE"};
    auto mapped = to_input_order_field(req, ctx);
    ASSERT_TRUE(mapped.has_value());
    const auto& f = *mapped;

    EXPECT_EQ(f.Direction, THOST_FTDC_D_Sell);
    EXPECT_EQ(f.CombOffsetFlag[0], THOST_FTDC_OF_CloseToday);
    EXPECT_EQ(f.MinVolume, 1);
}

TEST(ToInputOrderFieldTest, BuyCloseYesterday) {
    auto req =
        make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_CLOSE_YESTDAY, 2);
    OrderBuildContext ctx{"acc1", 1, 3, "SHFE"};
    auto mapped = to_input_order_field(req, ctx);
    ASSERT_TRUE(mapped.has_value());

    EXPECT_EQ(mapped->Direction, THOST_FTDC_D_Buy);
    EXPECT_EQ(mapped->CombOffsetFlag[0], THOST_FTDC_OF_CloseYesterday);
}

TEST(ToInputOrderFieldTest, Close) {
    auto req = make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_SHORT, DZ_POSITION_EFFECT_CLOSE, 1);
    OrderBuildContext ctx{"acc1", 1, 4, "CFFEX"};
    auto mapped = to_input_order_field(req, ctx);
    ASSERT_TRUE(mapped.has_value());

    EXPECT_EQ(mapped->CombOffsetFlag[0], THOST_FTDC_OF_Close);
}

TEST(ToInputOrderFieldTest, FokMinVolume) {
    // FOK 模拟: MinVolume 应等于 volume
    auto req = make_order_req(DZ_PRICE_FOK, DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 7);
    OrderBuildContext ctx{"acc1", 1, 5, "CFFEX"};
    auto mapped = to_input_order_field(req, ctx);
    ASSERT_TRUE(mapped.has_value());
    const auto& f = *mapped;

    EXPECT_EQ(f.TimeCondition, THOST_FTDC_TC_IOC);
    EXPECT_EQ(f.VolumeCondition, THOST_FTDC_VC_CV);
    EXPECT_EQ(f.MinVolume, 7);  // FOK 关键: MinVolume = VolumeTotalOriginal
    EXPECT_EQ(f.OrderPriceType, THOST_FTDC_OPT_LimitPrice);
}

TEST(ToInputOrderFieldTest, FakMinVolume) {
    // FAK: MinVolume 保持 1 (允许部分成交)
    auto req = make_order_req(DZ_PRICE_FAK, DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 7);
    OrderBuildContext ctx{"acc1", 1, 6, "CFFEX"};
    auto mapped = to_input_order_field(req, ctx);
    ASSERT_TRUE(mapped.has_value());

    EXPECT_EQ(mapped->TimeCondition, THOST_FTDC_TC_IOC);
    EXPECT_EQ(mapped->MinVolume, 1);
}

TEST(ToInputOrderFieldTest, OrderRefFormat) {
    // OrderRef 必须 12 位补零, 边界值测试
    auto req = make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 1);

    OrderBuildContext ctx1{"acc1", 0, 0, "CFFEX"};
    auto f1 = to_input_order_field(req, ctx1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_STREQ(f1->OrderRef, "000000000000");

    OrderBuildContext ctx2{"acc1", 999999999999LL, 0, "CFFEX"};
    auto f2 = to_input_order_field(req, ctx2);
    ASSERT_TRUE(f2.has_value());
    EXPECT_STREQ(f2->OrderRef, "999999999999");
}

TEST(ToInputOrderFieldTest, MarketPriceIgnored) {
    // 市价单 LimitPrice 应被忽略, 但仍拷贝 req.price
    auto req = make_order_req(DZ_PRICE_MARKET, DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 1, 0.0);
    OrderBuildContext ctx{"acc1", 1, 7, "CFFEX"};
    auto mapped = to_input_order_field(req, ctx);
    ASSERT_TRUE(mapped.has_value());

    EXPECT_EQ(mapped->OrderPriceType, THOST_FTDC_OPT_AnyPrice);
    EXPECT_EQ(mapped->TimeCondition, THOST_FTDC_TC_IOC);
}

TEST(ToInputOrderFieldTest, UnsupportedPriceTypeFails) {
    // 出账方向绝不兜底: 止损/询价单不能变成普通限价单被发到交易所.
    OrderBuildContext ctx{"acc1", 1, 8, "CFFEX"};
    for (DzPriceType pt : {DZ_PRICE_STOP, DZ_PRICE_RFQ, DZ_PRICE_UNKNOWN}) {
        auto req = make_order_req(pt, DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 1);
        EXPECT_FALSE(to_input_order_field(req, ctx).has_value())
            << "price_type=" << static_cast<int>(pt) << " must not be mapped silently";
    }
    auto req =
        make_order_req(static_cast<DzPriceType>(99), DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 1);
    EXPECT_FALSE(to_input_order_field(req, ctx).has_value());
}

TEST(ToInputOrderFieldTest, UnknownPositionEffectFails) {
    // 未知开平不得兜成 OPEN: 平仓单变开仓单会凭空放大敞口.
    OrderBuildContext ctx{"acc1", 1, 9, "CFFEX"};
    for (DzPositionEffect pe : {DZ_POSITION_EFFECT_UNKNOWN, DZ_POSITION_EFFECT_AUTO}) {
        auto req = make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_LONG, pe, 1);
        EXPECT_FALSE(to_input_order_field(req, ctx).has_value())
            << "position_effect=" << static_cast<int>(pe) << " must not be mapped silently";
    }
    auto req =
        make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_LONG, static_cast<DzPositionEffect>(99), 1);
    EXPECT_FALSE(to_input_order_field(req, ctx).has_value());
}

TEST(ToInputOrderFieldTest, UnknownDirectionFails) {
    // 报单方向只有买/卖两种: 未知方向兜成 Sell 会把一张「方向读不懂」的单发给交易所.
    OrderBuildContext ctx{"acc1", 1, 10, "CFFEX"};
    for (DzDirection dir : {DZ_DIRECTION_UNKNOWN, DZ_DIRECTION_NET}) {
        auto req = make_order_req(DZ_PRICE_LIMIT, dir, DZ_POSITION_EFFECT_OPEN, 1);
        EXPECT_FALSE(to_input_order_field(req, ctx).has_value())
            << "direction=" << static_cast<int>(dir) << " must not be mapped silently";
    }
    auto req =
        make_order_req(DZ_PRICE_LIMIT, static_cast<DzDirection>(99), DZ_POSITION_EFFECT_OPEN, 1);
    EXPECT_FALSE(to_input_order_field(req, ctx).has_value());
}

// ============================================================================
// to_order_record
// ============================================================================

TEST(ToOrderRecordTest, BasicFields) {
    auto of = make_order_field();
    auto r = to_order_record(of, "acc1", kTradingDay);

    // base 字段
    EXPECT_EQ(r.base.order_id, 0);  // 留 0, AccountSession 后填
    EXPECT_STREQ(r.base.instrument_id, "IF2506");
    EXPECT_EQ(r.base.direction, DZ_DIRECTION_LONG);  // CTP '0' Buy -> LONG
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_OPEN);
    EXPECT_EQ(r.base.price_type, DZ_PRICE_LIMIT);  // 由 OrderPriceType 反推
    EXPECT_EQ(r.base.status, DZ_ORDER_PART_TRADED);
    EXPECT_DOUBLE_EQ(r.base.price, 3900.0);
    EXPECT_EQ(r.base.volume, 5);
    EXPECT_EQ(r.base.volume_traded, 2);
    EXPECT_EQ(r.base.date, kTradingDay);
    EXPECT_EQ(r.base.time, 9 * 3600 + 30 * 60 + 45);  // 09:30:45
    EXPECT_STREQ(r.base.account_id, "acc1");
    EXPECT_STREQ(r.base.exchange_id, "CFFEX");

    // 扩展字段
    EXPECT_STREQ(r.order_ref, "123");
    EXPECT_STREQ(r.external_order_id, "EXT001");
    EXPECT_EQ(r.is_external, 0);      // 留 0, AccountSession 后填
    EXPECT_EQ(r.volume_canceled, 0);  // 留 0, AccountSession 后推导
    EXPECT_EQ(r.error_id, 0);

    // 时间/交易日字段 (TD 数据同步去重键依赖)
    EXPECT_STREQ(r.trading_day, "20260727");
    // insert_time / update_time 的日期部分都取会话交易日; 09:30:45 = 34245 秒
    EXPECT_EQ(r.insert_time, static_cast<int64_t>(kTradingDay) * 86400 + 9 * 3600 + 30 * 60 + 45);
    // 09:31:00 = 34260 秒
    EXPECT_EQ(r.update_time, static_cast<int64_t>(kTradingDay) * 86400 + 9 * 3600 + 31 * 60);
}

TEST(ToOrderRecordTest, InsertDateIgnoredInFavorOfTradingDay) {
    // 夜盘场景: CTP InsertDate 是自然日, 与归属交易日不同 —— 会话交易日才是唯一时间锚,
    // insert_time 必须与 base.date / trading_day[9] / update_time 同源, 不采信 InsertDate.
    auto of = make_order_field();
    std::strcpy(of.InsertDate, "20260728");

    auto r = to_order_record(of, "acc1", kTradingDay);
    // insert_time 09:30:45 / update_time 09:31:00, 日期部分同为会话交易日 2026-07-27.
    // 若采信 InsertDate (2026-07-28), insert_time 会跳到次日 09:30:45, 于是
    // update_time - insert_time 变为 15 - 86400 = -86385 而非 15.
    EXPECT_EQ(r.insert_time, static_cast<int64_t>(kTradingDay) * 86400 + 9 * 3600 + 30 * 60 + 45);
    EXPECT_STREQ(r.trading_day, "20260727");
    EXPECT_EQ(r.update_time, static_cast<int64_t>(kTradingDay) * 86400 + 9 * 3600 + 31 * 60);
    EXPECT_EQ(r.update_time - r.insert_time, 15) << "insert_time 与 update_time 必须同属一个交易日";
}

TEST(ToOrderRecordTest, EmptyTimesFallBackToZero) {
    auto of = make_order_field();
    std::strcpy(of.InsertTime, "");
    std::strcpy(of.UpdateTime, "");

    auto r = to_order_record(of, "acc1", kTradingDay);
    EXPECT_EQ(r.insert_time, 0);
    EXPECT_EQ(r.update_time, 0);
}

TEST(ToOrderRecordTest, SellCloseTodayMapping) {
    auto of = make_order_field();
    of.Direction = THOST_FTDC_D_Sell;
    of.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;
    of.OrderPriceType = THOST_FTDC_OPT_AnyPrice;

    auto r = to_order_record(of, "acc1", kTradingDay);
    EXPECT_EQ(r.base.direction, DZ_DIRECTION_SHORT);
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_CLOSE_TODAY);
    EXPECT_EQ(r.base.price_type, DZ_PRICE_MARKET);
}

TEST(ToOrderRecordTest, SellCloseYesterdayMapping) {
    auto of = make_order_field();
    of.Direction = THOST_FTDC_D_Sell;
    of.CombOffsetFlag[0] = THOST_FTDC_OF_CloseYesterday;

    auto r = to_order_record(of, "acc1", kTradingDay);
    EXPECT_EQ(r.base.direction, DZ_DIRECTION_SHORT);
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_CLOSE_YESTDAY);
}

TEST(ToOrderRecordTest, CloseMapping) {
    // CTP 通用平仓 (THOST_FTDC_OF_Close) -> DZ CLOSE (非 CLOSE_TODAY/CLOSE_YESTDAY)
    auto of = make_order_field();
    of.Direction = THOST_FTDC_D_Sell;
    of.CombOffsetFlag[0] = THOST_FTDC_OF_Close;

    auto r = to_order_record(of, "acc1", kTradingDay);
    EXPECT_EQ(r.base.direction, DZ_DIRECTION_SHORT);
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_CLOSE);
}

TEST(ToOrderRecordTest, StatusAllTraded) {
    auto of = make_order_field();
    of.OrderStatus = THOST_FTDC_OST_AllTraded;
    of.VolumeTraded = 5;

    auto r = to_order_record(of, "acc1", kTradingDay);
    EXPECT_EQ(r.base.status, DZ_ORDER_ALL_TRADED);
}

TEST(ToOrderRecordTest, StatusCancelled) {
    auto of = make_order_field();
    of.OrderStatus = THOST_FTDC_OST_Canceled;
    of.VolumeTraded = 1;
    of.VolumeTotalOriginal = 5;

    auto r = to_order_record(of, "acc1", kTradingDay);
    EXPECT_EQ(r.base.status, DZ_ORDER_CANCELLED);
    // volume_canceled 留 0, 由 AccountSession 推导 (= volume - volume_traded = 4)
    EXPECT_EQ(r.volume_canceled, 0);
}

TEST(ToOrderRecordTest, NullTime) {
    auto of = make_order_field();
    std::strcpy(of.InsertTime, "");

    auto r = to_order_record(of, "acc1", kTradingDay);
    EXPECT_EQ(r.base.time, -1);  // 解析失败
}

TEST(ToOrderRecordTest, PriceTypeFak) {
    auto of = make_order_field();
    // CTP 没有专门的 FAK 标志, 由 LimitPrice + IOC + MV 推断
    of.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    of.TimeCondition = THOST_FTDC_TC_IOC;
    of.VolumeCondition = THOST_FTDC_VC_MV;

    auto r = to_order_record(of, "acc1", kTradingDay);
    // 当前实现: OrderPriceType 优先, 反推为 LIMIT
    // (FAK/FOK 在 OrderRecord 中无独立字段, AccountSession 不依赖此区分)
    EXPECT_EQ(r.base.price_type, DZ_PRICE_LIMIT);
}

TEST(ToOrderRecordTest, UnknownStatusBecomesUnknown) {
    // 反转旧行为: 未知状态不得兜成 SUBMITTING (委托永久停非终态, 冻结不释放).
    auto of = make_order_field();
    of.OrderStatus = 'Z';

    UnknownEnumSink unknown;
    auto r = to_order_record(of, "acc1", kTradingDay, &unknown);
    EXPECT_EQ(r.base.status, DZ_ORDER_UNKNOWN);
    ASSERT_EQ(unknown.count, 1u);
    EXPECT_STREQ(unknown.items[0].category, kUnknownCategoryOrderStatus);
    EXPECT_EQ(unknown.items[0].raw_char, 'Z');
}

TEST(ToOrderRecordTest, UnknownOffsetBecomesUnknown) {
    auto of = make_order_field();
    of.CombOffsetFlag[0] = 'Z';

    UnknownEnumSink unknown;
    auto r = to_order_record(of, "acc1", kTradingDay, &unknown);
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_UNKNOWN);
    ASSERT_EQ(unknown.count, 1u);
    EXPECT_STREQ(unknown.items[0].category, kUnknownCategoryPositionEffect);
}

TEST(ToOrderRecordTest, UnknownPriceTypeBecomesUnknown) {
    // 反转旧行为: 未知 OrderPriceType 不得兜成 LIMIT.
    auto of = make_order_field();
    of.OrderPriceType = 'Z';

    UnknownEnumSink unknown;
    auto r = to_order_record(of, "acc1", kTradingDay, &unknown);
    EXPECT_EQ(r.base.price_type, DZ_PRICE_UNKNOWN);
    ASSERT_EQ(unknown.count, 1u);
    EXPECT_STREQ(unknown.items[0].category, kUnknownCategoryPriceType);
    EXPECT_EQ(unknown.items[0].raw_char, 'Z');
}

TEST(ToOrderRecordTest, KnownCodesHaveNoUnknownEvidence) {
    auto of = make_order_field();
    UnknownEnumSink unknown;
    auto r = to_order_record(of, "acc1", kTradingDay, &unknown);
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_OPEN);
    EXPECT_EQ(r.base.price_type, DZ_PRICE_LIMIT);
    EXPECT_EQ(r.base.status, DZ_ORDER_PART_TRADED);
    EXPECT_TRUE(unknown.empty());
}

TEST(ToOrderRecordTest, UnknownDirectionBecomesUnknown) {
    // 回报方向只有买/卖两种: 读不懂的方向不得猜成 SHORT (净敞口符号会反向).
    auto of = make_order_field();
    of.Direction = 'Z';

    UnknownEnumSink unknown;
    auto r = to_order_record(of, "acc1", kTradingDay, &unknown);
    EXPECT_EQ(r.base.direction, DZ_DIRECTION_UNKNOWN);
    ASSERT_EQ(unknown.count, 1u);
    EXPECT_STREQ(unknown.items[0].category, kUnknownCategoryDirection);
    EXPECT_EQ(unknown.items[0].raw_char, 'Z');
}

// ============================================================================
// to_trade_record
// ============================================================================

TEST(ToTradeRecordTest, BasicFields) {
    auto tf = make_trade_field();
    auto r = to_trade_record(tf, "acc1", kTradingDay);

    // base 字段
    EXPECT_EQ(r.base.order_id, 0);  // 留 0, AccountSession 后填
    EXPECT_STREQ(r.base.instrument_id, "IF2506");
    EXPECT_DOUBLE_EQ(r.base.price, 3900.0);
    EXPECT_EQ(r.base.volume, 2);
    EXPECT_EQ(r.base.direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_OPEN);
    EXPECT_EQ(r.base.date, kTradingDay);
    EXPECT_EQ(r.base.time, 9 * 3600 + 30 * 60 + 45);  // 09:30:45
    EXPECT_STREQ(r.base.account_id, "acc1");
    EXPECT_STREQ(r.base.exchange_id, "CFFEX");
    EXPECT_STREQ(r.base.trade_id, "T001");

    // 扩展字段 (schema v1 只有 trading_day 文本列; 无 time/date/commission 派生字段)
    EXPECT_STREQ(r.trading_day, "20260727");
}

TEST(ToTradeRecordTest, SellCloseYesterdayMapping) {
    auto tf = make_trade_field();
    tf.Direction = THOST_FTDC_D_Sell;
    tf.OffsetFlag = THOST_FTDC_OF_CloseYesterday;

    auto r = to_trade_record(tf, "acc1", kTradingDay);
    EXPECT_EQ(r.base.direction, DZ_DIRECTION_SHORT);
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_CLOSE_YESTDAY);
}

TEST(ToTradeRecordTest, SellCloseMapping) {
    auto tf = make_trade_field();
    tf.Direction = THOST_FTDC_D_Sell;
    tf.OffsetFlag = THOST_FTDC_OF_Close;

    auto r = to_trade_record(tf, "acc1", kTradingDay);
    EXPECT_EQ(r.base.direction, DZ_DIRECTION_SHORT);
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_CLOSE);
}

TEST(ToTradeRecordTest, BuyCloseTodayMapping) {
    auto tf = make_trade_field();
    tf.Direction = THOST_FTDC_D_Buy;
    tf.OffsetFlag = THOST_FTDC_OF_CloseToday;

    auto r = to_trade_record(tf, "acc1", kTradingDay);
    EXPECT_EQ(r.base.direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_CLOSE_TODAY);
}

TEST(ToTradeRecordTest, UnknownOffsetBecomesUnknownNotOpen) {
    // 反转旧行为: 未知开平标志不得兜成 OPEN —— 平仓成交记成开仓会让持仓凭空放大.
    auto tf = make_trade_field();
    tf.OffsetFlag = 'Z';

    UnknownEnumSink unknown;
    auto r = to_trade_record(tf, "acc1", kTradingDay, &unknown);
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_UNKNOWN);
    ASSERT_EQ(unknown.count, 1u);
    EXPECT_STREQ(unknown.items[0].category, kUnknownCategoryPositionEffect);
    EXPECT_EQ(unknown.items[0].raw_char, 'Z');
}

TEST(ToTradeRecordTest, UnknownDirectionBecomesUnknown) {
    // 成交方向同理: 未知方向不得猜成 SHORT.
    auto tf = make_trade_field();
    tf.Direction = 'Z';

    UnknownEnumSink unknown;
    auto r = to_trade_record(tf, "acc1", kTradingDay, &unknown);
    EXPECT_EQ(r.base.direction, DZ_DIRECTION_UNKNOWN);
    ASSERT_EQ(unknown.count, 1u);
    EXPECT_STREQ(unknown.items[0].category, kUnknownCategoryDirection);
    EXPECT_EQ(unknown.items[0].raw_char, 'Z');
}

TEST(ToTradeRecordTest, KnownOffsetHasNoUnknownEvidence) {
    auto tf = make_trade_field();
    UnknownEnumSink unknown;
    auto r = to_trade_record(tf, "acc1", kTradingDay, &unknown);
    EXPECT_EQ(r.base.position_effect, DZ_POSITION_EFFECT_OPEN);
    EXPECT_TRUE(unknown.empty());
}

// ============================================================================
// to_instrument_record
// ============================================================================

TEST(ToInstrumentRecordTest, Futures) {
    auto f = make_instrument_field();
    auto r = to_instrument_record(f, "20260101");

    EXPECT_EQ(r.instrument_id, "IF2506");
    EXPECT_EQ(r.symbol, "IF2506");  // CTP 裸码: symbol == instrument_id
    EXPECT_EQ(r.exchange_id, "CFFEX");
    EXPECT_EQ(r.product_class, DZ_PRODUCT_FUTURES);
    EXPECT_EQ(r.product_code, "IF");
    EXPECT_DOUBLE_EQ(r.volume_multiple, 300);
    EXPECT_DOUBLE_EQ(r.price_tick, 0.2);
    EXPECT_EQ(r.min_limit_order_volume, 1);
    EXPECT_EQ(r.max_limit_order_volume, 500);
    EXPECT_EQ(r.min_market_order_volume, 1);
    EXPECT_EQ(r.max_market_order_volume, 500);
    EXPECT_DOUBLE_EQ(r.volume_step, 1.0);
    EXPECT_EQ(r.currency, "CNY");
    EXPECT_EQ(r.settle_cycle, -1);
    EXPECT_EQ(r.is_inverse, 0);
    EXPECT_EQ(r.listed_date, parse_ctp_date("20260119"));    // OpenDate
    EXPECT_EQ(r.delisted_date, parse_ctp_date("20260619"));  // ExpireDate
    EXPECT_EQ(r.option_type, 0);                             // 非期权
    EXPECT_EQ(r.update_day, "20260101");
    EXPECT_EQ(r.updated_at, 0);  // 由调用方填 epoch ms
}

TEST(ToInstrumentRecordTest, OptionCall) {
    auto f = make_instrument_field();
    std::strcpy(f.InstrumentID, "SR509C4800");
    std::strcpy(f.InstrumentName, "SR509C4800");
    f.ProductClass = THOST_FTDC_PC_Options;  // 场内期权 (fixture 缺省 Futures)
    f.OptionsType = THOST_FTDC_CP_CallOptions;
    f.StrikePrice = 4800.0;
    std::strcpy(f.UnderlyingInstrID, "SR509");

    auto r = to_instrument_record(f, "20260101");
    EXPECT_EQ(r.product_class, DZ_PRODUCT_OPTION);
    EXPECT_EQ(r.option_type, DZ_OPTION_CALL);  // 1
    EXPECT_DOUBLE_EQ(r.option_strike, 4800.0);
    EXPECT_EQ(r.underlying_id, "SR509");
}

TEST(ToInstrumentRecordTest, OptionPut) {
    auto f = make_instrument_field();
    std::strcpy(f.InstrumentID, "SR509P4800");
    f.OptionsType = THOST_FTDC_CP_PutOptions;
    f.StrikePrice = 4800.0;

    auto r = to_instrument_record(f, "20260101");
    EXPECT_EQ(r.option_type, DZ_OPTION_PUT);  // -1
    EXPECT_DOUBLE_EQ(r.option_strike, 4800.0);
}

TEST(ToInstrumentRecordTest, UnknownProductNotFutures) {
    // 未识别 ProductClass 暴露为 UNKNOWN (v1 兜底为 'F', v2 改为显式暴露)
    auto f = make_instrument_field();
    f.ProductClass = static_cast<TThostFtdcProductClassType>('Z');

    auto r = to_instrument_record(f, "20260101");
    EXPECT_EQ(r.product_class, DZ_PRODUCT_UNKNOWN);
}

TEST(ToInstrumentRecordTest, SpotOptionIsOption) {
    // 商品期权 ProductClass=SpotOption('6') 非 Options('2') — 漏映射则全部
    // 商品期权变 UNKNOWN (v1 时代 default 兜底 'F' 蒙混, v2 必须显式映射)
    auto f = make_instrument_field();
    std::strcpy(f.InstrumentID, "sr509C4800");
    f.ProductClass = THOST_FTDC_PC_SpotOption;
    f.OptionsType = THOST_FTDC_CP_CallOptions;
    f.StrikePrice = 4800.0;
    std::strcpy(f.UnderlyingInstrID, "SR509");

    auto r = to_instrument_record(f, "20260101");
    EXPECT_EQ(r.product_class, DZ_PRODUCT_OPTION);
    EXPECT_EQ(r.option_type, DZ_OPTION_CALL);
}

TEST(ToInstrumentRecordTest, CombinationMapsToSpread) {
    // 《帧契约：合约信息》§6: CTP Combination -> DZ_PRODUCT_SPREAD
    auto f = make_instrument_field();
    f.ProductClass = THOST_FTDC_PC_Combination;

    auto r = to_instrument_record(f, "20260101");
    EXPECT_EQ(r.product_class, DZ_PRODUCT_SPREAD);
}

TEST(ToInstrumentRecordTest, SpotMapsToSpot) {
    // 《帧契约：合约信息》§6: CTP Spot -> DZ_PRODUCT_SPOT
    auto f = make_instrument_field();
    f.ProductClass = THOST_FTDC_PC_Spot;

    auto r = to_instrument_record(f, "20260101");
    EXPECT_EQ(r.product_class, DZ_PRODUCT_SPOT);
}

TEST(ToInstrumentRecordTest, EmptyDateIsNa) {
    // CTP 字段为空 -> DZ_DATE_NA(0), 非 v1 的 -1
    auto f = make_instrument_field();
    std::strcpy(f.OpenDate, "");
    std::strcpy(f.ExpireDate, "");

    auto r = to_instrument_record(f, "20260101");
    EXPECT_EQ(r.listed_date, DZ_DATE_NA);
    EXPECT_EQ(r.delisted_date, DZ_DATE_NA);
}

TEST(ToInstrumentRecordTest, NameIsUtf8) {
    // "沪" 的 GBK 字节. Windows SDK 返回 GBK, 需转 UTF-8;
    // Linux SDK 已返回 UTF-8, to_utf8_from_gbk 为恒等透传.
    const std::string gbk = std::string("\xBB\xA6", 2);
    auto f = make_instrument_field();
    std::strcpy(f.InstrumentName, gbk.c_str());

    auto r = to_instrument_record(f, "20260101");
    // 期望值硬编码且不经 to_utf8_from_gbk, 转换被改坏时两个平台都会红.
#ifdef _WIN32
    EXPECT_EQ(r.name, std::string("\xE6\xB2\xAA", 3));  // GBK "沪" -> UTF-8 "沪"
#else
    EXPECT_EQ(r.name, gbk);  // Linux 原样透传 (SDK 已是 UTF-8)
#endif
}

TEST(ToInstrumentRecordTest, ProductCodeAndUnderlyingMultiple) {
    auto f = make_instrument_field();
    std::strcpy(f.ProductID, "rb");
    f.UnderlyingMultiple = 10.0;

    auto r = to_instrument_record(f, "20260101");
    EXPECT_EQ(r.product_code, "rb");
    EXPECT_DOUBLE_EQ(r.underlying_multiple, 10.0);
}

TEST(ToInstrumentRecordTest, MoneyDefaults) {
    auto f = make_instrument_field();
    auto r = to_instrument_record(f, "20260101");
    EXPECT_EQ(r.settle_cycle, -1);  // CTP 衍生品语义: 不适用
    EXPECT_EQ(r.currency, "CNY");
    EXPECT_DOUBLE_EQ(r.volume_step, 1.0);
    EXPECT_EQ(r.base_asset, "");
    EXPECT_EQ(r.is_inverse, 0);
}

// ============================================================================
// to_dz_trading_account
// ============================================================================

TEST(ToDzTradingAccountTest, BasicFields) {
    auto af = make_account_field();
    auto a = to_dz_trading_account(af, "acc1", kTradingDay);

    EXPECT_STREQ(a.account_id, "acc1");
    EXPECT_DOUBLE_EQ(a.balance, 100000.0);
    EXPECT_DOUBLE_EQ(a.available, 80000.0);
    EXPECT_DOUBLE_EQ(a.frozen, 5000.0);  // CTP FrozenMargin -> DZ frozen
    EXPECT_DOUBLE_EQ(a.commission, 100.0);
    EXPECT_DOUBLE_EQ(a.margin, 15000.0);  // CTP CurrMargin -> DZ margin
    EXPECT_DOUBLE_EQ(a.withdraw_quota, 70000.0);
    EXPECT_DOUBLE_EQ(a.deposit, 0.0);
    EXPECT_DOUBLE_EQ(a.withdraw, 0.0);
    EXPECT_EQ(a.date, kTradingDay);
}

// ============================================================================
// to_dz_position
// ============================================================================

TEST(ToDzPositionTest, LongPositionMapsFields) {
    auto pf = make_position_field();
    auto p = to_dz_position(pf, "acc1", kTradingDay, 1.0);

    EXPECT_STREQ(p.instrument_id, "IF2506");
    EXPECT_STREQ(p.exchange_id, "CFFEX");
    EXPECT_STREQ(p.account_id, "acc1");
    EXPECT_EQ(p.direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(p.volume, 10);
    EXPECT_EQ(p.frozen_volume, 3);      // LongFrozen(2) + ShortFrozen(1)
    EXPECT_DOUBLE_EQ(p.price, 3900.0);  // PositionCost / (Position * 1.0)
    EXPECT_EQ(p.today_volume, 6);       // TodayPosition
    EXPECT_EQ(p.yd_volume, 4);          // Position - TodayPosition (不是静态 YdPosition=6)
    EXPECT_EQ(p.date, kTradingDay);
}

TEST(ToDzPositionTest, StaticYdPositionIsNotCurrentYesterdayVolume) {
    // YdPosition 是交易日开始时的静态昨仓, 平昨后不减少. 当前昨仓 = Position - TodayPosition.
    // 夹具历史: 开盘昨仓 6, 盘中开 6 手今仓、平昨 2 手 → 当前今 6 / 昨 4 / 总 10.
    auto pf = make_position_field();
    auto p = to_dz_position(pf, "acc1", kTradingDay, 1.0);
    ASSERT_EQ(pf.YdPosition, 6);  // 静态值仍是 6, 不能当作当前昨仓
    EXPECT_EQ(p.yd_volume, 4);
    EXPECT_EQ(p.today_volume, 6);

    // 静态值本身变化不影响当前今昨拆分 (只由 Position / TodayPosition 决定).
    pf.YdPosition = 2;
    auto p2 = to_dz_position(pf, "acc1", kTradingDay, 1.0);
    EXPECT_EQ(p2.yd_volume, 4);
    EXPECT_EQ(p2.today_volume, 6);
}

TEST(ToDzPositionTest, ShfeTodayRowMapsAllVolumeToToday) {
    // SHFE/INE 今仓行: Position = 今天, TodayPosition = 今天, YdPosition = 0 → yd = 0.
    auto pf = make_position_field();
    std::strcpy(pf.ExchangeID, "SHFE");
    pf.Position = 6;
    pf.TodayPosition = 6;
    pf.YdPosition = 0;
    auto p = to_dz_position(pf, "acc1", kTradingDay, 1.0);
    EXPECT_EQ(p.volume, 6);
    EXPECT_EQ(p.today_volume, 6);
    EXPECT_EQ(p.yd_volume, 0);
}

TEST(ToDzPositionTest, ShfeYesterdayRowMapsAllVolumeToYesterday) {
    // SHFE/INE 昨仓行: Position = 剩余昨仓, TodayPosition = 0, YdPosition = 开盘静态昨仓
    // (已平掉的昨仓不减少它) → today = 0, yd = 剩余昨仓.
    auto pf = make_position_field();
    std::strcpy(pf.ExchangeID, "SHFE");
    pf.Position = 5;
    pf.TodayPosition = 0;
    pf.YdPosition = 5;
    auto p = to_dz_position(pf, "acc1", kTradingDay, 1.0);
    EXPECT_EQ(p.volume, 5);
    EXPECT_EQ(p.today_volume, 0);
    EXPECT_EQ(p.yd_volume, 5);
}

TEST(ToDzPositionTest, TodayPositionOutOfRangeIsClamped) {
    // 柜台坏数据: TodayPosition > Position 时 today 夹到 [0, Position], yd 由减法派生必 >= 0.
    auto pf = make_position_field();
    pf.Position = 5;
    pf.TodayPosition = 8;
    auto p = to_dz_position(pf, "acc1", kTradingDay, 1.0);
    EXPECT_EQ(p.volume, 5);
    EXPECT_EQ(p.today_volume, 5);
    EXPECT_EQ(p.yd_volume, 0);
    EXPECT_GE(p.today_volume, 0);
    EXPECT_GE(p.yd_volume, 0);

    // 另一侧越界同样夹取: 负 TodayPosition → today = 0, 全部计入昨仓.
    pf.TodayPosition = -3;
    auto p2 = to_dz_position(pf, "acc1", kTradingDay, 1.0);
    EXPECT_EQ(p2.volume, 5);
    EXPECT_EQ(p2.today_volume, 0);
    EXPECT_EQ(p2.yd_volume, 5);
}

TEST(ToDzPositionTest, VolumeMultipleScalesAveragePrice) {
    // PositionCost 是金额: 均价 = PositionCost / (持仓量 × 合约乘数)
    auto pf = make_position_field();
    auto p = to_dz_position(pf, "acc1", kTradingDay, 300.0);
    EXPECT_DOUBLE_EQ(p.price, 13.0);  // 39000 / (10 * 300)
}

TEST(ToDzPositionTest, ShortPositionMapsDirection) {
    auto pf = make_position_field();
    pf.PosiDirection = THOST_FTDC_PD_Short;
    auto p = to_dz_position(pf, "acc1", kTradingDay, 1.0);
    EXPECT_EQ(p.direction, DZ_DIRECTION_SHORT);
}

TEST(ToDzPositionTest, NetPositionTreatedAsLong) {
    // CTP 净持仓 (组合等) 归为多头 (DZ 仅多/空两态)
    auto pf = make_position_field();
    pf.PosiDirection = THOST_FTDC_PD_Net;
    auto p = to_dz_position(pf, "acc1", kTradingDay, 1.0);
    EXPECT_EQ(p.direction, DZ_DIRECTION_LONG);
}

TEST(ToDzPositionTest, ZeroPositionHasZeroPrice) {
    auto pf = make_position_field();
    pf.Position = 0;
    pf.PositionCost = 39000.0;
    auto p = to_dz_position(pf, "acc1", kTradingDay, 1.0);
    EXPECT_EQ(p.volume, 0);
    EXPECT_DOUBLE_EQ(p.price, 0.0);  // 避免除零
    // Position=0 时 TodayPosition 一并夹到 [0, 0]: 旧口径的负今仓 (today = -YdPosition) 是 bug.
    EXPECT_EQ(p.today_volume, 0);
    EXPECT_EQ(p.yd_volume, 0);
}

TEST(ToDzPositionTest, UnknownPosiDirectionBecomesUnknown) {
    // 反转旧行为: 未知方向不得兜成 LONG —— 空头被记成多头会让净敞口符号反向.
    auto pf = make_position_field();
    pf.PosiDirection = 'Z';

    UnknownEnumSink unknown;
    auto p = to_dz_position(pf, "acc1", kTradingDay, 1.0, &unknown);
    EXPECT_EQ(p.direction, DZ_DIRECTION_UNKNOWN);
    ASSERT_EQ(unknown.count, 1u);
    EXPECT_STREQ(unknown.items[0].category, kUnknownCategoryDirection);
    EXPECT_EQ(unknown.items[0].raw_char, 'Z');
}

TEST(ToDzPositionTest, KnownDirectionsHaveNoUnknownEvidence) {
    UnknownEnumSink unknown;
    auto long_field = make_position_field();  // 缺省 Long('2')
    EXPECT_EQ(to_dz_position(long_field, "acc1", kTradingDay, 1.0, &unknown).direction,
              DZ_DIRECTION_LONG);

    auto net_field = make_position_field();
    net_field.PosiDirection = THOST_FTDC_PD_Net;
    EXPECT_EQ(to_dz_position(net_field, "acc1", kTradingDay, 1.0, &unknown).direction,
              DZ_DIRECTION_LONG);

    auto short_field = make_position_field();
    short_field.PosiDirection = THOST_FTDC_PD_Short;
    EXPECT_EQ(to_dz_position(short_field, "acc1", kTradingDay, 1.0, &unknown).direction,
              DZ_DIRECTION_SHORT);
    EXPECT_TRUE(unknown.empty());
}
