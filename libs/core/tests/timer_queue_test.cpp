#include <dztrader/core/timer_queue.h>

#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using dztrader::core::TimerQueue;
using namespace std::chrono_literals;

// ── 基本调度与触发 ──

TEST(TimerQueue, ScheduleAfterTriggersOnTick) {
    TimerQueue tq;
    bool called = false;
    tq.schedule_after(0ms, [&] { called = true; });

    EXPECT_FALSE(called);
    tq.tick();
    EXPECT_TRUE(called);
}

TEST(TimerQueue, ScheduleAtTriggersOnTick) {
    TimerQueue tq;
    bool called = false;
    tq.schedule_at(TimerQueue::Clock::now(), [&] { called = true; });

    tq.tick();
    EXPECT_TRUE(called);
}

TEST(TimerQueue, FutureTimerNotTriggered) {
    TimerQueue tq;
    bool called = false;
    tq.schedule_after(10s, [&] { called = true; });

    tq.tick();
    EXPECT_FALSE(called);
}

TEST(TimerQueue, ExpiredTimerTriggersAfterWait) {
    TimerQueue tq;
    bool called = false;
    tq.schedule_after(10ms, [&] { called = true; });

    tq.tick();
    EXPECT_FALSE(called);

    std::this_thread::sleep_for(20ms);
    tq.tick();
    EXPECT_TRUE(called);
}

// ── 取消 ──

TEST(TimerQueue, CancelPreventsTrigger) {
    TimerQueue tq;
    bool called = false;
    auto id = tq.schedule_after(0ms, [&] { called = true; });

    tq.cancel(id);
    tq.tick();
    EXPECT_FALSE(called);
}

TEST(TimerQueue, CancelInvalidIdNoThrow) {
    TimerQueue tq;
    EXPECT_NO_THROW(tq.cancel(99999));
    EXPECT_NO_THROW(tq.cancel(0));
}

TEST(TimerQueue, CancelDoesNotAffectOtherTimers) {
    TimerQueue tq;
    bool called_a = false;
    bool called_b = false;
    auto id_a = tq.schedule_after(0ms, [&] { called_a = true; });
    tq.schedule_after(0ms, [&] { called_b = true; });

    tq.cancel(id_a);
    tq.tick();
    EXPECT_FALSE(called_a);
    EXPECT_TRUE(called_b);
}

// ── next_timeout ──

// 空队列: empty() 返回 true, next_timeout() 不应被调用 (前置条件 !empty())
TEST(TimerQueue, EmptyQueueEmptyTrueAndNoTimeout) {
    TimerQueue tq;
    EXPECT_TRUE(tq.empty());
    // next_timeout() 前置条件是 !empty(), 空队列不应调用 (UB)
}

// 有定时器时返回剩余时间 (native duration, 基于 steady_clock)
TEST(TimerQueue, NextTimeoutReturnsRemainingTime) {
    TimerQueue tq;
    tq.schedule_after(std::chrono::hours(1), [] {});
    ASSERT_FALSE(tq.empty());  // 前置条件
    auto remaining = tq.next_timeout();
    // 容忍调度抖动, 1 小时应在 58-60 分钟之间
    EXPECT_GE(remaining, std::chrono::minutes(58));
    EXPECT_LE(remaining, std::chrono::hours(1));
}

// 已过期定时器返回 zero (下次 tick() 立即触发)
TEST(TimerQueue, NextTimeoutReturnsZeroWhenExpired) {
    TimerQueue tq;
    tq.schedule_at(TimerQueue::Clock::now(), [] {});
    ASSERT_FALSE(tq.empty());  // 前置条件
    EXPECT_EQ(tq.next_timeout(), TimerQueue::Duration::zero());
}

// ── 多定时器顺序触发 ──

TEST(TimerQueue, MultipleTimersTriggerInOrder) {
    TimerQueue tq;
    std::vector<int> order;
    tq.schedule_after(0ms, [&] { order.push_back(1); });
    tq.schedule_after(0ms, [&] { order.push_back(2); });
    tq.schedule_after(0ms, [&] { order.push_back(3); });

    tq.tick();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(TimerQueue, TimersWithDifferentDeadlines) {
    TimerQueue tq;
    std::vector<int> order;
    tq.schedule_after(10ms, [&] { order.push_back(2); });
    tq.schedule_after(0ms, [&] { order.push_back(1); });

    tq.tick();
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], 1);

    std::this_thread::sleep_for(15ms);
    tq.tick();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[1], 2);
}

// ── tick 中回调调度新定时器 ──

TEST(TimerQueue, ScheduleInCallbackDoesNotTriggerInSameTick) {
    TimerQueue tq;
    bool second_called = false;
    tq.schedule_after(0ms, [&] {
        tq.schedule_after(0ms, [&] { second_called = true; });
    });

    tq.tick();
    EXPECT_FALSE(second_called);

    tq.tick();
    EXPECT_TRUE(second_called);
}

// ── empty ──

TEST(TimerQueue, EmptyInitially) {
    TimerQueue tq;
    EXPECT_TRUE(tq.empty());
}

TEST(TimerQueue, NotEmptyAfterSchedule) {
    TimerQueue tq;
    tq.schedule_after(1s, [] {});
    EXPECT_FALSE(tq.empty());
}

TEST(TimerQueue, EmptyAfterAllTriggered) {
    TimerQueue tq;
    tq.schedule_after(0ms, [] {});
    tq.tick();
    EXPECT_TRUE(tq.empty());
}

TEST(TimerQueue, EmptyAfterCancel) {
    TimerQueue tq;
    auto id = tq.schedule_after(1s, [] {});
    tq.cancel(id);
    EXPECT_TRUE(tq.empty());
}

// ── TimerId 唯一性 ──

TEST(TimerQueue, TimerIdsAreUnique) {
    TimerQueue tq;
    auto id1 = tq.schedule_after(1s, [] {});
    auto id2 = tq.schedule_after(1s, [] {});
    EXPECT_NE(id1, id2);
}

// ── 带标签去重调度 ──

TEST(TimerQueue, IgnoredSkipsDuplicateTag) {
    TimerQueue tq;
    int count = 0;
    auto id1 = tq.schedule_after_ignored("tag1", 0ms, [&] { ++count; });
    auto id2 = tq.schedule_after_ignored("tag1", 0ms, [&] { ++count; });

    EXPECT_EQ(id1, id2);
    tq.tick();
    EXPECT_EQ(count, 1);
}

TEST(TimerQueue, IgnoredDifferentTagsBothSchedule) {
    TimerQueue tq;
    int count = 0;
    tq.schedule_after_ignored("tag1", 0ms, [&] { ++count; });
    tq.schedule_after_ignored("tag2", 0ms, [&] { ++count; });

    tq.tick();
    EXPECT_EQ(count, 2);
}

TEST(TimerQueue, IgnoredTagReuseAfterFire) {
    TimerQueue tq;
    int count = 0;
    tq.schedule_after_ignored("tag1", 0ms, [&] { ++count; });
    tq.tick();
    EXPECT_EQ(count, 1);

    // tag 已释放，可以再次使用
    tq.schedule_after_ignored("tag1", 0ms, [&] { ++count; });
    tq.tick();
    EXPECT_EQ(count, 2);
}

TEST(TimerQueue, IgnoredTagReuseAfterCancel) {
    TimerQueue tq;
    int count = 0;
    auto id = tq.schedule_after_ignored("tag1", 10s, [&] { ++count; });
    tq.cancel(id);

    // tag 已释放
    tq.schedule_after_ignored("tag1", 0ms, [&] { ++count; });
    tq.tick();
    EXPECT_EQ(count, 1);
}

TEST(TimerQueue, ReplaceCancelsOldAndReschedules) {
    TimerQueue tq;
    int count = 0;
    auto id1 = tq.schedule_after_replace("tag1", 10s, [&] { ++count; });
    auto id2 = tq.schedule_after_replace("tag1", 0ms, [&] { ++count; });

    EXPECT_NE(id1, id2);
    tq.tick();
    EXPECT_EQ(count, 1);
}

TEST(TimerQueue, ReplaceDifferentTagsBothSchedule) {
    TimerQueue tq;
    int count = 0;
    tq.schedule_after_replace("tag1", 0ms, [&] { ++count; });
    tq.schedule_after_replace("tag2", 0ms, [&] { ++count; });

    tq.tick();
    EXPECT_EQ(count, 2);
}

TEST(TimerQueue, ReplaceTagReuseAfterFire) {
    TimerQueue tq;
    int count = 0;
    tq.schedule_after_replace("tag1", 0ms, [&] { ++count; });
    tq.tick();
    EXPECT_EQ(count, 1);

    tq.schedule_after_replace("tag1", 0ms, [&] { ++count; });
    tq.tick();
    EXPECT_EQ(count, 2);
}

TEST(TimerQueue, ReplaceResetsDeadline) {
    TimerQueue tq;
    int count = 0;
    tq.schedule_after_replace("tag1", 0ms, [&] { ++count; });
    // replace with a future deadline — old 0ms timer should be cancelled
    tq.schedule_after_replace("tag1", 10s, [&] { ++count; });

    tq.tick();
    EXPECT_EQ(count, 0);  // old cancelled, new not yet expired
}

TEST(TimerQueue, IgnoredEmptyTagBehavesLikeUntagged) {
    TimerQueue tq;
    int count = 0;
    auto id1 = tq.schedule_after_ignored("", 0ms, [&] { ++count; });
    auto id2 = tq.schedule_after_ignored("", 0ms, [&] { ++count; });

    EXPECT_NE(id1, id2);  // empty tag does not dedup
    tq.tick();
    EXPECT_EQ(count, 2);
}

TEST(TimerQueue, ReplaceEmptyTagBehavesLikeUntagged) {
    TimerQueue tq;
    int count = 0;
    auto id1 = tq.schedule_after_replace("", 0ms, [&] { ++count; });
    auto id2 = tq.schedule_after_replace("", 0ms, [&] { ++count; });

    EXPECT_NE(id1, id2);  // empty tag does not dedup
    tq.tick();
    EXPECT_EQ(count, 2);
}

TEST(TimerQueue, CancelReplacedOldIdNoThrow) {
    TimerQueue tq;
    int count = 0;
    auto id1 = tq.schedule_after_replace("tag1", 10s, [&] { ++count; });
    tq.schedule_after_replace("tag1", 10s, [&] { ++count; });  // replaces id1

    EXPECT_NO_THROW(tq.cancel(id1));  // old id already cancelled
    EXPECT_EQ(count, 0);
}

TEST(TimerQueue, TaggedDoesNotAffectUntagged) {
    TimerQueue tq;
    int count = 0;
    tq.schedule_after_ignored("tag1", 0ms, [&] { ++count; });
    tq.schedule_after(0ms, [&] { ++count; });  // untagged, independent

    tq.tick();
    EXPECT_EQ(count, 2);
}

// ── next_timeout 边界测试 ──

// 验证新签名 next_timeout() 无参数 (编译期检查)
TEST(TimerQueue, NextTimeoutNoDefaultArg) {
    TimerQueue tq;
    tq.schedule_after(100ms, [] {});
    ASSERT_FALSE(tq.empty());
    auto to = tq.next_timeout();  // 无参调用, 返回 Duration
    EXPECT_LE(to, 100ms);
}

// 多定时器时返回最早到期的剩余时间
TEST(TimerQueue, NextTimeoutMultipleTimersReturnsEarliest) {
    TimerQueue tq;
    tq.schedule_after(500ms, [] {});
    tq.schedule_after(100ms, [] {});  // 最早
    tq.schedule_after(1000ms, [] {});
    ASSERT_FALSE(tq.empty());
    auto to = tq.next_timeout();
    EXPECT_LE(to, 100ms);
    EXPECT_GT(to, TimerQueue::Duration::zero());  // 不应已过期
}

// 取消最早定时器后, next_timeout 返回次早的剩余
TEST(TimerQueue, NextTimeoutAfterCancelUpdates) {
    TimerQueue tq;
    auto id1 = tq.schedule_after(100ms, [] {});
    tq.schedule_after(500ms, [] {});
    ASSERT_FALSE(tq.empty());
    EXPECT_LE(tq.next_timeout(), 100ms);

    tq.cancel(id1);  // 取消最早的
    ASSERT_FALSE(tq.empty());
    auto to = tq.next_timeout();
    EXPECT_GE(to, 400ms);  // 现在最近的是 500ms 那个
    EXPECT_LE(to, 500ms);
}

// tick 触发所有已过期定时器后, 队列变空 (不应再调 next_timeout)
TEST(TimerQueue, NextTimeoutAfterTickEmpty) {
    TimerQueue tq;
    tq.schedule_at(TimerQueue::Clock::now(), [] {});  // 立即过期
    ASSERT_FALSE(tq.empty());
    tq.tick();
    EXPECT_TRUE(tq.empty());
}

// 连续调用 (无 tick/cancel) 返回值应稳定 (允许 1ms 抖动)
TEST(TimerQueue, NextTimeoutSteadyUnderNoAdvance) {
    TimerQueue tq;
    tq.schedule_after(500ms, [] {});
    ASSERT_FALSE(tq.empty());
    auto to1 = tq.next_timeout();
    auto to2 = tq.next_timeout();
    EXPECT_LE(std::chrono::abs(to1 - to2), 1ms);
}

// 长期定时器(> uint32_t ms 上限 ~49.7 天)不应被截断 - 验证 Duration 返回值无溢出
TEST(TimerQueue, NextTimeoutHandlesLongDuration) {
    TimerQueue tq;
    tq.schedule_after(std::chrono::hours(24 * 60), [] {});  // 60 天 > uint32_t 上限
    ASSERT_FALSE(tq.empty());
    auto remaining = tq.next_timeout();
    EXPECT_GT(remaining, std::chrono::hours(24 * 59));
    EXPECT_LT(remaining, std::chrono::hours(24 * 60));
}

// tick 部分触发后, next_timeout 返回剩余定时器中最早的
TEST(TimerQueue, NextTimeoutAfterPartialTick) {
    TimerQueue tq;
    tq.schedule_at(TimerQueue::Clock::now(), [] {});              // 立即过期
    tq.schedule_after(std::chrono::hours(1), [] {});              // 1 小时后
    ASSERT_FALSE(tq.empty());
    tq.tick();  // 仅触发过期的
    ASSERT_FALSE(tq.empty());  // 还有 1 小时那个
    auto to = tq.next_timeout();
    EXPECT_GE(to, std::chrono::minutes(58));
    EXPECT_LE(to, std::chrono::hours(1));
}

// 回调内取消其他待触发定时器不应崩溃 (迭代器安全)
TEST(TimerQueue, CancelOtherTimerInCallback) {
    TimerQueue tq;
    bool called_b = false;
    auto id_b = tq.schedule_after(0ms, [&] { called_b = true; });
    tq.schedule_after(0ms, [&] { tq.cancel(id_b); });  // 回调内取消 b
    EXPECT_NO_FATAL_FAILURE(tq.tick());  // 不应崩溃 (b 是否已触发取决于顺序)
}

// 回调内用 replace 重新调度同 tag 不应崩溃
TEST(TimerQueue, ReplaceSameTagInCallback) {
    TimerQueue tq;
    int count = 0;
    tq.schedule_after_replace("self", 0ms, [&] {
        ++count;
        tq.schedule_after_replace("self", 10s, [&] { ++count; });  // 回调内 replace 自身
    });
    EXPECT_NO_FATAL_FAILURE(tq.tick());
    EXPECT_EQ(count, 1);  // 第一轮只触发一次
    EXPECT_FALSE(tq.empty());  // 新的 10s 定时器存在
}

// ── 边界条件补充 ──

// 负数 delay 等价于 schedule_at 传入过去的时间点, 下次 tick 立即触发
TEST(TimerQueue, ScheduleAfterNegativeDelayTriggersImmediately) {
    TimerQueue tq;
    bool called = false;
    tq.schedule_after(-10s, [&] { called = true; });

    EXPECT_FALSE(tq.empty());
    ASSERT_FALSE(tq.empty());
    EXPECT_EQ(tq.next_timeout(), TimerQueue::Duration::zero());  // 已过期, 返回 zero
    tq.tick();
    EXPECT_TRUE(called);
    EXPECT_TRUE(tq.empty());
}

// schedule_at 传入过去的时间点, 下次 tick 立即触发
TEST(TimerQueue, ScheduleAtPastDeadlineTriggersImmediately) {
    TimerQueue tq;
    bool called = false;
    auto past = TimerQueue::Clock::now() - std::chrono::hours(1);
    tq.schedule_at(past, [&] { called = true; });

    EXPECT_FALSE(tq.empty());
    ASSERT_FALSE(tq.empty());
    EXPECT_EQ(tq.next_timeout(), TimerQueue::Duration::zero());
    tq.tick();
    EXPECT_TRUE(called);
    EXPECT_TRUE(tq.empty());
}

// 回调内 cancel 自身不应崩溃 (自身 entry 已被 erase, cancel 找不到, 无操作)
TEST(TimerQueue, CancelSelfInCallbackNoThrow) {
    TimerQueue tq;
    TimerQueue::TimerId self_id = 0;
    bool called = false;
    self_id = tq.schedule_after(0ms, [&] {
        called = true;
        tq.cancel(self_id);  // 自身已被 erase, cancel 无操作
    });
    EXPECT_NO_FATAL_FAILURE(tq.tick());
    EXPECT_TRUE(called);
    EXPECT_TRUE(tq.empty());
}

// 相同 deadline 的多个定时器 (schedule_at 同一 TimePoint) 在一次 tick 中全部触发。
// 验证 multimap 相同 key 的多条目都能被遍历触发, 不遗漏。
TEST(TimerQueue, MultipleTimersWithSameDeadlineAllTrigger) {
    TimerQueue tq;
    auto deadline = TimerQueue::Clock::now();
    int count = 0;
    tq.schedule_at(deadline, [&] { ++count; });
    tq.schedule_at(deadline, [&] { ++count; });
    tq.schedule_at(deadline, [&] { ++count; });

    ASSERT_FALSE(tq.empty());
    tq.tick();
    EXPECT_EQ(count, 3);  // 三个相同 deadline 的定时器全部触发
    EXPECT_TRUE(tq.empty());
}
