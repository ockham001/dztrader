#ifndef DZTRADER_WEBUI_NOTIFY_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_NOTIFY_DOMAIN_SERVICE_H_

#include "notify_cache.h"
#include "ws_broadcaster.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace dztrader::webui {

/// 通知领域服务：NOTIFY_UI 的缓存 + WS 广播 + 分级日志（契约 notify-ui）。
/// 行为逐行对照原 WsController::handle_notify_ui：
/// - NotifyCache::add(payload) 环形缓存（max_size=0 时为 no-op）；
/// - 广播 payload 原样透传（instance_id 为空）；
/// - level 为 "warning"/"error" 时记 WARN，否则 INFO（level 缺失或非字符串按 INFO）。
/// 原实现无 try/catch，本服务按任务要求统一包裹 try/catch 记 WARN（正常路径行为不变）。
/// 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），所有访问均发生在主循环串行执行，不加锁。
class NotifyDomainService {
public:
    NotifyDomainService(NotifyCache& cache, WsBroadcaster& ws) : cache_(cache), ws_(ws) {}

    void on_notify_ui(const nlohmann::json& payload) {
        try {
            cache_.add(payload);
            ws_.broadcast("notify_ui", "", payload);
            std::string message = payload.value("message", "");
            std::string source = payload.value("source", "");
            bool is_warn = false;
            if (payload.contains("level")) {
                const auto& lvl = payload["level"];
                if (lvl.is_string()) {
                    const auto& s = lvl.get<std::string>();
                    is_warn = (s == "warning" || s == "error");
                }
            }
            if (is_warn) {
                SPDLOG_WARN("notify_ui pushed | source={} message={}", source, message);
            } else {
                SPDLOG_INFO("notify_ui pushed | source={} message={}", source, message);
            }
        } catch (const std::exception& e) {
            SPDLOG_WARN("failed to process NOTIFY_UI | error={}", e.what());
        }
    }

private:
    NotifyCache& cache_;
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_NOTIFY_DOMAIN_SERVICE_H_
