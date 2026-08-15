#include "td/td_offset_converter.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <spdlog/spdlog.h>

#include <dztrader/core/string_util.h>

namespace dztrader::ctp {

// ============================================================================
// Exchange 解析
// ============================================================================

Exchange parse_exchange_id(const char* id) {
    if (id == nullptr || id[0] == '\0') {
        return Exchange::Unknown;
    }
    // 国内期货交易所代码均为 4-5 字符大写
    if (std::strcmp(id, "SHFE") == 0) return Exchange::SHFE;
    if (std::strcmp(id, "INE") == 0) return Exchange::INE;
    if (std::strcmp(id, "CFFEX") == 0) return Exchange::CFFEX;
    if (std::strcmp(id, "DCE") == 0) return Exchange::DCE;
    if (std::strcmp(id, "CZCE") == 0) return Exchange::CZCE;
    if (std::strcmp(id, "GFEX") == 0) return Exchange::GFEX;
    return Exchange::Unknown;
}

// ============================================================================
// PositionHolding 实现
// ============================================================================

PositionHolding::PositionHolding(std::string instrument_id, Exchange exchange,
                                   int32_t trading_day)
    : instrument_id_(std::move(instrument_id)),
      exchange_(exchange),
      trading_day_(trading_day) {}

void PositionHolding::update_position_detail(const DzPositionDetail& detail) {
    // 按 trade_id 索引存储 (覆盖旧值, CTP 多次回调同一 trade_id 时更新)
    details_[detail.trade_id] = detail;
    recompute_totals();
}

void PositionHolding::recompute_totals() {
    long_td_ = long_yd_ = short_td_ = short_yd_ = 0;
    for (const auto& [id, d] : details_) {
        // direction: DZ_DIRECTION_LONG=1 (多头), DZ_DIRECTION_SHORT=-1 (空头)
        if (d.direction == DZ_DIRECTION_LONG) {
            if (d.open_date == trading_day_) {
                long_td_ += d.volume;
            } else {
                long_yd_ += d.volume;
            }
        } else if (d.direction == DZ_DIRECTION_SHORT) {
            if (d.open_date == trading_day_) {
                short_td_ += d.volume;
            } else {
                short_yd_ += d.volume;
            }
        }
        // direction == DZ_DIRECTION_NET (0): 不归类 (净持仓, OffsetConverter 不处理)
    }
}

void PositionHolding::update_from_trade(const DzTradeReport& trade) {
    // OPEN: 合成 PositionDetail 加入 details_, 增加今仓
    if (trade.position_effect == DZ_POSITION_EFFECT_OPEN) {
        DzPositionDetail d{};
        // I5: 改用 copy_string 统一缓冲区安全 (与 td_ctp_mapping.cpp 一致)
        copy_string(d.account_id, trade.account_id, true);
        copy_string(d.instrument_id, trade.instrument_id, true);
        copy_string(d.exchange_id, trade.exchange_id, true);
        d.direction = trade.direction;
        d.hedge_flag = 'S';  // 投机 (默认)
        d.open_date = trading_day_;  // 今仓
        copy_string(d.trade_id, trade.trade_id, true);
        d.volume = trade.volume;
        d.open_price = trade.price;
        details_[trade.trade_id] = d;
        recompute_totals();
        return;
    }

    // CLOSE/CLOSE_TODAY/CLOSE_YESTERDAY: 递减 frozen (挂起平仓单已成交)
    // 注意: 实际持仓减少由下一次 ReqQryInvestorPositionDetail 反映, 不在此扣减
    // CLOSE_YESTERDAY 递减 yd frozen, CLOSE (generic)/CLOSE_TODAY 递减 today frozen
    // (CLOSE generic 默认归 today; 非 SHFE 不区分今昨, SHFE 由 OffsetConverter 拆分)
    if (trade.position_effect == DZ_POSITION_EFFECT_CLOSE_YESTDAY) {
        // 昨仓冻结递减
        // direction 含义: 平多 (sell to close long) -> long_frozen_yd 递减
        //                 平空 (buy to close short) -> short_frozen_yd 递减
        if (trade.direction == DZ_DIRECTION_SHORT) {
            long_frozen_yd_ -= trade.volume;
            if (long_frozen_yd_ < 0) {
                SPDLOG_WARN("long_frozen_yd underflow | instrument={} trade_id={} volume={}",
                            instrument_id_, trade.trade_id, trade.volume);
                long_frozen_yd_ = 0;
            }
        } else if (trade.direction == DZ_DIRECTION_LONG) {
            short_frozen_yd_ -= trade.volume;
            if (short_frozen_yd_ < 0) {
                SPDLOG_WARN("short_frozen_yd underflow | instrument={} trade_id={} volume={}",
                            instrument_id_, trade.trade_id, trade.volume);
                short_frozen_yd_ = 0;
            }
        }
    } else {
        // CLOSE (generic) / CLOSE_TODAY: 今仓冻结递减
        if (trade.direction == DZ_DIRECTION_SHORT) {
            long_frozen_today_ -= trade.volume;
            if (long_frozen_today_ < 0) {
                SPDLOG_WARN("long_frozen_today underflow | instrument={} trade_id={} volume={}",
                            instrument_id_, trade.trade_id, trade.volume);
                long_frozen_today_ = 0;
            }
        } else if (trade.direction == DZ_DIRECTION_LONG) {
            short_frozen_today_ -= trade.volume;
            if (short_frozen_today_ < 0) {
                SPDLOG_WARN("short_frozen_today underflow | instrument={} trade_id={} volume={}",
                            instrument_id_, trade.trade_id, trade.volume);
                short_frozen_today_ = 0;
            }
        }
    }
}

void PositionHolding::update_order_frozen(const DzOrderReq& req, bool is_sent) {
    // 仅 CLOSE 类订单影响 frozen
    if (req.position_effect != DZ_POSITION_EFFECT_CLOSE &&
        req.position_effect != DZ_POSITION_EFFECT_CLOSE_TODAY &&
        req.position_effect != DZ_POSITION_EFFECT_CLOSE_YESTDAY) {
        return;  // OPEN 不冻结
    }

    int64_t delta = is_sent ? req.volume : -req.volume;

    // CLOSE_YESTERDAY 影响昨仓冻结, CLOSE (generic)/CLOSE_TODAY 影响今仓冻结
    // (CLOSE generic 默认归 today; 非 SHFE 不区分今昨, SHFE 由 OffsetConverter 拆分)
    // direction 含义: 平多 (sell to close long) -> long_frozen += volume
    //                 平空 (buy to close short) -> short_frozen += volume
    if (req.position_effect == DZ_POSITION_EFFECT_CLOSE_YESTDAY) {
        if (req.direction == DZ_DIRECTION_SHORT) {
            long_frozen_yd_ += delta;
            if (long_frozen_yd_ < 0) {
                SPDLOG_WARN("long_frozen_yd negative on cancel | instrument={} delta={}",
                            instrument_id_, delta);
                long_frozen_yd_ = 0;
            }
        } else if (req.direction == DZ_DIRECTION_LONG) {
            short_frozen_yd_ += delta;
            if (short_frozen_yd_ < 0) {
                SPDLOG_WARN("short_frozen_yd negative on cancel | instrument={} delta={}",
                            instrument_id_, delta);
                short_frozen_yd_ = 0;
            }
        }
    } else {
        // CLOSE (generic) / CLOSE_TODAY: 今仓冻结
        if (req.direction == DZ_DIRECTION_SHORT) {
            long_frozen_today_ += delta;
            if (long_frozen_today_ < 0) {
                SPDLOG_WARN("long_frozen_today negative on cancel | instrument={} delta={}",
                            instrument_id_, delta);
                long_frozen_today_ = 0;
            }
        } else if (req.direction == DZ_DIRECTION_LONG) {
            short_frozen_today_ += delta;
            if (short_frozen_today_ < 0) {
                SPDLOG_WARN("short_frozen_today negative on cancel | instrument={} delta={}",
                            instrument_id_, delta);
                short_frozen_today_ = 0;
            }
        }
    }
}

void PositionHolding::clear() {
    details_.clear();
    long_td_ = long_yd_ = short_td_ = short_yd_ = 0;
    long_frozen_today_ = long_frozen_yd_ = 0;
    short_frozen_today_ = short_frozen_yd_ = 0;
}

// ============================================================================
// OffsetConverter 实现
// ============================================================================

std::vector<OrderSubRequest> OffsetConverter::convert_order_request(
    const DzOrderReq& req, const PositionHolding& holding, OffsetConvertMode mode) {
    std::vector<OrderSubRequest> result;

    switch (mode) {
        case OffsetConvertMode::None:
        case OffsetConvertMode::Net: {
            // 直接透传
            result.push_back({req.direction, req.position_effect, req.volume, req.price});
            return result;
        }

        case OffsetConvertMode::Lock: {
            // Close 类转为 Open (反向开仓), Open 类透传
            if (req.position_effect == DZ_POSITION_EFFECT_CLOSE ||
                req.position_effect == DZ_POSITION_EFFECT_CLOSE_TODAY ||
                req.position_effect == DZ_POSITION_EFFECT_CLOSE_YESTDAY) {
                result.push_back({req.direction, DZ_POSITION_EFFECT_OPEN, req.volume, req.price});
            } else {
                result.push_back({req.direction, req.position_effect, req.volume, req.price});
            }
            return result;
        }

        case OffsetConvertMode::Shfe: {
            // OPEN/CLOSE_TODAY/CLOSE_YESTERDAY 不拆分, 直接透传
            if (req.position_effect == DZ_POSITION_EFFECT_OPEN ||
                req.position_effect == DZ_POSITION_EFFECT_CLOSE_TODAY ||
                req.position_effect == DZ_POSITION_EFFECT_CLOSE_YESTDAY) {
                result.push_back({req.direction, req.position_effect, req.volume, req.price});
                return result;
            }

            // CLOSE 拆分: 先平今 (减 frozen), 再平昨
            // direction=SHORT (卖出平仓) -> 平多头持仓
            // direction=LONG (买入平仓) -> 平空头持仓
            int64_t need = req.volume;
            int64_t today_avail = 0;
            int64_t yd_avail = 0;

            if (req.direction == DZ_DIRECTION_SHORT) {
                // 平多头: long_today - long_frozen_today, long_yesterday - long_frozen_yd
                // I2: 昨仓可用也需减冻结, 与今仓保持一致
                today_avail = holding.long_available_today();
                yd_avail = holding.long_available_yesterday();
            } else if (req.direction == DZ_DIRECTION_LONG) {
                // 平空头: short_today - short_frozen_today, short_yesterday - short_frozen_yd
                // I2: 昨仓可用也需减冻结, 与今仓保持一致
                today_avail = holding.short_available_today();
                yd_avail = holding.short_available_yesterday();
            } else {
                // NET 方向不处理 (理论不会到此)
                return result;
            }

            // 平今
            if (today_avail > 0 && need > 0) {
                int64_t today_vol = std::min(need, today_avail);
                result.push_back({req.direction, DZ_POSITION_EFFECT_CLOSE_TODAY,
                                  static_cast<DzVolume>(today_vol), req.price});
                need -= today_vol;
            }

            // 平昨
            if (yd_avail > 0 && need > 0) {
                int64_t yd_vol = std::min(need, yd_avail);
                result.push_back({req.direction, DZ_POSITION_EFFECT_CLOSE_YESTDAY,
                                  static_cast<DzVolume>(yd_vol), req.price});
                need -= yd_vol;
            }

            // need > 0 表示持仓不足, 返回部分子订单 (调用方应记录并决策)
            return result;
        }
    }

    // 兜底 (不应到达)
    return result;
}

}  // namespace dztrader::ctp
