#include <gtest/gtest.h>

#include "timer_heap.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

using dztrader::TimerHeap;

namespace {

using Clock = TimerHeap::Clock;

TEST(TimerHeap, EmptyAndTop) {
    TimerHeap heap;
    EXPECT_TRUE(heap.empty());
    heap.push(Clock::now() + std::chrono::seconds(1), 1);
    EXPECT_FALSE(heap.empty());
    EXPECT_EQ(heap.top().token, 1u);
    heap.clear();
    EXPECT_TRUE(heap.empty());
}

TEST(TimerHeap, PopReturnsEarliestDeadline) {
    TimerHeap heap;
    const auto base = Clock::now();
    heap.push(base + std::chrono::milliseconds(300), 3);
    heap.push(base + std::chrono::milliseconds(100), 1);
    heap.push(base + std::chrono::milliseconds(200), 2);
    EXPECT_EQ(heap.pop().token, 1u);
    EXPECT_EQ(heap.pop().token, 2u);
    EXPECT_EQ(heap.pop().token, 3u);
    EXPECT_TRUE(heap.empty());
}

TEST(TimerHeap, EqualDeadlinesKeepTokens) {
    TimerHeap heap;
    const auto t = Clock::now() + std::chrono::seconds(1);
    heap.push(t, 7);
    heap.push(t, 8);
    heap.push(t, 9);
    std::vector<uint64_t> popped;
    while (!heap.empty()) {
        popped.push_back(heap.pop().token);
    }
    std::sort(popped.begin(), popped.end());
    EXPECT_EQ(popped, (std::vector<uint64_t>{7, 8, 9}));
}

TEST(TimerHeap, TwoHundredNodesMaintainHeapOrder) {
    TimerHeap heap;
    std::mt19937_64 gen{42};
    const auto base = Clock::now();
    std::vector<int64_t> deadlines;
    for (int i = 0; i < 200; ++i) {
        const int64_t ms = static_cast<int64_t>(gen() % 100000);
        deadlines.push_back(ms);
        heap.push(base + std::chrono::milliseconds(ms), static_cast<uint64_t>(ms));
    }
    std::sort(deadlines.begin(), deadlines.end());
    for (int i = 0; i < 200; ++i) {
        const auto node = heap.pop();
        EXPECT_EQ(node.deadline - base, std::chrono::milliseconds(deadlines[i]));
    }
    EXPECT_TRUE(heap.empty());
}

TEST(TimerHeap, RemoveTokenKeepsHeapProperty) {
    TimerHeap heap;
    const auto base = Clock::now();
    for (int i = 0; i < 50; ++i) {
        heap.push(base + std::chrono::milliseconds(i), static_cast<uint64_t>(i));
    }
    // 移除堆顶/中间/尾部各一个, 之后全部按序弹出
    EXPECT_TRUE(heap.remove_token(0));   // 堆顶
    EXPECT_TRUE(heap.remove_token(25));  // 中间
    EXPECT_TRUE(heap.remove_token(49));  // 尾部
    EXPECT_FALSE(heap.remove_token(100));  // 不存在
    int64_t prev = -1;
    int count = 0;
    while (!heap.empty()) {
        const auto node = heap.pop();
        const int64_t ms = (node.deadline - base) / std::chrono::milliseconds(1);
        EXPECT_GE(ms, prev);
        prev = ms;
        ++count;
    }
    EXPECT_EQ(count, 47);
}

TEST(TimerHeap, RemoveTokensBelowRebuildsHeap) {
    TimerHeap heap;
    const auto base = Clock::now();
    constexpr uint64_t kInternal = 1ull << 63;
    for (int i = 0; i < 50; ++i) {
        heap.push(base + std::chrono::milliseconds(i), static_cast<uint64_t>(i));
    }
    heap.push(base + std::chrono::milliseconds(5), kInternal);
    heap.remove_tokens_below(kInternal);
    // 仅内部条目保留, 且堆性质保持
    EXPECT_FALSE(heap.empty());
    EXPECT_EQ(heap.top().token, kInternal);
    EXPECT_EQ(heap.pop().token, kInternal);
    EXPECT_TRUE(heap.empty());
}

}  // namespace
