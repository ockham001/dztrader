#include "sysmon_controller.h"

namespace dztrader::webui {

void SysmonCtrl::get_snapshot(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    (void)req;  // 认证由 JWT advice 处理，本端点所有已认证用户可读（契约 rest §2.8）
    const nlohmann::json instance = mirror_->instance(std::string(SysmonDomainService::kInstance));
    if (!instance.is_object() || !instance.contains("sysmon_snapshot")) {
        callback(error_response(drogon::k503ServiceUnavailable, "sysmon snapshot not available"));
        return;
    }
    callback(json_response(drogon::k200OK, instance["sysmon_snapshot"]));
}

}  // namespace dztrader::webui
