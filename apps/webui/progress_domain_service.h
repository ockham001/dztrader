#ifndef DZTRADER_WEBUI_PROGRESS_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_PROGRESS_DOMAIN_SERVICE_H_

#include "mirror_store.h"
#include "ws_broadcaster.h"
#include <nlohmann/json.hpp>
#include <string>

namespace dztrader::webui {

/// 进度领域服务（契约 progress）：RTN_PROGRESS 的镜像 + WS 广播。
/// 单条完整状态：前端收到后直接覆盖，后到覆盖先到；无 SET、无持久化。
class ProgressDomainService {
public:
    ProgressDomainService(MirrorStore& mirror, WsBroadcaster& ws)
        : mirror_(mirror), ws_(ws) {}

    void on_rtn_progress(const std::string& source, const nlohmann::json& payload) {
        mirror_.update(source, "progress", payload);
        ws_.broadcast("progress", source, payload);
    }

private:
    MirrorStore& mirror_;
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_PROGRESS_DOMAIN_SERVICE_H_
