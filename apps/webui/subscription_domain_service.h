#ifndef DZTRADER_WEBUI_SUBSCRIPTION_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_SUBSCRIPTION_DOMAIN_SERVICE_H_

#include "ws_broadcaster.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace dztrader::webui {

/// 订阅领域服务：RTN_MD_SUBSCRIPTIONS 的 WS 广播（契约 md-subscription）。
/// 行为逐行对照原 WsController::handle_rtn_md_subscriptions：
/// - payload 拷贝后注入 source 字段，再整体作为广播 payload（原实现 std::move 语义等价，
///   消息内容逐字节一致）；
/// - 原实现无 try/catch，本服务按任务要求统一包裹 try/catch 记 WARN（正常路径行为不变）。
/// 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），所有访问均发生在主循环串行执行，不加锁。
class SubscriptionDomainService {
public:
    explicit SubscriptionDomainService(WsBroadcaster& ws) : ws_(ws) {}

    void on_rtn_md_subscriptions(const std::string& source, const nlohmann::json& payload) {
        try {
            nlohmann::json p = payload;
            p["source"] = source;
            ws_.broadcast("md_rtn_subscriptions", "", p);
            SPDLOG_DEBUG("md_rtn_subscriptions pushed | source={}", source);
        } catch (const std::exception& e) {
            SPDLOG_WARN("failed to parse RTN_MD_SUBSCRIPTIONS | error={}", e.what());
        }
    }

private:
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_SUBSCRIPTION_DOMAIN_SERVICE_H_
