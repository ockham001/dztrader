#ifndef DZTRADER_WEBUI_NOTIFY_CACHE_H_
#define DZTRADER_WEBUI_NOTIFY_CACHE_H_

#include <deque>
#include <vector>
#include <nlohmann/json.hpp>

namespace dztrader::webui {

/// 最近 NOTIFY_UI 消息的环形缓存。
/// max_size from WebuiConfig, 0 = disabled (add is no-op)。
/// 线程安全: 依赖 dzweb 固定单线程事件循环(thread_num=1), 所有访问均发生在主循环串行执行,
/// 故不加锁。若未来引入多线程必须重新评估加锁。
class NotifyCache {
public:
    explicit NotifyCache(size_t max_size) : max_size_(max_size) {}

    /// 运行时调整缓存上限。设为 0 表示禁用写入(不清空已有缓存)；
    /// 缩小到小于当前条数时,由后续 add() 逐条 pop_front 自然收敛。
    /// 依赖 dzweb 单线程事件循环串行执行,不加锁。
    void set_max_size(size_t max_size) { max_size_ = max_size; }

    void add(const nlohmann::json& payload) {
        if (max_size_ == 0) return;
        if (messages_.size() >= max_size_) {
            messages_.pop_front();
        }
        messages_.push_back(payload);
    }

    std::vector<nlohmann::json> get_all() const {
        return {messages_.begin(), messages_.end()};
    }

private:
    size_t max_size_;
    std::deque<nlohmann::json> messages_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_NOTIFY_CACHE_H_
