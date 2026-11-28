#ifndef DZTRADER_WEBUI_NOTIFY_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_NOTIFY_DOMAIN_SERVICE_H_

#include "notify_cache.h"
#include "ws_broadcaster.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <string>

namespace dztrader::webui {

/// 通知领域服务：NOTIFY_UI 的去重折叠 + 缓存 + WS 广播 + 日志（契约 notify-ui，ADR 0060）。
///
/// 处理顺序（**折叠先于缓存**，两者都在 `NotifyCache` 内，保证唯一漏斗）：
/// 1. `NotifyCache::try_fold` 判是否命中已有条目：
///    - 命中 → 就地 `count++` / 更新时间戳，广播**折叠后的 payload**（不新增条目）；
///    - 未命中 → `NotifyCache::add`（补 `count=1`）后广播**补齐后的条目**（不是原 payload，
///      否则 WS 增量缺 `count`，与 REST 基线不同形）。
/// 2. 转发日志用 DEBUG（ADR 0060 决策 5）：转发轨迹不是独立事件，
///    不得以 WARN 形态再报一次（旧实现把 error 通知打成 WARN，导致同一事件日志里出现两个级别）。
///
/// 线程安全：依赖 dzweb
/// 固定单线程事件循环（thread_num=1），所有访问均发生在主循环串行执行，不加锁。
class NotifyDomainService {
public:
    NotifyDomainService(NotifyCache& cache, WsBroadcaster& ws)
        : cache_(cache),
          ws_(ws) {}

    void on_notify_ui(const nlohmann::json& payload) {
        try {
            // 折叠判据用 **payload 自带的时间戳**（契约 notify-ui 的 `timestamp`，
            // Unix 秒 = 发送方产生该事件的时间），而不是 dzweb 收到的时间：
            // - 转发有延迟/积压时，按收到时间判会把「同一事件跨窗口」误判成窗口内；
            // - 也让本服务可被确定性测试（注入时间）。
            // 缺失或非整数时回退当前时间。
            const int64_t now_sec =
                payload.contains("timestamp") && payload["timestamp"].is_number_integer()
                    ? payload["timestamp"].get<int64_t>()
                    : std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
            // 折叠命中：缓存条目已就地更新，只广播折叠结果。
            if (auto folded = cache_.try_fold(payload, now_sec)) {
                ws_.broadcast("notify_ui", "", *folded);
                SPDLOG_DEBUG("notify_ui folded | source={} count={}", folded->value("source", ""),
                             folded->value("count", 1));
                return;
            }
            // 未命中折叠：写入缓存（add 会补 count=1），并广播**补齐后**的条目。
            // 不能广播原始 payload —— 那样 WS 增量里没有 count，前端拿不到契约声明的字段
            // （首条会缺字段，与 REST 基线不同形）。
            const auto stored = cache_.add(payload);
            ws_.broadcast("notify_ui", "", stored);
            SPDLOG_DEBUG("notify_ui pushed | source={} message=\"{}\"", stored.value("source", ""),
                         stored.value("message", ""));
        } catch (const std::exception& e) {
            SPDLOG_WARN("failed to process NOTIFY_UI | error=\"{}\"", e.what());
        }
    }

private:
    NotifyCache& cache_;
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_NOTIFY_DOMAIN_SERVICE_H_
