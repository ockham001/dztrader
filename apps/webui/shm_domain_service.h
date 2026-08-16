#ifndef DZTRADER_WEBUI_SHM_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_SHM_DOMAIN_SERVICE_H_

#include "mirror_store.h"
#include "ws_broadcaster.h"
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace dztrader::webui {

/// SHM 通道配置领域服务（契约 shm）：RTN_EVENT_SHM_CONFIG / RTN_MD_SHM_CONFIG 的镜像 + WS 广播。
/// 镜像 key：事件通道配置无 instance_id，固定挂 dztraderd（P1 镜像约定：无 instance_id 帧统一挂 master 实例）；
/// 行情通道配置挂帧头 instance_id。
class ShmDomainService {
public:
    ShmDomainService(MirrorStore& mirror, WsBroadcaster& ws)
        : mirror_(mirror), ws_(ws) {}

    void on_rtn_event_shm_config(const nlohmann::json& payload) {
        mirror_.update(std::string(kMasterInstance), "event_shm_config", payload);        ws_.broadcast("event_shm_config", "", payload);
    }

    void on_rtn_md_shm_config(const std::string& source, const nlohmann::json& payload) {
        mirror_.update(source, "md_shm_config", payload);
        ws_.broadcast("md_shm_config", source, payload);
    }

private:
    MirrorStore& mirror_;
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_SHM_DOMAIN_SERVICE_H_
