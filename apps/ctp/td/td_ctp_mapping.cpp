#include "td/td_ctp_mapping.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>

#include <dztrader/core/encoding.h>     // to_utf8_from_gbk
#include <dztrader/core/string_util.h>  // copy_string
#include <dztrader/date_time/date.h>    // Date

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

    int32_t y = (yyyymmdd[0] - '0') * 1000 + (yyyymmdd[1] - '0') * 100 + (yyyymmdd[2] - '0') * 10 +
                (yyyymmdd[3] - '0');
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

DzOrderStatus STATUS_CTP2VT(char ctp_status, UnknownEnumSink* unknown) noexcept {
    switch (ctp_status) {
        case THOST_FTDC_OST_AllTraded:
            return DZ_ORDER_ALL_TRADED;  // '0'
        case THOST_FTDC_OST_PartTradedQueueing:
            return DZ_ORDER_PART_TRADED;  // '1'
        case THOST_FTDC_OST_PartTradedNotQueueing:
            return DZ_ORDER_PART_TRADED;  // '2'
        case THOST_FTDC_OST_NoTradeQueueing:
            return DZ_ORDER_NOT_TRADED;  // '3'
        case THOST_FTDC_OST_NoTradeNotQueueing:
            return DZ_ORDER_CANCELLED;  // '4'
        case THOST_FTDC_OST_Canceled:
            return DZ_ORDER_CANCELLED;  // '5'
        case THOST_FTDC_OST_NotTouched:
            return DZ_ORDER_NOT_TRADED;  // 'b'
        case THOST_FTDC_OST_Touched:
            return DZ_ORDER_PART_TRADED;  // 'c'
        case THOST_FTDC_OST_Unknown:
            return DZ_ORDER_SUBMITTING;  // 'a' 柜台自报「未知」, 语义即等待下次回报
        default:
            // 编译期防线对本函数无效 (CTP 侧是 char 字面量, -Wswitch 永不生效),
            // 只能运行期暴露: 返回 UNKNOWN 交给调用方上报, 绝不猜成某个真实状态.
            if (unknown != nullptr) {
                unknown->add(kUnknownCategoryOrderStatus, ctp_status);
            }
            return DZ_ORDER_UNKNOWN;
    }
}

// ============================================================================
// ORDERTYPE_VT2CTP: DzPriceType -> CTP 价格类型三元组
// ============================================================================

std::optional<OrderTypeTriplet> ORDERTYPE_VT2CTP(DzPriceType t) noexcept {
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
            // STOP/RFQ 及任何未知类型: 期货不支持 -> 失败, 由调用方本地拒单.
            // 兜底成限价会把止损单当普通限价单发到交易所 (最严重的一类错误).
            return std::nullopt;
    }
    return r;
}

// ============================================================================
// to_input_order_field: DzOrderReq + ctx -> CThostFtdcInputOrderField
// ============================================================================

std::optional<CThostFtdcInputOrderField> to_input_order_field(
    const DzOrderReq& req, const OrderBuildContext& ctx) noexcept {
    CThostFtdcInputOrderField f{};

    // 标识字段
    copy_to_dz(f.InvestorID, ctx.account_id.c_str());
    copy_to_dz(f.InstrumentID, req.instrument_id);
    copy_to_dz(f.ExchangeID, ctx.exchange_id.c_str());

    // OrderRef: 12 位补零 (CTP TThostFtdcOrderRefType[13], 末位留给 null)
    std::snprintf(f.OrderRef, sizeof(f.OrderRef), "%012lld", static_cast<long long>(ctx.order_ref));

    f.RequestID = ctx.request_id;

    // 买卖方向: DZ LONG(1) -> CTP Buy('0'), DZ SHORT(-1) -> CTP Sell('1').
    // 其余取值 (含 NET/UNKNOWN) 返回失败: 报单方向只有买/卖两种, 兜成 Sell 会把一张
    // 「方向读不懂」的单当成卖单发给交易所. 由调用方本地拒单.
    switch (req.direction) {
        case DZ_DIRECTION_LONG:
            f.Direction = THOST_FTDC_D_Buy;
            break;
        case DZ_DIRECTION_SHORT:
            f.Direction = THOST_FTDC_D_Sell;
            break;
        default:
            return std::nullopt;
    }

    // 开平标志: CombOffsetFlag[0]. 未知开平不得兜成 Open —— 那会把平仓单变成开仓单,
    // 凭空放大敞口; 返回失败由调用方本地拒单.
    switch (req.position_effect) {
        case DZ_POSITION_EFFECT_OPEN:
            f.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
            break;
        case DZ_POSITION_EFFECT_CLOSE:
            f.CombOffsetFlag[0] = THOST_FTDC_OF_Close;
            break;
        case DZ_POSITION_EFFECT_CLOSE_TODAY:
            f.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;
            break;
        case DZ_POSITION_EFFECT_CLOSE_YESTDAY:
            f.CombOffsetFlag[0] = THOST_FTDC_OF_CloseYesterday;
            break;
        default:
            return std::nullopt;
    }

    // 价格类型三元组 (不支持的类型同样返回失败)
    const auto triplet = ORDERTYPE_VT2CTP(req.price_type);
    if (!triplet.has_value()) {
        return std::nullopt;
    }
    f.OrderPriceType = triplet->limit_price_type;
    f.TimeCondition = triplet->time_condition;
    f.VolumeCondition = triplet->volume_condition;

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
                            int32_t trading_day,
                            UnknownEnumSink* unknown) noexcept {
    OrderRecord r{};

    // ---- base (DzOrderReport) ----
    r.base.order_id = 0;  // 留 0, AccountSession 查 OrderRefMap 后填
    // strategy_id 留空, AccountSession 后填
    copy_to_dz(r.base.instrument_id, o.InstrumentID);
    copy_to_dz(r.base.account_id, account_id.c_str());
    copy_to_dz(r.base.exchange_id, o.ExchangeID);

    // 买卖方向: 未知方向不得猜成 SHORT (那会把一张卖单谎报成买单, 净敞口符号反向).
    // 编译期防线对本函数无效 (CTP 侧是 char 字面量, -Wswitch 永不生效), 只能运行期暴露.
    switch (o.Direction) {
        case THOST_FTDC_D_Buy:
            r.base.direction = DZ_DIRECTION_LONG;
            break;
        case THOST_FTDC_D_Sell:
            r.base.direction = DZ_DIRECTION_SHORT;
            break;
        default:
            if (unknown != nullptr) {
                unknown->add(kUnknownCategoryDirection, o.Direction);
            }
            r.base.direction = DZ_DIRECTION_UNKNOWN;
            break;
    }

    // 开平标志 (CombOffsetFlag[0]). 未知值不得兜成 OPEN (平仓成交记成开仓会放大敞口).
    switch (o.CombOffsetFlag[0]) {
        case THOST_FTDC_OF_Open:
            r.base.position_effect = DZ_POSITION_EFFECT_OPEN;
            break;
        case THOST_FTDC_OF_Close:
            r.base.position_effect = DZ_POSITION_EFFECT_CLOSE;
            break;
        case THOST_FTDC_OF_CloseToday:
            r.base.position_effect = DZ_POSITION_EFFECT_CLOSE_TODAY;
            break;
        case THOST_FTDC_OF_CloseYesterday:
            r.base.position_effect = DZ_POSITION_EFFECT_CLOSE_YESTDAY;
            break;
        default:
            if (unknown != nullptr) {
                unknown->add(kUnknownCategoryPositionEffect, o.CombOffsetFlag[0]);
            }
            r.base.position_effect = DZ_POSITION_EFFECT_UNKNOWN;
            break;
    }

    // 价格类型: 由 OrderPriceType 反推 (FAK/FOK 在 OrderField 中无独立标志, 一律归为 LIMIT)
    switch (o.OrderPriceType) {
        case THOST_FTDC_OPT_AnyPrice:
            r.base.price_type = DZ_PRICE_MARKET;
            break;
        case THOST_FTDC_OPT_LimitPrice:
            r.base.price_type = DZ_PRICE_LIMIT;
            break;
        default:
            if (unknown != nullptr) {
                unknown->add(kUnknownCategoryPriceType, o.OrderPriceType);
            }
            r.base.price_type = DZ_PRICE_UNKNOWN;
            break;
    }

    r.base.status = STATUS_CTP2VT(o.OrderStatus, unknown);
    r.base.price = o.LimitPrice;
    r.base.volume = o.VolumeTotalOriginal;
    r.base.volume_traded = o.VolumeTraded;
    r.base.date = trading_day;
    r.base.time = parse_ctp_time(o.InsertTime);

    // ---- CTP 扩展字段 ----
    copy_to_dz(r.order_ref, o.OrderRef);
    copy_to_dz(r.external_order_id, o.OrderSysID);
    r.is_external = 0;      // 留 0, AccountSession 后填
    r.volume_canceled = 0;  // 留 0, AccountSession 在 on_rtn_order 中推导
    r.error_id = 0;         // OnRtnOrder 不带 ErrorID, 留 0; 拒单场景由 OnErrRtnOrderInsert 填

    // trading_day[9]: "YYYYMMDD" 文本 (SQL 列, 从 DzDate 距纪元天数转换)
    // Date 的 year/month/day 均为 constexpr noexcept, 安全用于 noexcept 函数
    {
        dztrader::Date d{trading_day};
        auto* end = std::format_to_n(r.trading_day, sizeof(r.trading_day) - 1, "{:04d}{:02d}{:02d}",
                                     d.year(), d.month(), d.day())
                        .out;
        *end = '\0';
    }

    // insert_time / update_time: epoch seconds = 交易日 * 86400 + HH:MM:SS 秒数
    // 日期部分一律取会话交易日 (trading_day 参数), 不用 CTP 自带的自然日字段:
    // 夜盘 (21:00 起) 的 InsertDate 是自然日 D, 但整笔委托归属交易日 D+1, 用 InsertDate
    // 会让同一行的 insert_time 与 update_time 分属两天. 会话交易日只有登录响应一个来源
    // (见 AccountSession::on_rsp_user_login), 这里与 base.date / trading_day[9] 同源.
    int64_t day_secs = static_cast<int64_t>(trading_day) * 86400;
    int32_t insert_secs = parse_ctp_time(o.InsertTime);
    int32_t update_secs = parse_ctp_time(o.UpdateTime);
    r.insert_time = (insert_secs >= 0) ? day_secs + insert_secs : 0;
    r.update_time = (update_secs >= 0) ? day_secs + update_secs : 0;

    return r;
}

// ============================================================================
// to_trade_record: CTP TradeField -> TradeRecord
// ============================================================================

TradeRecord to_trade_record(const CThostFtdcTradeField& t,
                            const std::string& account_id,
                            int32_t trading_day,
                            UnknownEnumSink* unknown) noexcept {
    TradeRecord r{};

    // ---- base (DzTradeReport) ----
    r.base.order_id = 0;  // 留 0, AccountSession 后填
    copy_to_dz(r.base.instrument_id, t.InstrumentID);
    copy_to_dz(r.base.account_id, account_id.c_str());
    copy_to_dz(r.base.exchange_id, t.ExchangeID);
    copy_to_dz(r.base.trade_id, t.TradeID);

    // 买卖方向: 未知方向不得猜成 SHORT (同 to_order_record, 净敞口符号不得反向).
    switch (t.Direction) {
        case THOST_FTDC_D_Buy:
            r.base.direction = DZ_DIRECTION_LONG;
            break;
        case THOST_FTDC_D_Sell:
            r.base.direction = DZ_DIRECTION_SHORT;
            break;
        default:
            if (unknown != nullptr) {
                unknown->add(kUnknownCategoryDirection, t.Direction);
            }
            r.base.direction = DZ_DIRECTION_UNKNOWN;
            break;
    }
    // 未知开平标志不得兜成 OPEN: 平仓成交记成开仓会让持仓凭空放大.
    switch (t.OffsetFlag) {
        case THOST_FTDC_OF_Open:
            r.base.position_effect = DZ_POSITION_EFFECT_OPEN;
            break;
        case THOST_FTDC_OF_Close:
            r.base.position_effect = DZ_POSITION_EFFECT_CLOSE;
            break;
        case THOST_FTDC_OF_CloseToday:
            r.base.position_effect = DZ_POSITION_EFFECT_CLOSE_TODAY;
            break;
        case THOST_FTDC_OF_CloseYesterday:
            r.base.position_effect = DZ_POSITION_EFFECT_CLOSE_YESTDAY;
            break;
        default:
            if (unknown != nullptr) {
                unknown->add(kUnknownCategoryPositionEffect, t.OffsetFlag);
            }
            r.base.position_effect = DZ_POSITION_EFFECT_UNKNOWN;
            break;
    }

    r.base.price = t.Price;
    r.base.volume = t.Volume;
    r.base.date = trading_day;
    r.base.time = parse_ctp_time(t.TradeTime);

    // ---- CTP 扩展字段 ----
    // trading_day[9]: "YYYYMMDD" 文本 (SQL 列, 从 DzDate 距纪元天数转换)
    {
        dztrader::Date d{trading_day};
        auto* end = std::format_to_n(r.trading_day, sizeof(r.trading_day) - 1, "{:04d}{:02d}{:02d}",
                                     d.year(), d.month(), d.day())
                        .out;
        *end = '\0';
    }

    return r;
}

// ============================================================================
// to_instrument_record: CTP InstrumentField -> tdstore::InstrumentRecord
// ============================================================================

namespace {

/// CTP OptionsType -> DZ_OPTION_* (0=非期权).
/// 这里刻意保持 `default -> 0` 而不是引入「未知」取值, 理由（#367 审查 F11 复核结论）:
/// - CTP 只定义了两个取值 (CallOptions '1' / PutOptions '2'); **非期权合约（绝大多数）**
///   的 OptionsType 本就既不是 '1' 也不是 '2', 返回 0 对它们是正确答案而不是兜底猜测.
/// - `DzOptionType` 只有 PUT(-1)/CALL(1), 0 是契约与 DB 列默认值明文的「非期权」
///   （《帧契约：合约》· 期权段）, 没有第三个合法取值可用; 新增 UNKNOWN 要同时改策略可见
///   头文件、DB 列默认值、契约与前端, 属独立票, 不在本票「4 个枚举」范围内.
/// - 信息不会丢失: 是否期权由 `product_class`（DZ_PRODUCT_OPTION）独立表达, 读端不会因为
///   option_type=0 就把一张期权当成期货.
int32_t option_type_from_ctp(TThostFtdcOptionsTypeType type) noexcept {
    switch (type) {
        case THOST_FTDC_CP_CallOptions:
            return DZ_OPTION_CALL;  // 1
        case THOST_FTDC_CP_PutOptions:
            return DZ_OPTION_PUT;  // -1
        default:
            return 0;  // 非期权
    }
}

}  // namespace

int32_t product_class_from_ctp(TThostFtdcProductClassType pc) noexcept {
    switch (pc) {
        case THOST_FTDC_PC_Futures:
            return DZ_PRODUCT_FUTURES;
        case THOST_FTDC_PC_Options:
            return DZ_PRODUCT_OPTION;
        case THOST_FTDC_PC_SpotOption:
            return DZ_PRODUCT_OPTION;
        case THOST_FTDC_PC_Combination:
            return DZ_PRODUCT_SPREAD;
        case THOST_FTDC_PC_Spot:
            return DZ_PRODUCT_SPOT;
        default:
            return DZ_PRODUCT_UNKNOWN;
    }
}

tdstore::InstrumentRecord to_instrument_record(const CThostFtdcInstrumentField& f,
                                               const std::string& update_day) noexcept {
    tdstore::InstrumentRecord r{};
    r.instrument_id = f.InstrumentID;
    r.exchange_id = f.ExchangeID;
    r.symbol = f.InstrumentID;
    r.name = dztrader::to_utf8_from_gbk(f.InstrumentName);
    r.product_class = product_class_from_ctp(f.ProductClass);
    r.product_code = f.ProductID;
    r.settle_cycle = -1;  // CTP 衍生品语义
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
    r.frozen = a.FrozenMargin;  // CTP FrozenMargin -> DZ frozen
    r.commission = a.Commission;
    r.margin = a.CurrMargin;  // CTP CurrMargin -> DZ margin
    r.withdraw_quota = a.WithdrawQuota;
    r.deposit = a.Deposit;
    r.withdraw = a.Withdraw;
    r.date = trading_day;

    return r;
}

// ============================================================================
// to_dz_position: CTP InvestorPositionField -> DzPositionInfo
// ============================================================================

std::optional<DzPositionInfo> to_dz_position(const CThostFtdcInvestorPositionField& p,
                                             const std::string& account_id,
                                             int32_t trading_day,
                                             double volume_multiple,
                                             UnknownEnumSink* unknown) noexcept {
    DzPositionInfo r{};

    copy_to_dz(r.instrument_id, p.InstrumentID);
    copy_to_dz(r.exchange_id, p.ExchangeID);
    copy_to_dz(r.account_id, account_id.c_str());

    // 持仓方向: 多('2') -> LONG, 空('3') -> SHORT.
    // 净持仓('1') 返回 nullopt: 它是 CTP 文档定义的合法枚举, 但 DZ 只有多/空两态, 且持仓查询
    // 实际按多/空返回 (ADR 0049 决策 9) —— 跳过该行 + 由会话记 WARN. 不能归为多头 (凭空造持仓),
    // 也不能走未知方向分支 (合法枚举不该让整轮快照作废 → 账户登录不上).
    // 真正读不懂的码返回 UNKNOWN —— 兜成 LONG 会把空头写成多头, 净敞口符号反向.
    switch (p.PosiDirection) {
        case THOST_FTDC_PD_Short:
            r.direction = DZ_DIRECTION_SHORT;
            break;
        case THOST_FTDC_PD_Long:
            r.direction = DZ_DIRECTION_LONG;
            break;
        case THOST_FTDC_PD_Net:
            return std::nullopt;
        default:
            if (unknown != nullptr) {
                unknown->add(kUnknownCategoryDirection, p.PosiDirection);
            }
            r.direction = DZ_DIRECTION_UNKNOWN;
            break;
    }

    // 持仓量: CTP 持仓查询响应中 Position 为该行的当前总持仓 (含今昨), 映射到 DZ 总持仓.
    // 负值是坏数据 (CTP 量字段非负), 夹到 0: 保证下面的 today + yd == volume 且两者非负.
    const int64_t volume = std::max<int64_t>(0, p.Position);
    r.volume = volume;
    // 冻结量: 多/空冻结合并 (DZ 单值)
    r.frozen_volume = static_cast<int64_t>(p.LongFrozen) + static_cast<int64_t>(p.ShortFrozen);
    // 均价: 无直接字段. PositionCost 为金额 (vnpy: cost/(volume*size)),
    // 除以总持仓与合约乘数; 持仓/乘数 <= 0 时留 0 (防除零, 缺合约表降级).
    if (p.Position > 0 && volume_multiple > 0) {
        r.price = p.PositionCost / (static_cast<double>(p.Position) * volume_multiple);
    }
    // 今昨口径 (ADR 0049 / 契约 td-data-sync §3): 今仓 = TodayPosition, 昨仓 = Position - 今仓.
    //
    // 为什么不用 YdPosition: 它是交易日开始时的**静态**昨仓, 平昨后不减少, 不是当前昨仓.
    // 反例 (知乎《CTP 程序化交易入门系列之十二: 持仓的查询与更新(上)》a2101 例):
    // Position=26 / TodayPosition=25 / YdPosition=14 —— 当前昨仓 = 26-25 = 1 手,
    // 用 YdPosition 会算成 14 手.
    //
    // 三种形态均成立 (ADR 0049 决策 4):
    // - 非 SHFE/INE 单行: Position=今+昨, TodayPosition=今 → 减出来是昨
    // - SHFE/INE 今仓行: Position=今天, TodayPosition=今天 → yd=0
    // - SHFE/INE 昨仓行: Position=剩余昨, TodayPosition=0 → today=0, yd=剩余昨
    //
    // 异常数据 (TodayPosition 越界 = 柜台坏数据): today 夹到 [0, Position], yd 由减法派生
    // 故必 >= 0. 映射层保持纯函数, 异常 WARN 由调用方 (查询聚合层) 记.
    const int64_t today_volume = std::clamp<int64_t>(p.TodayPosition, 0, volume);
    r.today_volume = today_volume;
    r.yd_volume = volume - today_volume;
    r.date = trading_day;

    return r;
}

}  // namespace dztrader::ctp
