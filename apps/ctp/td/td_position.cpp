#include "td/td_position.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <spdlog/spdlog.h>

namespace dztrader::ctp {
namespace {

bool same_side(const PositionSide& a, const PositionSide& b) noexcept {
    return a.today == b.today && a.yd == b.yd && a.price == b.price &&
           a.frozen_td == b.frozen_td && a.frozen_yd == b.frozen_yd;
}

void add_open(PositionSide& s, const DzTradeReport& t) {
    int64_t old = s.volume();
    if (old <= 0 || s.price <= 0.0) {
        s.price = t.price;
    } else {
        s.price = (s.price * static_cast<double>(old) + t.price * static_cast<double>(t.volume)) /
                  static_cast<double>(old + t.volume);
    }
    s.today += t.volume;
}

void close_side(PositionSide& s, const DzTradeReport& t, Exchange ex,
                const std::string& instrument_id) {
    if (t.position_effect == DZ_POSITION_EFFECT_CLOSE_TODAY) {
        s.today -= t.volume;
    } else if (t.position_effect == DZ_POSITION_EFFECT_CLOSE_YESTDAY) {
        s.yd -= t.volume;
    } else if (ex == Exchange::SHFE || ex == Exchange::INE) {
        s.yd -= t.volume;  // SHFE/INE generic CLOSE 平昨
    } else {
        s.today -= t.volume;
        if (s.today < 0) {  // 先今后昨溢出
            s.yd += s.today;
            s.today = 0;
        }
    }
    if (s.today < 0 || s.yd < 0) {
        SPDLOG_WARN("position negative clamped | instrument={} today={} yd={}",
                    instrument_id, s.today, s.yd);
        s.today = std::max<int64_t>(0, s.today);
        s.yd = std::max<int64_t>(0, s.yd);
    }
    if (s.volume() == 0) {
        s.price = 0.0;
    }
}

}  // namespace

Exchange parse_exchange_id(const char* id) noexcept {
    if (id == nullptr) return Exchange::Unknown;
    if (std::strcmp(id, "SHFE") == 0) return Exchange::SHFE;
    if (std::strcmp(id, "INE") == 0) return Exchange::INE;
    if (std::strcmp(id, "CFFEX") == 0) return Exchange::CFFEX;
    if (std::strcmp(id, "DCE") == 0) return Exchange::DCE;
    if (std::strcmp(id, "CZCE") == 0) return Exchange::CZCE;
    if (std::strcmp(id, "GFEX") == 0) return Exchange::GFEX;
    return Exchange::Unknown;
}

bool is_terminal_order_status(DzOrderStatus status) noexcept {
    return status == DZ_ORDER_ALL_TRADED || status == DZ_ORDER_CANCELLED ||
           status == DZ_ORDER_REJECTED;
}

PositionHolding::PositionHolding(std::string instrument_id, std::string exchange_id)
    : instrument_id_(std::move(instrument_id)),
      exchange_id_(std::move(exchange_id)),
      exchange_(parse_exchange_id(exchange_id_.c_str())) {}

PositionSide& PositionHolding::mutable_side(DzDirection dir) noexcept {
    return dir == DZ_DIRECTION_SHORT ? short_ : long_;
}

const PositionSide& PositionHolding::side(DzDirection dir) const noexcept {
    return dir == DZ_DIRECTION_SHORT ? short_ : long_;
}

void PositionHolding::set_seq(DzDirection dir, uint64_t seq) noexcept {
    mutable_side(dir).seq = seq;
}

SideChange PositionHolding::apply_query_side(DzDirection dir, int64_t volume, int64_t yd,
                                             double price) {
    if (dir != DZ_DIRECTION_LONG && dir != DZ_DIRECTION_SHORT) return {};
    const PositionSide lb = long_;
    const PositionSide sb = short_;
    PositionSide& s = mutable_side(dir);
    int64_t v = std::max<int64_t>(0, volume);
    int64_t y = std::clamp<int64_t>(yd, 0, v);
    s.today = v - y;
    s.yd = y;
    s.price = v > 0 ? price : 0.0;
    recompute_frozen();  // 查询缩量后立即夹取冻结, 避免推送 frozen>volume
    return {!same_side(lb, long_), !same_side(sb, short_)};
}

SideChange PositionHolding::apply_trade(const DzTradeReport& trade) {
    if (trade.volume <= 0) return {};
    const PositionSide lb = long_;
    const PositionSide sb = short_;
    if (trade.direction == DZ_DIRECTION_LONG) {
        if (trade.position_effect == DZ_POSITION_EFFECT_OPEN) {
            add_open(long_, trade);
        } else {
            close_side(short_, trade, exchange_, instrument_id_);
        }
    } else if (trade.direction == DZ_DIRECTION_SHORT) {
        if (trade.position_effect == DZ_POSITION_EFFECT_OPEN) {
            add_open(short_, trade);
        } else {
            close_side(long_, trade, exchange_, instrument_id_);
        }
    } else {
        return {};
    }
    recompute_frozen();  // 平仓成交先于委托回报到达时立即夹取, 防止 frozen>volume
    return {!same_side(lb, long_), !same_side(sb, short_)};
}

SideChange PositionHolding::apply_order(const ActiveOrderUpdate& order) {
    if (order.order_ref.empty() || order.instrument_id != instrument_id_) return {};
    if (order.direction != DZ_DIRECTION_LONG && order.direction != DZ_DIRECTION_SHORT) return {};
    const PositionSide lb = long_;
    const PositionSide sb = short_;
    if (is_terminal_order_status(order.status) ||
        order.effect == DZ_POSITION_EFFECT_OPEN) {
        active_orders_.erase(order.order_ref);
    } else {
        active_orders_[order.order_ref] =
            ActiveOrder{order.direction, order.effect, order.volume, order.volume_traded};
    }
    recompute_frozen();
    return {!same_side(lb, long_), !same_side(sb, short_)};
}

SideChange PositionHolding::rebuild_active_orders(const std::vector<ActiveOrderUpdate>& orders) {
    const PositionSide lb = long_;
    const PositionSide sb = short_;
    active_orders_.clear();
    for (const auto& o : orders) {
        if (o.order_ref.empty() || o.instrument_id != instrument_id_) continue;
        if (o.direction != DZ_DIRECTION_LONG && o.direction != DZ_DIRECTION_SHORT) continue;
        if (is_terminal_order_status(o.status) ||
            o.effect == DZ_POSITION_EFFECT_OPEN) {
            continue;
        }
        active_orders_[o.order_ref] =
            ActiveOrder{o.direction, o.effect, o.volume, o.volume_traded};
    }
    recompute_frozen();
    return {!same_side(lb, long_), !same_side(sb, short_)};
}

void PositionHolding::recompute_frozen() noexcept {
    long_.frozen_td = long_.frozen_yd = 0;
    short_.frozen_td = short_.frozen_yd = 0;
    int64_t long_generic = 0;
    int64_t short_generic = 0;
    for (const auto& [ref, o] : active_orders_) {
        int64_t remaining = o.volume - o.traded;
        if (remaining <= 0) continue;
        const bool closes_long = (o.direction == DZ_DIRECTION_SHORT);
        PositionSide& s = closes_long ? long_ : short_;
        if (o.effect == DZ_POSITION_EFFECT_CLOSE_TODAY) {
            s.frozen_td += remaining;
        } else if (o.effect == DZ_POSITION_EFFECT_CLOSE_YESTDAY) {
            s.frozen_yd += remaining;
        } else if (closes_long) {
            long_generic += remaining;
        } else {
            short_generic += remaining;
        }
    }
    auto spill_and_clamp = [](PositionSide& s, int64_t generic) {
        int64_t room_td = std::max<int64_t>(0, s.today - s.frozen_td);
        int64_t take_td = std::min(generic, room_td);
        s.frozen_td += take_td;
        int64_t rest = generic - take_td;
        if (rest > 0) {
            int64_t room_yd = std::max<int64_t>(0, s.yd - s.frozen_yd);
            s.frozen_yd += std::min(rest, room_yd);
        }
        s.frozen_td = std::min(s.frozen_td, std::max<int64_t>(0, s.today));
        s.frozen_yd = std::min(s.frozen_yd, std::max<int64_t>(0, s.yd));
    };
    spill_and_clamp(long_, long_generic);
    spill_and_clamp(short_, short_generic);
}

SideChange PositionHolding::on_day_switch() noexcept {
    const PositionSide lb = long_;
    const PositionSide sb = short_;
    long_.yd += long_.today;
    long_.today = 0;
    short_.yd += short_.today;
    short_.today = 0;
    long_.frozen_td = long_.frozen_yd = 0;
    short_.frozen_td = short_.frozen_yd = 0;
    active_orders_.clear();
    return {!same_side(lb, long_), !same_side(sb, short_)};
}

void PositionHolding::clear() noexcept {
    long_ = PositionSide{};
    short_ = PositionSide{};
    active_orders_.clear();
}

int64_t PositionHolding::long_available_today() const noexcept { return long_.today - long_.frozen_td; }
int64_t PositionHolding::short_available_today() const noexcept { return short_.today - short_.frozen_td; }
int64_t PositionHolding::long_available_yesterday() const noexcept { return long_.yd - long_.frozen_yd; }
int64_t PositionHolding::short_available_yesterday() const noexcept { return short_.yd - short_.frozen_yd; }
int64_t PositionHolding::long_today() const noexcept { return long_.today; }
int64_t PositionHolding::long_yesterday() const noexcept { return long_.yd; }
int64_t PositionHolding::short_today() const noexcept { return short_.today; }
int64_t PositionHolding::short_yesterday() const noexcept { return short_.yd; }
int64_t PositionHolding::long_frozen() const noexcept { return long_.frozen(); }
int64_t PositionHolding::short_frozen() const noexcept { return short_.frozen(); }
int64_t PositionHolding::long_frozen_today() const noexcept { return long_.frozen_td; }
int64_t PositionHolding::long_frozen_yesterday() const noexcept { return long_.frozen_yd; }
int64_t PositionHolding::short_frozen_today() const noexcept { return short_.frozen_td; }
int64_t PositionHolding::short_frozen_yesterday() const noexcept { return short_.frozen_yd; }

}  // namespace dztrader::ctp
