#include "td/td_offset_converter.h"

#include <algorithm>

namespace dztrader::ctp {

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
