#include <gtest/gtest.h>

#include "timer_heap.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <utility>
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
    EXPECT_TRUE(heap.remove_token(0));     // 堆顶
    EXPECT_TRUE(heap.remove_token(25));    // 中间
    EXPECT_TRUE(heap.remove_token(49));    // 尾部
    EXPECT_FALSE(heap.remove_token(100));  // 不存在
    int64_t prev = -1;
    std::vector<uint64_t> popped_tokens;
    while (!heap.empty()) {
        const auto node = heap.pop();
        const int64_t ms = (node.deadline - base) / std::chrono::milliseconds(1);
        EXPECT_GE(ms, prev);
        prev = ms;
        popped_tokens.push_back(node.token);
    }
    // 残留集合是取消语义的锚点: 总是删 index 0 的实现会留下 25/49, 此处变红
    std::vector<uint64_t> expected_tokens;
    for (uint64_t token = 0; token < 50; ++token) {
        if (token != 0 && token != 25 && token != 49) {
            expected_tokens.push_back(token);
        }
    }
    std::sort(popped_tokens.begin(), popped_tokens.end());
    EXPECT_EQ(popped_tokens, expected_tokens);
}

TEST(TimerHeap, RemoveTokenReplacementBubblesUp) {
    // 构造: 删除 token=101 (index 3) 时, 换入的尾元素 {4} 小于父 {100},
    // 触发 sift_up 链; 其后的 sift_down(原索引) 作用于被换下的父元素,
    // 按堆不变量必为 no-op。验证堆性质在双向修正后仍保持。
    TimerHeap heap;
    const auto base = Clock::now();
    const std::vector<std::pair<int64_t, uint64_t>> init = {
        {1, 1}, {100, 100}, {2, 2}, {101, 101}, {102, 102}, {3, 3}, {4, 4}};
    for (const auto& [ms, token] : init) {
        heap.push(base + std::chrono::milliseconds(ms), token);
    }
    EXPECT_TRUE(heap.remove_token(101));
    int64_t prev = -1;
    std::vector<uint64_t> popped_tokens;
    while (!heap.empty()) {
        const auto node = heap.pop();
        const int64_t ms = (node.deadline - base) / std::chrono::milliseconds(1);
        EXPECT_GE(ms, prev);
        prev = ms;
        popped_tokens.push_back(node.token);
    }
    // 被删的必须是 token 101 (而非 index 0 的 token 1): 残留集合逐 token 对齐
    std::sort(popped_tokens.begin(), popped_tokens.end());
    EXPECT_EQ(popped_tokens, (std::vector<uint64_t>{1, 2, 3, 4, 100, 102}));
}

TEST(TimerHeap, RemoveTokensBelowRebuildsHeap) {
    TimerHeap heap;
    const auto base = Clock::now();
    constexpr uint64_t kInternal = 1ull << 63;
    // 5 个内部条目 (token >= 阈值) 交错在用户条目之间; 过滤后按插入序残留
    // [30, 10, 50, 20, 25] (非堆序), 重建循环必须真正下沉才能恢复堆性质。
    const std::vector<std::pair<int64_t, uint64_t>> entries = {
        {5, 1},               // 用户条目: 被过滤
        {30, kInternal | 0},  // 内部条目: 保留
        {100, 2},             // 用户条目: 被过滤
        {10, kInternal | 1},  // 内部条目: 保留
        {7, 3},               // 用户条目: 被过滤
        {50, kInternal | 2},  // 内部条目: 保留
        {20, kInternal | 3},  // 内部条目: 保留
        {25, kInternal | 4},  // 内部条目: 保留
    };
    for (const auto& [ms, token] : entries) {
        heap.push(base + std::chrono::milliseconds(ms), token);
    }
    heap.remove_tokens_below(kInternal);

    // 重建后堆顶 = 最小 deadline; 跳过重建时堆顶是 30ms (首条残留)
    ASSERT_FALSE(heap.empty());
    EXPECT_EQ(heap.top().token, kInternal | 1);

    std::vector<std::pair<int64_t, uint64_t>> popped;
    while (!heap.empty()) {
        const auto node = heap.pop();
        popped.emplace_back((node.deadline - base) / std::chrono::milliseconds(1), node.token);
    }
    // deadline 升序 + 残留 token 集合: 两者共同锚定过滤与重建语义
    EXPECT_EQ(popped, (std::vector<std::pair<int64_t, uint64_t>>{
                          {10, kInternal | 1},
                          {20, kInternal | 3},
                          {25, kInternal | 4},
                          {30, kInternal | 0},
                          {50, kInternal | 2},
                      }));
}

}  // namespace
