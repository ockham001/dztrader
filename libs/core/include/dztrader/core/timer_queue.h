/**
 * @file timer_queue.h
 * @brief 单线程定时器队列, 基于有序集合, 用于事件循环中的延时调度
 *
 * 与 NamedSemaphore::wait_for()/wait() 配合:
 *   - 有定时器: next_timeout() 返回最近到期定时器剩余时间(native duration), 调用方
 *              按下游 API 需求转换(如 duration_cast<milliseconds> + clamp)
 *   - 无定时器: empty() 返回 true, 调用方走 wait() 无限等待
 * 使用 steady_clock, 不受系统时间调整影响。
 */
#ifndef DZTRADER_CORE_TIMER_QUEUE_H_
#define DZTRADER_CORE_TIMER_QUEUE_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>

namespace dztrader::core {

class TimerQueue {
public:
    using TimerId = uint64_t;
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;
    using Callback = std::function<void()>;

    /// 延时调度, 返回 TimerId 用于取消。
    /// delay 为负数或零时, 等价于 schedule_at 传入过去/当前时间点, 下次 tick() 立即触发。
    TimerId schedule_after(Duration delay, Callback cb);

    /// 定时调度, 在指定时间点触发。
    /// deadline 已过时, 下次 tick() 立即触发 (在 next_timeout() 中返回 zero)。
    TimerId schedule_at(TimePoint deadline, Callback cb);

    /// 延时调度（带标签去重）：tag 已存在时忽略，返回已有 TimerId。
    /// tag 为空时等同 schedule_after（不去重）。
    TimerId schedule_after_ignored(std::string tag, Duration delay, Callback cb);

    /// 延时调度（带标签去重）：tag 已存在时取消旧定时器，重新计时。
    /// tag 为空时等同 schedule_after（不去重）。
    TimerId schedule_after_replace(std::string tag, Duration delay, Callback cb);

    /// 取消定时器，id 无效时无操作
    void cancel(TimerId id);

    /// 触发所有已到期定时器，在事件循环中每轮调用
    void tick();

    /// 返回最近到期定时器的剩余时间 (steady_clock native duration, 通常 nanoseconds)。
    /// 前置条件: !empty()。空队列调用此方法是未定义行为 (调用方须先判 empty())。
    /// 已过期的定时器返回 zero (下次 tick() 立即触发)。
    /// 不做单位转换或截断, 调用方按下游 API 需求自行 duration_cast + clamp。
    [[nodiscard]] Duration next_timeout() const;

    /// 队列是否为空。与 next_timeout() 共享同一数据源 (deadlines_),
    /// 保证 empty() 返回 false 时 next_timeout() 可安全调用。
    [[nodiscard]] bool empty() const;

private:
    struct Entry {
        Callback callback;
        std::multimap<TimePoint, TimerId>::iterator deadline_it;
        std::string tag;
    };

    std::multimap<TimePoint, TimerId> deadlines_;
    std::unordered_map<TimerId, Entry> entries_;
    std::unordered_map<std::string, TimerId> tags_;
    TimerId next_id_ = 0;

    TimerId schedule_at_internal(TimePoint deadline, std::string tag, Callback cb);
};

}  // namespace dztrader::core

#endif  // DZTRADER_CORE_TIMER_QUEUE_H_
