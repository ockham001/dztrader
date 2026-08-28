/**
 * @file timer_heap.h
 * @brief 策略 SDK 专用定时器堆 — 平坦 vector 小顶堆（私有实现，不对外）
 *
 * 相比 core::TimerQueue（multimap + unordered_map + std::function + tag）：
 *   - 单容器：16B 连续条目，无每次调度分配，缓存友好（200 条 = 3.2KB 全热）
 *   - 零回调对象：条目仅 {deadline, token}，派发逻辑在 DzContext（用户稳定 ID /
 *     内部预加载令牌）
 *   - 用户取消走懒删除（注册表判定，零队列往返）；内部替换走物理移除
 *     （remove_token，冷路径 O(n)，内部条目 ≤2）
 * 正确性：标准数组二叉堆（小顶），对任意 n 保持堆不变量。
 */
#ifndef DZTRADER_STRATEGY_API_TIMER_HEAP_H_
#define DZTRADER_STRATEGY_API_TIMER_HEAP_H_

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace dztrader {

class TimerHeap {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Node {
        TimePoint deadline;
        uint64_t token;  ///< 用户定时器: 稳定 ID; 内部定时器: 带高位标记的令牌
    };
    static_assert(sizeof(Node) == 16, "timer heap node must be 16 bytes");

    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

    /// 最近到期条目（前置条件: !empty()）
    [[nodiscard]] const Node& top() const noexcept { return nodes_.front(); }

    void push(TimePoint deadline, uint64_t token) {
        nodes_.push_back(Node{deadline, token});
        sift_up(nodes_.size() - 1);
    }

    /// 弹出最近到期条目（前置条件: !empty()）
    Node pop() noexcept {
        const Node top = nodes_.front();
        nodes_.front() = nodes_.back();
        nodes_.pop_back();
        if (!nodes_.empty()) {
            sift_down(0);
        }
        return top;
    }

    /// 物理移除指定 token 的条目（线性查找；冷路径：内部定时器替换/清理）。
    /// @return true 已移除；false 不存在
    bool remove_token(uint64_t token) noexcept {
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].token != token) {
                continue;
            }
            nodes_[i] = nodes_.back();
            nodes_.pop_back();
            if (i < nodes_.size()) {
                // 换入元素可能上浮或下沉，双向修正（标准堆删除）
                sift_up(i);
                sift_down(i);
            }
            return true;
        }
        return false;
    }

    /// 单遍过滤移除全部 token < threshold 的条目并重建堆（O(n)；
    /// 冷路径: 取消全部用户定时器, 内部令牌保留）。
    void remove_tokens_below(uint64_t threshold) noexcept {
        if (nodes_.empty()) {
            return;
        }
        std::size_t write = 0;
        for (std::size_t read = 0; read < nodes_.size(); ++read) {
            if (nodes_[read].token >= threshold) {
                nodes_[write++] = nodes_[read];
            }
        }
        nodes_.resize(write);
        // 过滤破坏堆性质: O(n) 重建
        for (std::size_t i = nodes_.size() / 2; i-- > 0;) {
            sift_down(i);
        }
    }

    void clear() noexcept { nodes_.clear(); }

private:
    static bool before(const Node& a, const Node& b) noexcept { return a.deadline < b.deadline; }

    void sift_up(std::size_t i) noexcept {
        while (i > 0) {
            const std::size_t parent = (i - 1) / 2;
            if (!before(nodes_[i], nodes_[parent])) {
                break;
            }
            std::swap(nodes_[i], nodes_[parent]);
            i = parent;
        }
    }

    void sift_down(std::size_t i) noexcept {
        for (;;) {
            const std::size_t left = 2 * i + 1;
            if (left >= nodes_.size()) {
                break;
            }
            const std::size_t right = left + 1;
            std::size_t smaller = left;
            if (right < nodes_.size() && before(nodes_[right], nodes_[left])) {
                smaller = right;
            }
            if (!before(nodes_[smaller], nodes_[i])) {
                break;
            }
            std::swap(nodes_[i], nodes_[smaller]);
            i = smaller;
        }
    }

    std::vector<Node> nodes_;
};

}  // namespace dztrader

#endif  // DZTRADER_STRATEGY_API_TIMER_HEAP_H_
