#ifndef DZTRADER_WEBUI_AUTO_LOGIN_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_AUTO_LOGIN_DOMAIN_SERVICE_H_

#include "mirror_store.h"
#include "ws_broadcaster.h"
#include <dztrader/platform/auto_login.h>   // 只读引用静态校验函数，契约层零改动
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <string>

namespace dztrader::webui {

/// 自动登录/登出排程领域服务（契约 auto-login）：RTN_AUTO_LOGIN 的镜像 + WS 广播。
/// **校验义务（契约 auto-login）**：dzweb 读取时校验全量，非法则记日志并忽略（不更新镜像、不广播）。
class AutoLoginDomainService {
public:
    AutoLoginDomainService(MirrorStore& mirror, WsBroadcaster& ws)
        : mirror_(mirror), ws_(ws) {}

    void on_rtn_auto_login(const std::string& source, const nlohmann::json& payload) {
        // 契约 auto-login：dzweb 读取时校验全量，非法则记日志并忽略（不更新镜像）
        if (auto err = dztrader::platform::AutoLoginConfig::validate(payload)) {
            SPDLOG_WARN("invalid RTN_AUTO_LOGIN ignored | source={} error={}", source, *err);
            return;
        }
        mirror_.update(source, "auto_login", payload);
        ws_.broadcast("auto_login", source, payload);
    }

private:
    MirrorStore& mirror_;
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_AUTO_LOGIN_DOMAIN_SERVICE_H_
