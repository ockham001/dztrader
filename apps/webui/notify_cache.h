#ifndef DZTRADER_WEBUI_NOTIFY_CACHE_H_
#define DZTRADER_WEBUI_NOTIFY_CACHE_H_

#include <deque>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace dztrader::webui {

/// 最近 NOTIFY_UI 消息的环形缓存 + 去重折叠（契约 notify-ui，ADR 0060 决策 4）。
/// max_size from WebuiConfig, 0 = disabled (add is no-op)。
///
/// 去重语义（唯一漏斗在 dzweb）：
/// - 指纹 = `source + "\x1f" + level + "\x1f" + 归一化文本`；
/// - 归一化文本 = 把连续数字、`HH:MM:SS` 类时间串、价格类小数替换为占位符，
///   使「同一事件、不同订单号/时间戳」落成同一指纹；
/// - 同指纹在 `window` 秒内**合并为一条**：`count++`、就地更新 `timestamp`（最后发生时间），
///   返回该条目的索引供调用方广播；窗口内**不新增条目**；
/// - 超过 `window` 但未超 `cooldown`：仍合并计数（避免同一故障在窗口边界处抖出两条），
///   就地更新时间戳；
/// - 超过 `cooldown`：视为新事件，新增条目并重置计数。冷却判定依据是**该指纹最后一次
///   出现的时间**（不是条目的创建时间），符合「持续故障」与「已恢复后再次发生」两种情况。
///
/// 线程安全: 依赖 dzweb 固定单线程事件循环(thread_num=1), 所有访问均发生在主循环串行执行,
/// 故不加锁。若未来引入多线程必须重新评估加锁。
class NotifyCache {
public:
    /// @param max_size 环形缓存上限，0 = 禁用写入
    /// @param dedup_window_sec 去重窗口（秒），<=0 表示关闭去重
    /// @param dedup_cooldown_sec 冷却（秒），<=0 表示不设冷却（永不作为新条目重现）
    explicit NotifyCache(size_t max_size, int dedup_window_sec = 60, int dedup_cooldown_sec = 300)
        : max_size_(max_size),
          dedup_window_sec_(dedup_window_sec),
          dedup_cooldown_sec_(dedup_cooldown_sec) {}

    /// 运行时调整缓存上限。设为 0 表示禁用写入(不清空已有缓存)；
    /// 缩小到小于当前条数时,由后续 add() 逐条 pop_front 自然收敛。
    /// 依赖 dzweb 单线程事件循环串行执行,不加锁。
    void set_max_size(size_t max_size) { max_size_ = max_size; }

    /// 运行时调整去重参数（配置热更新用；<=0 分别关闭去重/不设冷却）。
    void set_dedup(int window_sec, int cooldown_sec) {
        dedup_window_sec_ = window_sec;
        dedup_cooldown_sec_ = cooldown_sec;
    }

    /// 判断是否需要把一条 payload 折叠到已有条目上。
    ///
    /// @param payload 待处理的 NOTIFY_UI payload
    /// @param now_sec 当前 Unix 秒（调用方注入，便于测试）
    /// @return 命中已有条目时返回其**折叠后的 payload**（已更新 `count` 与 `timestamp`）；
    ///         未命中返回 nullopt 表示这应作为新条目。
    ///
    /// 命中时调用方**不得**再调 add()：缓存条目已就地更新，只需广播返回值即可。
    /// 未命中时调用方应调 add()（它会补 `count=1` 并遵守 max_size）。
    [[nodiscard]] std::optional<nlohmann::json> try_fold(const nlohmann::json& payload,
                                                         int64_t now_sec) {
        if (max_size_ == 0 || dedup_window_sec_ <= 0) {
            return std::nullopt;
        }
        const std::string fp = fingerprint(payload);
        if (fp.empty()) {
            return std::nullopt;
        }
        // 从新到旧扫描：同一指纹的最近一条优先（环形缓存有上限，旧条目可能已被挤出）。
        for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
            auto& entry = *it;
            if (fingerprint(entry) != fp) {
                continue;
            }
            const int64_t last_seen = entry.value("timestamp", static_cast<int64_t>(0));
            const int64_t age = now_sec - last_seen;
            // 冷却已过：视为新事件，交给调用方 add()（不在这里返回命中）。
            if (dedup_cooldown_sec_ > 0 && age >= dedup_cooldown_sec_) {
                return std::nullopt;
            }
            // 窗口内外都合并计数：窗口内是同一事件；窗口外、冷却内是同一故障的延续。
            const int prev_count = entry.value("count", 1);
            entry["count"] = prev_count + 1;
            entry["timestamp"] = now_sec;
            return entry;
        }
        return std::nullopt;
    }

    /// 新增一条通知。调用方已通过 try_fold 确认未命中。
    /// 自动补 `count=1`（若 payload 未带）；超上限时 pop_front。
    void add(const nlohmann::json& payload) {
        if (max_size_ == 0) {
            return;
        }
        nlohmann::json entry = payload;
        if (!entry.contains("count")) {
            entry["count"] = 1;
        }
        if (messages_.size() >= max_size_) {
            messages_.pop_front();
        }
        messages_.push_back(std::move(entry));
    }

    std::vector<nlohmann::json> get_all() const { return {messages_.begin(), messages_.end()}; }

    /// 指纹归一化（公开供测试）：`source\x1flevel\x1f归一化文本`。
    /// 归一化把连续数字 / 时间串 / 小数替换为 `#`，使订单号、时间戳、价格不影响指纹。
    [[nodiscard]] static std::string fingerprint(const nlohmann::json& payload) {
        const std::string source = payload.value("source", "");
        const std::string level = payload.value("level", "");
        const std::string message = payload.value("message", "");
        if (source.empty() && level.empty() && message.empty()) {
            return {};
        }
        return source + '\x1f' + level + '\x1f' + normalize(message);
    }

    /// 文本归一化：连续数字（含小数、带千分位逗号、时间串）统一替换为单个 `#`。
    /// 只替换数字，保留 ASCII 标点与中文，避免归一化过度把不同事件合并。
    [[nodiscard]] static std::string normalize(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        bool in_number = false;
        for (const char c : text) {
            const bool is_digit = c >= '0' && c <= '9';
            // 数字内部的 `.` `,` `:` `-` 视为同一数字的一部分（覆盖 1,234.56 / 14:05:23 /
            // 2026-11-27）。
            const bool is_number_sep = in_number && (c == '.' || c == ',' || c == ':' || c == '-');
            if (is_digit || is_number_sep) {
                if (!in_number) {
                    out.push_back('#');
                }
                in_number = true;
                continue;
            }
            in_number = false;
            out.push_back(c);
        }
        return out;
    }

private:
    size_t max_size_;
    int dedup_window_sec_;
    int dedup_cooldown_sec_;
    std::deque<nlohmann::json> messages_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_NOTIFY_CACHE_H_
