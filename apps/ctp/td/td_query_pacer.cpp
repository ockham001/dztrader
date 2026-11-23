#include "td/td_query_pacer.h"

#include <algorithm>

namespace dztrader::ctp {

QueryPacer::QueryPacer(Duration min_gap) noexcept
    : min_gap_(std::max(min_gap, Duration{std::chrono::milliseconds(1)})) {}

void QueryPacer::set_min_gap(Duration min_gap) noexcept {
    min_gap_ = std::max(min_gap, Duration{std::chrono::milliseconds(1)});
}

bool QueryPacer::can_issue(TimePoint now) const noexcept {
    if (in_flight_) {
        return false;
    }
    if (!has_sent_) {
        return true;
    }
    return now - last_sent_ >= min_gap_;
}

QueryPacer::TimePoint QueryPacer::next_eligible(TimePoint now) const noexcept {
    if (in_flight_) {
        return TimePoint::max();
    }
    if (!has_sent_) {
        return now;
    }
    const TimePoint eligible = last_sent_ + min_gap_;
    return std::max(eligible, now);
}

void QueryPacer::note_sent(TimePoint now) noexcept {
    has_sent_ = true;
    in_flight_ = true;
    last_sent_ = now;
}

void QueryPacer::note_done() noexcept { in_flight_ = false; }

void QueryPacer::reset() noexcept {
    has_sent_ = false;
    in_flight_ = false;
    last_sent_ = TimePoint{};
}

}  // namespace dztrader::ctp
