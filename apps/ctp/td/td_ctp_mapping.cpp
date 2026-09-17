#include "td/td_ctp_mapping.h"

#include <cstdio>
#include <cstring>
#include <format>

#include <dztrader/core/encoding.h>      // to_utf8_from_gbk
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
// 哨兵: DZ_DATE_NA(0) 表示无效/未提供 (POD 零初始化即 NA, 安全缺省)。
// 注意: "19700101" 合法解析结果也是 0, 与哨兵重合 — 业务上不存在 1970 年真实交易日期,
// 该输入视同 NA 是设计决策而非缺陷 (见 struct.h DZ_DATE_NA 注释)。
// 注意 parse_ctp_time 仍以 -1 为哨兵 (0 秒是合法值), 两者语义独立, 勿统一。
// ============================================================================

int32_t parse_ctp_date(const char* yyyymmdd) noexcept {
    if (yyyymmdd == nullptr || yyyymmdd[0] == '\0') {
        return DZ_DATE_NA;
    }
    if (std::strlen(yyyymmdd) != 8) {
        return DZ_DATE_NA;
    }
    for (int i = 0; i < 8; ++i) {
        if (yyyymmdd[i] < '0' || yyyymmdd[i] > '9') {
            return DZ_DATE_NA;
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
        return DZ_DATE_NA;
    }
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
    int32_t day_date = (insert_date != DZ_DATE_NA) ? insert_date : trading_day;
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
    if (ctp_trade_date != DZ_DATE_NA) {
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
    int32_t trade_date_days = (ctp_trade_date != DZ_DATE_NA) ? ctp_trade_date : trading_day;
    int32_t trade_secs = parse_ctp_time(t.TradeTime);
    int64_t trade_day_secs = static_cast<int64_t>(trade_date_days) * 86400;
    r.trade_time = (trade_secs >= 0) ? trade_day_secs + trade_secs : 0;

    r.commission = 0.0;  // 留 0（平台不查询费率，ADR 0013）

    return r;
}

// ============================================================================
// to_instrument_record: CTP InstrumentField -> tdstore::InstrumentRecord
// ============================================================================

namespace {

/// CTP ProductClass -> DZ_PRODUCT_* (未知暴露为 UNKNOWN, 不再伪装期货).
/// 注意: 商品期权是 SpotOption('6') 而非 Options('2'); EFP('5')/TAS('7')/MI('I') 暴露为 UNKNOWN
int32_t product_class_from_ctp(TThostFtdcProductClassType pc) noexcept {
    switch (pc) {
        case THOST_FTDC_PC_Futures:     return DZ_PRODUCT_FUTURES;
        case THOST_FTDC_PC_Options:     return DZ_PRODUCT_OPTION;
        case THOST_FTDC_PC_SpotOption:  return DZ_PRODUCT_OPTION;
        case THOST_FTDC_PC_Combination: return DZ_PRODUCT_SPREAD;
        case THOST_FTDC_PC_Spot:        return DZ_PRODUCT_SPOT;
        default:                        return DZ_PRODUCT_UNKNOWN;
    }
}

/// CTP OptionsType -> DZ_OPTION_* (0=非期权)
int32_t option_type_from_ctp(TThostFtdcOptionsTypeType type) noexcept {
    switch (type) {
        case THOST_FTDC_CP_CallOptions: return DZ_OPTION_CALL;  // 1
        case THOST_FTDC_CP_PutOptions:  return DZ_OPTION_PUT;   // -1
        default:                        return 0;               // 非期权
    }
}

}  // namespace

tdstore::InstrumentRecord to_instrument_record(const CThostFtdcInstrumentField& f,
                                               const std::string& update_day) noexcept {
    tdstore::InstrumentRecord r{};
    r.instrument_id = f.InstrumentID;
    r.exchange_id = f.ExchangeID;
    r.symbol = f.InstrumentID;
    r.name = dztrader::to_utf8_from_gbk(f.InstrumentName);
    r.product_class = product_class_from_ctp(f.ProductClass);
    r.product_code = f.ProductID;
    r.settle_cycle = -1;                 // CTP 衍生品语义
    r.currency = "CNY";
    r.volume_multiple = static_cast<double>(f.VolumeMultiple);
    r.volume_step = 1.0;
    r.price_tick = f.PriceTick;
    r.min_limit_order_volume = f.MinLimitOrderVolume;
    r.max_limit_order_volume = f.MaxLimitOrderVolume;
    r.min_market_order_volume = f.MinMarketOrderVolume;
    r.max_market_order_volume = f.MaxMarketOrderVolume;
    r.listed_date = parse_ctp_date(f.OpenDate);
    // TODO(ctp-verify): 核对 CTP ExpireDate 是否等于最后交易日 (rb/IF/IO/m/SR/si + 1 期权抽查),
    // 例外写回 docs/frame_contracts/instrument.md §11 核对项。
    r.delisted_date = parse_ctp_date(f.ExpireDate);
    r.option_type = option_type_from_ctp(f.OptionsType);
    r.option_strike = f.StrikePrice;
    r.underlying_id = f.UnderlyingInstrID;
    // TODO(ctp-verify): 核对 UnderlyingMultiple 语义 (CFFEX 指数期权 + 商品期权各一例),
    // 结论回填 docs/frame_contracts/instrument.md §11 核对项。
    r.underlying_multiple = f.UnderlyingMultiple;
    r.update_day = update_day;
    // r.updated_at 由调用方置 epoch ms
    return r;
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

// ============================================================================
// to_dz_position: CTP InvestorPositionField -> DzPositionInfo
// ============================================================================

DzPositionInfo to_dz_position(const CThostFtdcInvestorPositionField& p,
                               const std::string& account_id,
                               int32_t trading_day,
                               double volume_multiple) noexcept {
    DzPositionInfo r{};

    copy_to_dz(r.instrument_id, p.InstrumentID);
    copy_to_dz(r.exchange_id, p.ExchangeID);
    copy_to_dz(r.account_id, account_id.c_str());

    // 持仓方向: CTP 净持仓('1') 归为多头 (DZ 仅两态); 多('2') -> LONG, 空('3') -> SHORT
    switch (p.PosiDirection) {
        case THOST_FTDC_PD_Short: r.direction = DZ_DIRECTION_SHORT; break;
        case THOST_FTDC_PD_Long:
        case THOST_FTDC_PD_Net:
        default:                  r.direction = DZ_DIRECTION_LONG;  break;
    }

    // 持仓量: CTP 持仓查询响应中 Position 为总持仓口径 (含今昨), 映射到 DZ 总持仓.
    r.volume = p.Position;
    // 冻结量: 多/空冻结合并 (DZ 单值)
    r.frozen_volume = static_cast<int64_t>(p.LongFrozen) + static_cast<int64_t>(p.ShortFrozen);
    // 均价: 无直接字段. PositionCost 为金额 (vnpy: cost/(volume*size)),
    // 除以总持仓与合约乘数; 持仓/乘数 <= 0 时留 0 (防除零, 缺合约表降级).
    if (p.Position > 0 && volume_multiple > 0) {
        r.price = p.PositionCost / (static_cast<double>(p.Position) * volume_multiple);
    }
    r.yd_volume = p.YdPosition;
    r.today_volume = p.Position - p.YdPosition;  // 今仓 = 总 - 昨 (CTP 无直接今仓字段在此结构)
    r.date = trading_day;

    return r;
}

}  // namespace dztrader::ctp
