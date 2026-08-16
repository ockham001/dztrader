#ifndef DZTRADER_WEBUI_MD_STATUS_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_MD_STATUS_DOMAIN_SERVICE_H_

#include "process_mirror.h"
#include "ws_broadcaster.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace dztrader::webui {

/// 行情源状态领域服务：RTN_MD_STATUS 的镜像更新 + WS 广播（契约 md-status）。
/// 行为逐行对照原 WsController::handle_rtn_md_status：
/// - 镜像更新 process_mirror.update_gateway_status(source, payload)；
/// - 广播 payload = {source, status}（instance_id 为空，source 在 payload 里）。
/// 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），所有访问均发生在主循环串行执行，不加锁。
class MdStatusDomainService {
public:
    MdStatusDomainService(ProcessMirror& process_mirror, WsBroadcaster& ws)
        : process_mirror_(process_mirror), ws_(ws) {}

    void on_rtn_md_status(const std::string& source, const nlohmann::json& payload) {
        try {
            process_mirror_.update_gateway_status(source, payload);
            ws_.broadcast("md_rtn_status", "",
                          nlohmann::json{{"source", source}, {"status", payload}});
            SPDLOG_DEBUG("md_rtn_status pushed | source={}", source);
        } catch (const std::exception& e) {
            SPDLOG_WARN("failed to parse RTN_MD_STATUS | error={}", e.what());
        }
    }

private:
    ProcessMirror& process_mirror_;
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_MD_STATUS_DOMAIN_SERVICE_H_
