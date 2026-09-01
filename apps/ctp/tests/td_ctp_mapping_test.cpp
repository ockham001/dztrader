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
    f.ProductClass = THOST_FTDC_PC_Futures;
    f.VolumeMultiple = 300;
    f.PriceTick = 0.2;
    f.MinLimitOrderVolume = 1;
    f.MaxLimitOrderVolume = 500;
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

/// 辅助: 构造一个 DzOrderReq
DzOrderReq make_order_req(DzPriceType pt, DzDirection dir, DzPositionEffect off,
                          int32_t volume, double price = 3900.0) {
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
    EXPECT_EQ(parse_ctp_time("9:30:00"), -1);   // 长度不足
    EXPECT_EQ(parse_ctp_time("09:30"), -1);     // 长度不足
    EXPECT_EQ(parse_ctp_time("09:30:00:00"), -1);  // 长度过长
    EXPECT_EQ(parse_ctp_time("09:30:0a"), -1);  // 非数字
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
    EXPECT_EQ(parse_ctp_date(nullptr), -1);
    EXPECT_EQ(parse_ctp_date(""), -1);
}

TEST(ParseCtpDateTest, InvalidFormat) {
    EXPECT_EQ(parse_ctp_date("abc"), -1);
    EXPECT_EQ(parse_ctp_date("2026072"), -1);   // 长度不足
    EXPECT_EQ(parse_ctp_date("202607277"), -1);  // 长度过长
    EXPECT_EQ(parse_ctp_date("2026072a"), -1);  // 非数字
}

TEST(ParseCtpDateTest, InvalidDate) {
    EXPECT_EQ(parse_ctp_date("20260230"), -1);  // 2 月 30 日不存在
    EXPECT_EQ(parse_ctp_date("20261301"), -1);  // 月份越界
    EXPECT_EQ(parse_ctp_date("20260001"), -1);  // 月份为 0
}

// ============================================================================
// normalize_to_product
// ============================================================================

TEST(NormalizeToProductTest, DigitSuffix) {
    EXPECT_EQ(normalize_to_product("IF2506"), "IF");
    EXPECT_EQ(normalize_to_product("rb2510"), "rb");
    EXPECT_EQ(normalize_to_product("T2509"), "T");
    EXPECT_EQ(normalize_to_product("SR509C4800"), "SR");  // 期权: 取首次数字之前
}

TEST(NormalizeToProductTest, NoDigit) {
    EXPECT_EQ(normalize_to_product("IF"), "IF");
    EXPECT_EQ(normalize_to_product(""), "");
    EXPECT_EQ(normalize_to_product("rb"), "rb");
}

// ============================================================================
// STATUS_CTP2VT (设计 §12.3 状态映射)
// ============================================================================

TEST(StatusCtp2VtTest, Map) {
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_AllTraded), DZ_ORDER_ALL_TRADED);             // '0'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_PartTradedQueueing), DZ_ORDER_PART_TRADED);   // '1'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_PartTradedNotQueueing), DZ_ORDER_PART_TRADED);  // '2'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_NoTradeQueueing), DZ_ORDER_NOT_TRADED);       // '3'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_NoTradeNotQueueing), DZ_ORDER_CANCELLED);     // '4'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_Canceled), DZ_ORDER_CANCELLED);               // '5'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_Unknown), DZ_ORDER_SUBMITTING);               // 'a'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_NotTouched), DZ_ORDER_NOT_TRADED);            // 'b'
    EXPECT_EQ(STATUS_CTP2VT(THOST_FTDC_OST_Touched), DZ_ORDER_PART_TRADED);              // 'c'
}

TEST(StatusCtp2VtTest, UnknownDefault) {
    EXPECT_EQ(STATUS_CTP2VT('z'), DZ_ORDER_SUBMITTING);
    EXPECT_EQ(STATUS_CTP2VT('\0'), DZ_ORDER_SUBMITTING);
    EXPECT_EQ(STATUS_CTP2VT('X'), DZ_ORDER_SUBMITTING);
}

// ============================================================================
// ORDERTYPE_VT2CTP
// ============================================================================

TEST(OrderTypeVt2CtpTest, Limit) {
    auto t = ORDERTYPE_VT2CTP(DZ_PRICE_LIMIT);
    EXPECT_EQ(t.limit_price_type, THOST_FTDC_OPT_LimitPrice);
    EXPECT_EQ(t.time_condition, THOST_FTDC_TC_GFD);
    EXPECT_EQ(t.volume_condition, THOST_FTDC_VC_MV);
}

TEST(OrderTypeVt2CtpTest, Market) {
    auto t = ORDERTYPE_VT2CTP(DZ_PRICE_MARKET);
    EXPECT_EQ(t.limit_price_type, THOST_FTDC_OPT_AnyPrice);
    EXPECT_EQ(t.time_condition, THOST_FTDC_TC_IOC);
    EXPECT_EQ(t.volume_condition, THOST_FTDC_VC_AV);
}

TEST(OrderTypeVt2CtpTest, Fak) {
    auto t = ORDERTYPE_VT2CTP(DZ_PRICE_FAK);
    EXPECT_EQ(t.limit_price_type, THOST_FTDC_OPT_LimitPrice);
    EXPECT_EQ(t.time_condition, THOST_FTDC_TC_IOC);
    EXPECT_EQ(t.volume_condition, THOST_FTDC_VC_MV);
}

TEST(OrderTypeVt2CtpTest, Fok) {
    // CTP 无 FOK 时间条件, 用 IOC + CV 模拟 (MinVolume 由 to_input_order_field 设为 volume)
    auto t = ORDERTYPE_VT2CTP(DZ_PRICE_FOK);
    EXPECT_EQ(t.limit_price_type, THOST_FTDC_OPT_LimitPrice);
    EXPECT_EQ(t.time_condition, THOST_FTDC_TC_IOC);
    EXPECT_EQ(t.volume_condition, THOST_FTDC_VC_CV);
}

TEST(OrderTypeVt2CtpTest, UnsupportedFallback) {
    // STOP/RFQ 期货不支持, 兜底返回 LimitPrice + GFD
    auto t = ORDERTYPE_VT2CTP(DZ_PRICE_STOP);
    EXPECT_EQ(t.limit_price_type, THOST_FTDC_OPT_LimitPrice);
    EXPECT_EQ(t.time_condition, THOST_FTDC_TC_GFD);
    auto t2 = ORDERTYPE_VT2CTP(DZ_PRICE_RFQ);
    EXPECT_EQ(t2.limit_price_type, THOST_FTDC_OPT_LimitPrice);
}

// ============================================================================
// to_input_order_field
// ============================================================================

TEST(ToInputOrderFieldTest, LimitBuyOpen) {
    auto req = make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 5);
    OrderBuildContext ctx{"acc1", 123, 1, "CFFEX"};
    auto f = to_input_order_field(req, ctx);

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
    auto req = make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_SHORT,
                              DZ_POSITION_EFFECT_CLOSE_TODAY, 3);
    OrderBuildContext ctx{"acc1", 999, 2, "SHFE"};
    auto f = to_input_order_field(req, ctx);

    EXPECT_EQ(f.Direction, THOST_FTDC_D_Sell);
    EXPECT_EQ(f.CombOffsetFlag[0], THOST_FTDC_OF_CloseToday);
    EXPECT_EQ(f.MinVolume, 1);
}

TEST(ToInputOrderFieldTest, BuyCloseYesterday) {
    auto req = make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_LONG,
                              DZ_POSITION_EFFECT_CLOSE_YESTDAY, 2);
    OrderBuildContext ctx{"acc1", 1, 3, "SHFE"};
    auto f = to_input_order_field(req, ctx);

    EXPECT_EQ(f.Direction, THOST_FTDC_D_Buy);
    EXPECT_EQ(f.CombOffsetFlag[0], THOST_FTDC_OF_CloseYesterday);
}

TEST(ToInputOrderFieldTest, Close) {
    auto req = make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_SHORT,
                              DZ_POSITION_EFFECT_CLOSE, 1);
    OrderBuildContext ctx{"acc1", 1, 4, "CFFEX"};
    auto f = to_input_order_field(req, ctx);

    EXPECT_EQ(f.CombOffsetFlag[0], THOST_FTDC_OF_Close);
}

TEST(ToInputOrderFieldTest, FokMinVolume) {
    // FOK 模拟: MinVolume 应等于 volume
    auto req = make_order_req(DZ_PRICE_FOK, DZ_DIRECTION_LONG,
                              DZ_POSITION_EFFECT_OPEN, 7);
    OrderBuildContext ctx{"acc1", 1, 5, "CFFEX"};
    auto f = to_input_order_field(req, ctx);

    EXPECT_EQ(f.TimeCondition, THOST_FTDC_TC_IOC);
    EXPECT_EQ(f.VolumeCondition, THOST_FTDC_VC_CV);
    EXPECT_EQ(f.MinVolume, 7);  // FOK 关键: MinVolume = VolumeTotalOriginal
    EXPECT_EQ(f.OrderPriceType, THOST_FTDC_OPT_LimitPrice);
}

TEST(ToInputOrderFieldTest, FakMinVolume) {
    // FAK: MinVolume 保持 1 (允许部分成交)
    auto req = make_order_req(DZ_PRICE_FAK, DZ_DIRECTION_LONG,
                              DZ_POSITION_EFFECT_OPEN, 7);
    OrderBuildContext ctx{"acc1", 1, 6, "CFFEX"};
    auto f = to_input_order_field(req, ctx);

    EXPECT_EQ(f.TimeCondition, THOST_FTDC_TC_IOC);
    EXPECT_EQ(f.MinVolume, 1);
}

TEST(ToInputOrderFieldTest, OrderRefFormat) {
    // OrderRef 必须 12 位补零, 边界值测试
    auto req = make_order_req(DZ_PRICE_LIMIT, DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 1);

    OrderBuildContext ctx1{"acc1", 0, 0, "CFFEX"};
    auto f1 = to_input_order_field(req, ctx1);
    EXPECT_STREQ(f1.OrderRef, "000000000000");

    OrderBuildContext ctx2{"acc1", 999999999999LL, 0, "CFFEX"};
    auto f2 = to_input_order_field(req, ctx2);
    EXPECT_STREQ(f2.OrderRef, "999999999999");
}

TEST(ToInputOrderFieldTest, MarketPriceIgnored) {
    // 市价单 LimitPrice 应被忽略, 但仍拷贝 req.price
    auto req = make_order_req(DZ_PRICE_MARKET, DZ_DIRECTION_LONG, DZ_POSITION_EFFECT_OPEN, 1, 0.0);
    OrderBuildContext ctx{"acc1", 1, 7, "CFFEX"};
    auto f = to_input_order_field(req, ctx);

    EXPECT_EQ(f.OrderPriceType, THOST_FTDC_OPT_AnyPrice);
    EXPECT_EQ(f.TimeCondition, THOST_FTDC_TC_IOC);
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
    EXPECT_EQ(r.is_external, 0);        // 留 0, AccountSession 后填
    EXPECT_EQ(r.volume_canceled, 0);    // 留 0, AccountSession 后推导
    EXPECT_EQ(r.error_id, 0);
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

    // 扩展字段
    EXPECT_EQ(r.commission, 0.0);  // 留 0, AccountSession 后填
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

// ============================================================================
// to_dz_instrument
// ============================================================================

TEST(ToDzInstrumentInfoTest, Futures) {
    auto f = make_instrument_field();
    auto c = to_dz_instrument(f);

    EXPECT_STREQ(c.instrument_id, "IF2506");
    EXPECT_STREQ(c.exchange_id, "CFFEX");
    EXPECT_EQ(c.volume_multiple, 300);
    EXPECT_DOUBLE_EQ(c.price_tick, 0.2);
    EXPECT_EQ(c.min_order_volume, 1);
    EXPECT_EQ(c.max_order_volume, 500);
    EXPECT_EQ(c.option_type, 0);  // 非期权
    EXPECT_EQ(c.option_listed, parse_ctp_date("20260119"));  // OpenDate
    EXPECT_EQ(c.option_expiry, parse_ctp_date("20260619"));  // ExpireDate
}

TEST(ToDzInstrumentInfoTest, OptionCall) {
    auto f = make_instrument_field();
    std::strcpy(f.InstrumentID, "SR509C4800");
    std::strcpy(f.InstrumentName, "SR509C4800");
    f.OptionsType = THOST_FTDC_CP_CallOptions;
    f.StrikePrice = 4800.0;
    std::strcpy(f.UnderlyingInstrID, "SR509");

    auto c = to_dz_instrument(f);
    EXPECT_STREQ(c.instrument_id, "SR509C4800");
    EXPECT_EQ(c.option_type, DZ_OPTION_CALL);  // 1
    EXPECT_DOUBLE_EQ(c.option_strike, 4800.0);
    EXPECT_STREQ(c.option_underlying, "SR509");
}

TEST(ToDzInstrumentInfoTest, OptionPut) {
    auto f = make_instrument_field();
    std::strcpy(f.InstrumentID, "SR509P4800");
    f.OptionsType = THOST_FTDC_CP_PutOptions;
    f.StrikePrice = 4800.0;

    auto c = to_dz_instrument(f);
    EXPECT_EQ(c.option_type, DZ_OPTION_PUT);  // -1
    EXPECT_DOUBLE_EQ(c.option_strike, 4800.0);
}

TEST(ToDzInstrumentInfoTest, EmptyDate) {
    // CTP 字段为空时不应崩溃, 返回 -1
    auto f = make_instrument_field();
    std::strcpy(f.OpenDate, "");
    std::strcpy(f.ExpireDate, "");

    auto c = to_dz_instrument(f);
    EXPECT_EQ(c.option_listed, -1);
    EXPECT_EQ(c.option_expiry, -1);
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
