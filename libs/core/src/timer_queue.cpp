#include <dztrader/core/timer_queue.h>

namespace dztrader::core {

TimerQueue::TimerId TimerQueue::schedule_after(Duration delay, Callback cb) {
    return schedule_at(Clock::now() + delay, std::move(cb));
}

TimerQueue::TimerId TimerQueue::schedule_at(TimePoint deadline, Callback cb) {
    return schedule_at_internal(deadline, {}, std::move(cb));
}

TimerQueue::TimerId TimerQueue::schedule_after_ignored(std::string tag, Duration delay, Callback cb) {
    if (!tag.empty()) {
        auto it = tags_.find(tag);
        if (it != tags_.end()) {
            return it->second;
        }
    }
    return schedule_at_internal(Clock::now() + delay, std::move(tag), std::move(cb));
}

TimerQueue::TimerId TimerQueue::schedule_after_replace(std::string tag, Duration delay, Callback cb) {
    if (!tag.empty()) {
        auto it = tags_.find(tag);
        if (it != tags_.end()) {
            cancel(it->second);
        }
    }
    return schedule_at_internal(Clock::now() + delay, std::move(tag), std::move(cb));
}

void TimerQueue::cancel(TimerId id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return;
    }
    if (!it->second.tag.empty()) {
        tags_.erase(it->second.tag);
    }
    deadlines_.erase(it->second.deadline_it);
    entries_.erase(it);
}

void TimerQueue::tick() {
    auto now = Clock::now();
    while (!deadlines_.empty() && deadlines_.begin()->first <= now) {
        auto it = deadlines_.begin();
        TimerId id = it->second;
        deadlines_.erase(it);
        auto entry_it = entries_.find(id);
        // 防御: deadlines_ 与 entries_ 一一对应, entry_it 必命中。
        // 保留 find 保护仅为 invariant 兜底, 避免未来改动破坏对应关系时崩溃。
        if (entry_it != entries_.end()) {
            if (!entry_it->second.tag.empty()) {
                tags_.erase(entry_it->second.tag);
            }
            Callback cb = std::move(entry_it->second.callback);
            entries_.erase(entry_it);
            cb();
        }
    }
}

TimerQueue::Duration TimerQueue::next_timeout() const {
    // 前置条件: !empty()。空队列调用是 UB (对 deadlines_.begin() 解引用未定义)。
    auto remaining = deadlines_.begin()->first - Clock::now();
    return remaining < Duration::zero() ? Duration::zero() : remaining;
}

bool TimerQueue::empty() const {
    // 与 next_timeout() 共享 deadlines_, 保证 empty()==false 时 next_timeout() 可安全调用
    return deadlines_.empty();
}

TimerQueue::TimerId TimerQueue::schedule_at_internal(TimePoint deadline, std::string tag, Callback cb) {
    auto id = ++next_id_;
    auto deadline_it = deadlines_.emplace(deadline, id);
    auto& entry = entries_.emplace(id, Entry{std::move(cb), deadline_it, std::move(tag)}).first->second;
    if (!entry.tag.empty()) {
        tags_[entry.tag] = id;
    }
    return id;
}

}  // namespace dztrader::core
