#ifndef DZTRADER_WEBUI_SYSMON_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_SYSMON_DOMAIN_SERVICE_H_

#include "mirror_store.h"
#include "ws_broadcaster.h"

#include <dztrader/platform/sysmon.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace dztrader::webui {

/// 系统监控领域服务（契约 sysmon）：RTN_SYSMON_SNAPSHOT / RTN_SYSMON_CONFIG 的镜像 + WS 广播。
///
/// 快照帧无 instance_id、发送方固定 `dzsysmon`，按契约 sysmon §5 固定挂载到实例
/// `dzsysmon`（总则 §9 的固定挂载例外）。配置帧虽带 instance_id（恒为 dzsysmon），
/// 同样固定挂载到 `dzsysmon`，与契约 §5 的镜像表一致。
///
/// 校验义务（契约 sysmon §5）：dzweb 读取时必须校验 payload，非法则记日志并忽略
/// （不更新镜像、不广播）。
class SysmonDomainService {
public:
    /// 快照/配置的固定实例名（契约 sysmon §5）。
    static constexpr std::string_view kInstance = platform::SYSMON_INSTANCE_NAME;

    SysmonDomainService(MirrorStore& mirror, WsBroadcaster& ws)
        : mirror_(mirror),
          ws_(ws) {}

    /// RTN_SYSMON_SNAPSHOT（无 instance_id）。
    void on_rtn_sysmon_snapshot(const nlohmann::json& payload) {
        if (auto error = dztrader::platform::SysmonSnapshot::validate(payload)) {
            SPDLOG_WARN("invalid sysmon snapshot ignored | error={}", *error);
            return;
        }
        mirror_.update(std::string(kInstance), "sysmon_snapshot", payload);
        ws_.broadcast("sysmon_snapshot", std::string(kInstance), payload);
    }

    /// RTN_SYSMON_CONFIG（定向帧，source = 来源进程名）。
    void on_rtn_sysmon_config(const std::string& source, const nlohmann::json& payload) {
        if (auto error = dztrader::platform::SysmonConfig::validate(payload)) {
            SPDLOG_WARN("invalid sysmon config ignored | source={} error={}", source, *error);
            return;
        }
        // 契约 sysmon §5：两个 sysmon 域固定挂载实例 dzsysmon（帧头来源即该固定实例）。
        mirror_.update(std::string(kInstance), "sysmon_config", payload);
        ws_.broadcast("sysmon_config", std::string(kInstance), payload);
    }

private:
    MirrorStore& mirror_;
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_SYSMON_DOMAIN_SERVICE_H_
