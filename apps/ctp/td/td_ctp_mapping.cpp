#include "td/td_ctp_mapping.h"

#include <cstdio>
#include <cstring>
#include <format>

#include <dztrader/core/string_util.h>  // copy_string
#include <dztrader/date_time/date.h>     // Date

namespace dztrader::ctp {

// ============================================================================
// 内部辅助: CTP 字符串 -> DZ 字段 (带截断保护)
// ============================================================================

namespace {

/// 安全拷贝 CTP 字符串到 DZ 字段. 源为空指针时写空串.
template <size_t N>
void copy_to_dz(char (&dest)[N], const char* src) noexcept {
    copy_string(dest, src, /*truncate=*/true);
}

}  // namespace

// ============================================================================
// parse_ctp_time: "HH:MM:SS" -> 距午夜秒数
// ============================================================================

int32_t parse_ctp_time(const char* hh_mm_ss) noexcept {
    if (hh_mm_ss == nullptr || hh_mm_ss[0] == '\0') {
        return -1;
    }
    // 严格 "HH:MM:SS" 格式, 长度必须 8
    if (std::strlen(hh_mm_ss) != 8) {
        return -1;
    }
    if (hh_mm_ss[2] != ':' || hh_mm_ss[5] != ':') {
        return -1;
    }
    for (int i : {0, 1, 3, 4, 6, 7}) {
        if (hh_mm_ss[i] < '0' || hh_mm_ss[i] > '9') {
            return -1;
        }
    }

    int32_t hh = (hh_mm_ss[0] - '0') * 10 + (hh_mm_ss[1] - '0');
    int32_t mm = (hh_mm_ss[3] - '0') * 10 + (hh_mm_ss[4] - '0');
    int32_t ss = (hh_mm_ss[6] - '0') * 10 + (hh_mm_ss[7] - '0');

    if (hh >= 24 || mm >= 60 || ss >= 60) {
        return -1;
    }
    return hh * 3600 + mm * 60 + ss;
}

// ============================================================================
// parse_ctp_date: "YYYYMMDD" -> 距纪元天数 (DzDate)
// ============================================================================

int32_t parse_ctp_date(const char* yyyymmdd) noexcept {
    if (yyyymmdd == nullptr || yyyymmdd[0] == '\0') {
        return -1;
    }
    if (std::strlen(yyyymmdd) != 8) {
        return -1;
    }
    for (int i = 0; i < 8; ++i) {
        if (yyyymmdd[i] < '0' || yyyymmdd[i] > '9') {
            return -1;
        }
    }

    int32_t y = (yyyymmdd[0] - '0') * 1000 + (yyyymmdd[1] - '0') * 100 +
                (yyyymmdd[2] - '0') * 10 + (yyyymmdd[3] - '0');
    int32_t m = (yyyymmdd[4] - '0') * 10 + (yyyymmdd[5] - '0');
    int32_t d = (yyyymmdd[6] - '0') * 10 + (yyyymmdd[7] - '0');

    // Date::from_year_month_day 会校验 y/m/d 范围, 失败抛异常
    try {
        return Date::from_year_month_day(y, m, d).days_since_epoch();
    } catch (...) {
        return -1;
    }
}

// ============================================================================
// normalize_to_product: 提取首个数字之前的前缀作为品种代码
// ============================================================================

std::string normalize_to_product(const std::string& instrument_id) {
    if (instrument_id.empty()) {
        return {};
    }
    // 取首个数字之前的前缀 (品种代码). 期货如 "IF2506" -> "IF",
    // 期权如 "SR509C4800" -> "SR", "IO2506-C-3900" -> "IO".
    size_t i = 0;
    while (i < instrument_id.size() &&
           (instrument_id[i] < '0' || instrument_id[i] > '9')) {
        ++i;
    }
    return instrument_id.substr(0, i);
}

// ============================================================================
// STATUS_CTP2VT: CTP OrderStatus -> DzOrderStatus
// ============================================================================

DzOrderStatus STATUS_CTP2VT(char ctp_status) noexcept {
    switch (ctp_status) {
        case THOST_FTDC_OST_AllTraded:               return DZ_ORDER_ALL_TRADED;    // '0'
        case THOST_FTDC_OST_PartTradedQueueing:       return DZ_ORDER_PART_TRADED;   // '1'
        case THOST_FTDC_OST_PartTradedNotQueueing:    return DZ_ORDER_PART_TRADED;   // '2'
        case THOST_FTDC_OST_NoTradeQueueing:          return DZ_ORDER_NOT_TRADED;    // '3'
        case THOST_FTDC_OST_NoTradeNotQueueing:       return DZ_ORDER_CANCELLED;     // '4'
        case THOST_FTDC_OST_Canceled:                 return DZ_ORDER_CANCELLED;     // '5'
        case THOST_FTDC_OST_NotTouched:               return DZ_ORDER_NOT_TRADED;    // 'b'
        case THOST_FTDC_OST_Touched:                  return DZ_ORDER_PART_TRADED;   // 'c'
        case THOST_FTDC_OST_Unknown:                  return DZ_ORDER_SUBMITTING;    // 'a'
        default:                                      return DZ_ORDER_SUBMITTING;    // 兜底
    }
}

// ============================================================================
// ORDERTYPE_VT2CTP: DzPriceType -> CTP 价格类型三元组
// ============================================================================

OrderTypeTriplet ORDERTYPE_VT2CTP(DzPriceType t) noexcept {
    OrderTypeTriplet r{};
    switch (t) {
        case DZ_PRICE_LIMIT:
            r.limit_price_type = THOST_FTDC_OPT_LimitPrice;
            r.time_condition = THOST_FTDC_TC_GFD;
            r.volume_condition = THOST_FTDC_VC_MV;  // MinVolume (配合 to_input_order_field 设 1)
            break;
        case DZ_PRICE_MARKET:
            r.limit_price_type = THOST_FTDC_OPT_AnyPrice;
            r.time_condition = THOST_FTDC_TC_IOC;
            r.volume_condition = THOST_FTDC_VC_AV;
            break;
        case DZ_PRICE_FAK:
            r.limit_price_type = THOST_FTDC_OPT_LimitPrice;
            r.time_condition = THOST_FTDC_TC_IOC;
            r.volume_condition = THOST_FTDC_VC_MV;
            break;
        case DZ_PRICE_FOK:
            // CTP 无 FOK 时间条件, 用 IOC + CV(全部数量) + MinVolume=VolumeTotalOriginal 模拟
            r.limit_price_type = THOST_FTDC_OPT_LimitPrice;
            r.time_condition = THOST_FTDC_TC_IOC;
            r.volume_condition = THOST_FTDC_VC_CV;
            break;
        default:
            // STOP/RFQ 期货不支持, 兜底为限价 (不会实际发出, 由调用方拦截)
            r.limit_price_type = THOST_FTDC_OPT_LimitPrice;
            r.time_condition = THOST_FTDC_TC_GFD;
            r.volume_condition = THOST_FTDC_VC_MV;
            break;
    }
    return r;
}

// ============================================================================
// to_input_order_field: DzOrderReq + ctx -> CThostFtdcInputOrderField
// ============================================================================

CThostFtdcInputOrderField to_input_order_field(const DzOrderReq& req,
                                                const OrderBuildContext& ctx) noexcept {
    CThostFtdcInputOrderField f{};

    // 标识字段
    copy_to_dz(f.InvestorID, ctx.account_id.c_str());
    copy_to_dz(f.InstrumentID, req.instrument_id);
    copy_to_dz(f.ExchangeID, ctx.exchange_id.c_str());

    // OrderRef: 12 位补零 (CTP TThostFtdcOrderRefType[13], 末位留给 null)
    std::snprintf(f.OrderRef, sizeof(f.OrderRef), "%012lld",
                  static_cast<long long>(ctx.order_ref));

    f.RequestID = ctx.request_id;

    // 买卖方向: DZ LONG(1) -> CTP Buy('0'), DZ SHORT(-1) -> CTP Sell('1')
    f.Direction = (req.direction == DZ_DIRECTION_LONG) ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;

    // 开平标志: CombOffsetFlag[0]
    switch (req.position_effect) {
        case DZ_POSITION_EFFECT_OPEN:           f.CombOffsetFlag[0] = THOST_FTDC_OF_Open;            break;
        case DZ_POSITION_EFFECT_CLOSE:          f.CombOffsetFlag[0] = THOST_FTDC_OF_Close;          break;
        case DZ_POSITION_EFFECT_CLOSE_TODAY:    f.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;     break;
        case DZ_POSITION_EFFECT_CLOSE_YESTDAY:  f.CombOffsetFlag[0] = THOST_FTDC_OF_CloseYesterday; break;
        default:                                f.CombOffsetFlag[0] = THOST_FTDC_OF_Open;            break;
    }

    // 价格类型三元组
    auto triplet = ORDERTYPE_VT2CTP(req.price_type);
    f.OrderPriceType = triplet.limit_price_type;
    f.TimeCondition = triplet.time_condition;
    f.VolumeCondition = triplet.volume_condition;

    // 价格 + 数量
    f.LimitPrice = req.price;
    f.VolumeTotalOriginal = req.volume;

    // MinVolume: FOK 用 volume 模拟"全部成交否则撤销", 其他类型默认 1
    if (req.price_type == DZ_PRICE_FOK) {
        f.MinVolume = req.volume;
    } else {
        f.MinVolume = 1;
    }

    // 默认值: 投机套保标志, 触发条件, 强平原因等
    f.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    f.ContingentCondition = THOST_FTDC_CC_Immediately;
    f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    f.IsAutoSuspend = 0;
    f.UserForceClose = 0;

    return f;
}

// ============================================================================
// to_order_record: CTP OrderField -> OrderRecord
// ============================================================================

OrderRecord to_order_record(const CThostFtdcOrderField& o,
                             const std::string& account_id,
                             int32_t trading_day) noexcept {
    OrderRecord r{};

    // ---- base (DzOrderReport) ----
    r.base.order_id = 0;  // 留 0, AccountSession 查 OrderRefMap 后填
    // strategy_id 留空, AccountSession 后填
    copy_to_dz(r.base.instrument_id, o.InstrumentID);
    copy_to_dz(r.base.account_id, account_id.c_str());
    copy_to_dz(r.base.exchange_id, o.ExchangeID);

    // 买卖方向
    r.base.direction = (o.Direction == THOST_FTDC_D_Buy) ? DZ_DIRECTION_LONG : DZ_DIRECTION_SHORT;

    // 开平标志 (CombOffsetFlag[0])
    switch (o.CombOffsetFlag[0]) {
        case THOST_FTDC_OF_Open:           r.base.position_effect = DZ_POSITION_EFFECT_OPEN;           break;
        case THOST_FTDC_OF_Close:          r.base.position_effect = DZ_POSITION_EFFECT_CLOSE;          break;
        case THOST_FTDC_OF_CloseToday:     r.base.position_effect = DZ_POSITION_EFFECT_CLOSE_TODAY;    break;
        case THOST_FTDC_OF_CloseYesterday: r.base.position_effect = DZ_POSITION_EFFECT_CLOSE_YESTDAY;  break;
        default:                           r.base.position_effect = DZ_POSITION_EFFECT_OPEN;           break;
    }

    // 价格类型: 由 OrderPriceType 反推 (FAK/FOK 在 OrderField 中无独立标志, 一律归为 LIMIT)
    switch (o.OrderPriceType) {
        case THOST_FTDC_OPT_AnyPrice:  r.base.price_type = DZ_PRICE_MARKET; break;
        case THOST_FTDC_OPT_LimitPrice: r.base.price_type = DZ_PRICE_LIMIT; break;
        default:                        r.base.price_type = DZ_PRICE_LIMIT; break;
    }

    r.base.status = STATUS_CTP2VT(o.OrderStatus);
    r.base.price = o.LimitPrice;
    r.base.volume = o.VolumeTotalOriginal;
    r.base.volume_traded = o.VolumeTraded;
    r.base.date = trading_day;
    r.base.time = parse_ctp_time(o.InsertTime);

    // ---- CTP 扩展字段 ----
    copy_to_dz(r.order_ref, o.OrderRef);
    copy_to_dz(r.external_order_id, o.OrderSysID);
    r.is_external = 0;          // 留 0, AccountSession 后填
    r.volume_canceled = 0;      // 留 0, AccountSession 在 on_rtn_order 中推导
    r.error_id = 0;             // OnRtnOrder 不带 ErrorID, 留 0; 拒单场景由 OnErrRtnOrderInsert 填

    // trading_day[9]: "YYYYMMDD" 文本 (SQL 列, 从 DzDate 距纪元天数转换)
    // Date 的 year/month/day 均为 constexpr noexcept, 安全用于 noexcept 函数
    {
        dztrader::Date d{trading_day};
        auto* end = std::format_to_n(r.trading_day, sizeof(r.trading_day) - 1,
                                     "{:04d}{:02d}{:02d}",
                                     d.year(), d.month(), d.day()).out;
        *end = '\0';
    }

    // insert_time / update_time: epoch seconds = 日期 * 86400 + HH:MM:SS 秒数
    // 优先用 CTP InsertDate (夜盘场景下 InsertDate 与 trading_day 可能不同),
    // 失败回退到 trading_day 参数
    int32_t insert_date = parse_ctp_date(o.InsertDate);
    int32_t day_date = (insert_date >= 0) ? insert_date : trading_day;
    int64_t day_secs = static_cast<int64_t>(day_date) * 86400;
    int32_t insert_secs = parse_ctp_time(o.InsertTime);
    int32_t update_secs = parse_ctp_time(o.UpdateTime);
    r.insert_time = (insert_secs >= 0) ? day_secs + insert_secs : 0;
    // UpdateTime 没有对应 UpdateDate, 用 trading_day 作为日期部分
    int64_t trade_day_secs = static_cast<int64_t>(trading_day) * 86400;
    r.update_time = (update_secs >= 0) ? trade_day_secs + update_secs : 0;

    return r;
}

// ============================================================================
// to_trade_record: CTP TradeField -> TradeRecord
// ============================================================================

TradeRecord to_trade_record(const CThostFtdcTradeField& t,
                             const std::string& account_id,
                             int32_t trading_day) noexcept {
    TradeRecord r{};

    // ---- base (DzTradeReport) ----
    r.base.order_id = 0;  // 留 0, AccountSession 后填
    copy_to_dz(r.base.instrument_id, t.InstrumentID);
    copy_to_dz(r.base.account_id, account_id.c_str());
    copy_to_dz(r.base.exchange_id, t.ExchangeID);
    copy_to_dz(r.base.trade_id, t.TradeID);

    r.base.direction = (t.Direction == THOST_FTDC_D_Buy) ? DZ_DIRECTION_LONG : DZ_DIRECTION_SHORT;
    r.base.position_effect = DZ_POSITION_EFFECT_OPEN;  // 兜底
    switch (t.OffsetFlag) {
        case THOST_FTDC_OF_Open:           r.base.position_effect = DZ_POSITION_EFFECT_OPEN;           break;
        case THOST_FTDC_OF_Close:          r.base.position_effect = DZ_POSITION_EFFECT_CLOSE;          break;
        case THOST_FTDC_OF_CloseToday:     r.base.position_effect = DZ_POSITION_EFFECT_CLOSE_TODAY;    break;
        case THOST_FTDC_OF_CloseYesterday: r.base.position_effect = DZ_POSITION_EFFECT_CLOSE_YESTDAY;  break;
        default:                           break;
    }

    r.base.price = t.Price;
    r.base.volume = t.Volume;
    r.base.date = trading_day;
    r.base.time = parse_ctp_time(t.TradeTime);

    // ---- CTP 扩展字段 ----
    // trading_day[9]: "YYYYMMDD" 文本 (SQL 列, 从 DzDate 距纪元天数转换)
    {
        dztrader::Date d{trading_day};
        auto* end = std::format_to_n(r.trading_day, sizeof(r.trading_day) - 1,
                                     "{:04d}{:02d}{:02d}",
                                     d.year(), d.month(), d.day()).out;
        *end = '\0';
    }

    // trade_date: YYYYMMDD as int (优先用 CTP TradeDate, 失败从 trading_day 重组)
    int32_t ctp_trade_date = parse_ctp_date(t.TradeDate);
    if (ctp_trade_date >= 0) {
        // CTP TradeDate "YYYYMMDD" -> int64_t
        try {
            r.trade_date = std::stoll(t.TradeDate);
        } catch (...) {
            dztrader::Date d{trading_day};
            r.trade_date = static_cast<int64_t>(d.year()) * 10000
                         + static_cast<int64_t>(d.month()) * 100
                         + static_cast<int64_t>(d.day());
        }
    } else {
        dztrader::Date d{trading_day};
        r.trade_date = static_cast<int64_t>(d.year()) * 10000
                     + static_cast<int64_t>(d.month()) * 100
                     + static_cast<int64_t>(d.day());
    }

    // trade_time: epoch seconds (优先用 CTP TradeDate, 失败回退 trading_day)
    int32_t trade_date_days = (ctp_trade_date >= 0) ? ctp_trade_date : trading_day;
    int32_t trade_secs = parse_ctp_time(t.TradeTime);
    int64_t trade_day_secs = static_cast<int64_t>(trade_date_days) * 86400;
    r.trade_time = (trade_secs >= 0) ? trade_day_secs + trade_secs : 0;

    r.commission = 0.0;  // 留 0, AccountSession 后续查询 CommissionRate 后填

    return r;
}

// ============================================================================
// to_dz_instrument: CTP InstrumentField -> DzInstrumentInfo
// ============================================================================

DzInstrumentInfo to_dz_instrument(const CThostFtdcInstrumentField& f) noexcept {
    DzInstrumentInfo c{};

    copy_to_dz(c.instrument_id, f.InstrumentID);
    copy_to_dz(c.exchange_id, f.ExchangeID);
    // CTP InstrumentName 为 GBK, 此处原样拷贝 (UTF-8 转换由调用方处理)
    copy_to_dz(c.name, f.InstrumentName);

    // 产品类型: CTP ProductClass -> DZ product (int8_t 标记)
    // 'F'=期货, 'O'=期权, 'S'=组合 (与 DzInstrumentInfo 注释一致)
    switch (f.ProductClass) {
        case THOST_FTDC_PC_Futures:     c.product = 'F'; break;
        case THOST_FTDC_PC_Options:     c.product = 'O'; break;
        case THOST_FTDC_PC_Combination: c.product = 'S'; break;
        default:                        c.product = 'F'; break;  // 兜底
    }

    c.volume_multiple = f.VolumeMultiple;
    c.price_tick = f.PriceTick;
    c.min_order_volume = f.MinLimitOrderVolume;
    c.max_order_volume = f.MaxLimitOrderVolume;

    // 期权字段
    switch (f.OptionsType) {
        case THOST_FTDC_CP_CallOptions: c.option_type = DZ_OPTION_CALL; break;  // 1
        case THOST_FTDC_CP_PutOptions:  c.option_type = DZ_OPTION_PUT;  break;  // -1
        default:                        c.option_type = 0; break;  // 非期权
    }
    c.option_strike = f.StrikePrice;
    copy_to_dz(c.option_underlying, f.UnderlyingInstrID);

    // CTP 无 ListedDate, 用 OpenDate (上市日) 映射到 option_listed
    c.option_listed = parse_ctp_date(f.OpenDate);
    c.option_expiry = parse_ctp_date(f.ExpireDate);

    return c;
}

// ============================================================================
// to_dz_trading_account: CTP TradingAccountField -> DzTradingAccount
// ============================================================================

DzTradingAccount to_dz_trading_account(const CThostFtdcTradingAccountField& a,
                                        const std::string& account_id,
                                        int32_t trading_day) noexcept {
    DzTradingAccount r{};

    copy_to_dz(r.account_id, account_id.c_str());
    r.balance = a.Balance;
    r.available = a.Available;
    r.frozen = a.FrozenMargin;     // CTP FrozenMargin -> DZ frozen
    r.commission = a.Commission;
    r.margin = a.CurrMargin;       // CTP CurrMargin -> DZ margin
    r.withdraw_quota = a.WithdrawQuota;
    r.deposit = a.Deposit;
    r.withdraw = a.Withdraw;
    r.date = trading_day;

    return r;
}

}  // namespace dztrader::ctp
