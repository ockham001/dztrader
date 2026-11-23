#include "td/td_query_lane.h"

#include <algorithm>

namespace dztrader::ctp {

QueryLane::QueryLane(QueryPacer& pacer, int max_retries) noexcept
    : pacer_(pacer),
      max_retries_(std::max(max_retries, 1)) {}

void QueryLane::mark_dirty(QueryKind kind) noexcept {
    const std::size_t k = query_kind_index(kind);
    ++dirty_seq_[k];
    // 新的业务变化重新武装该种类：清耗尽/退避，允许下一轮重新尝试。
    exhausted_[k] = false;
    attempts_[k] = 0;
    retry_pending_[k] = false;
    has_retry_at_[k] = false;
}

bool QueryLane::pending(QueryKind kind) const noexcept {
    const std::size_t k = query_kind_index(kind);
    // 失败重试与「发送后新置脏」都算待查：前者覆盖的事件没有拿到数据，必须再查一次。
    return retry_pending_[k] || dirty_seq_[k] > covered_seq_[k];
}

bool QueryLane::exhausted(QueryKind kind) const noexcept {
    return exhausted_[query_kind_index(kind)];
}

void QueryLane::clear_exhausted(QueryKind kind) noexcept {
    const std::size_t k = query_kind_index(kind);
    exhausted_[k] = false;
    attempts_[k] = 0;
    has_retry_at_[k] = false;
}

bool QueryLane::retry_ready(QueryKind kind, TimePoint now) const noexcept {
    const std::size_t k = query_kind_index(kind);
    if (!has_retry_at_[k]) {
        return true;
    }
    return now >= retry_at_[k];
}

std::optional<QueryKind> QueryLane::take_ready(TimePoint now) noexcept {
    if (in_flight_.has_value() || !pacer_.can_issue(now)) {
        return std::nullopt;
    }
    for (std::size_t k = 0; k < kQueryKindCount; ++k) {
        const auto kind = static_cast<QueryKind>(k);
        if (!pending(kind) || exhausted_[k] || !retry_ready(kind, now)) {
            continue;
        }
        return kind;
    }
    return std::nullopt;
}

std::optional<QueryLane::TimePoint> QueryLane::next_wake(TimePoint now) const noexcept {
    if (in_flight_.has_value() || pacer_.in_flight()) {
        return std::nullopt;  // 等响应收口；收口后调用方重新询问
    }
    std::optional<TimePoint> wake;
    const TimePoint eligible = pacer_.next_eligible(now);
    for (std::size_t k = 0; k < kQueryKindCount; ++k) {
        const auto kind = static_cast<QueryKind>(k);
        if (!pending(kind) || exhausted_[k]) {
            continue;
        }
        TimePoint candidate = std::max(eligible, now);
        if (has_retry_at_[k]) {
            candidate = std::max(candidate, retry_at_[k]);
        }
        wake = wake.has_value() ? std::min(*wake, candidate) : candidate;
    }
    return wake;
}

void QueryLane::note_sent(TimePoint now, QueryKind kind) noexcept {
    const std::size_t k = query_kind_index(kind);
    pacer_.note_sent(now);
    in_flight_ = kind;
    covered_seq_[k] = dirty_seq_[k];
    retry_pending_[k] = false;
    has_retry_at_[k] = false;
}

void QueryLane::note_response(TimePoint now, QueryKind kind) noexcept {
    (void)now;  // 与 note_failure 对称；成功路径不需要退避时刻。
    const std::size_t k = query_kind_index(kind);
    if (in_flight_ == kind) {
        pacer_.note_done();
        in_flight_.reset();
    }
    attempts_[k] = 0;
    retry_pending_[k] = false;
    has_retry_at_[k] = false;
    exhausted_[k] = false;
}

void QueryLane::note_failure(TimePoint now, QueryKind kind) noexcept {
    const std::size_t k = query_kind_index(kind);
    if (in_flight_ == kind) {
        pacer_.note_done();
        in_flight_.reset();
    }
    ++attempts_[k];
    if (attempts_[k] > max_retries_) {
        // 退避用尽：丢弃本次待查（下一个事件 / 手动刷新可重新置脏），标记一次性通知。
        exhausted_[k] = true;
        retry_pending_[k] = false;
        covered_seq_[k] = dirty_seq_[k];
        has_retry_at_[k] = false;
        return;
    }
    // 失败意味着这次查询没有拿到数据：本次覆盖的事件必须再查一次（不丢）。
    retry_pending_[k] = true;
    has_retry_at_[k] = true;
    retry_at_[k] = now + backoff_for(attempts_[k]);
}

QueryLane::Duration QueryLane::backoff_for(int attempts) const noexcept {
    // 第 n 次失败等待 min_gap * 2^(n-1)，封顶 5s（ADR 0055 决策 6）。
    Duration delay = pacer_.min_gap();
    for (int i = 1; i < attempts; ++i) {
        delay *= 2;
        if (delay >= kBackoffCap) {
            return kBackoffCap;
        }
    }
    return std::min(delay, kBackoffCap);
}

void QueryLane::reset() noexcept {
    pacer_.reset();
    dirty_seq_.fill(0);
    covered_seq_.fill(0);
    attempts_.fill(0);
    retry_pending_.fill(false);
    exhausted_.fill(false);
    has_retry_at_.fill(false);
    retry_at_.fill(TimePoint{});
    in_flight_.reset();
}

int QueryLane::attempts(QueryKind kind) const noexcept { return attempts_[query_kind_index(kind)]; }

}  // namespace dztrader::ctp
