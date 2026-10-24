#include "sysmon_controller.h"

#include <dztrader/sysmonstore/history_store.h>

#include <exception>
#include <string>
#include <string_view>

namespace dztrader::webui {

void SysmonCtrl::get_snapshot(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    (void)req;  // 认证由 JWT advice 处理，本端点所有已认证用户可读（《帧契约：REST》§2.8）
    const nlohmann::json instance = mirror_->instance(std::string(SysmonDomainService::kInstance));
    if (!instance.is_object() || !instance.contains("sysmon_snapshot")) {
        callback(error_response(drogon::k503ServiceUnavailable, "sysmon snapshot not available"));
        return;
    }
    callback(json_response(drogon::k200OK, instance["sysmon_snapshot"]));
}

void SysmonCtrl::get_history(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    // 认证由 JWT advice 处理，本端点所有已认证用户可读（《帧契约：REST》§2.8）。
    // 历史库路径在构造时注入（生产为 $DZTRADER_HOME/db/sysmon.db）；未注入时按「历史不可用」
    // 降级，而不是去猜一个路径读错文件。
    if (history_db_path_.empty()) {
        // reason 走共享模块的稳定 ASCII
        // 码映射（《帧契约：系统监控》§6.1.3），不在此另写一份字面量。
        callback(json_response(
            drogon::k200OK,
            {{"available", false},
             {"reason", std::string(dztrader::sysmonstore::history_availability_reason(
                            dztrader::sysmonstore::HistoryAvailability::NotFound))}}));
        return;
    }
    SysmonHistoryService service(history_db_path_);
    const auto parsed =
        service.parse([&req](std::string_view key) { return req->getParameter(std::string(key)); });
    if (!parsed.query.has_value()) {
        // 《帧契约：系统监控》§6.1：参数非法一律 400，不做静默钳制。
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    callback(json_response(drogon::k200OK, service.query(*parsed.query)));
}

void SysmonCtrl::set_config(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    // 《帧契约：REST》§2.8：写类端点需 admin（服务端校验，不是前端隐藏）。
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "admin required"));
        return;
    }
    if (!shm_writer_ || !shm_writer_->is_ready()) {
        callback(error_response(drogon::k503ServiceUnavailable, "shm not available"));
        return;
    }
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req->getBody());
    } catch (const std::exception&) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    if (!body.is_object()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    // 整体透传 merge patch：字段校验（范围/类型/任意位置 null）由 dzsysmon 的
    // SysmonConfig::apply_patch 负责，失败按契约 sysmon §3.3 的四件套表达
    // （回滚旧值 + 日志 + NOTIFY_UI + 回推旧值）；此处的 200 只表示已写事件通道。
    shm_writer_->write_set_sysmon_config(body);
    callback(json_response(drogon::k200OK, {{"ok", true}, {"message", "dispatched"}}));
}

}  // namespace dztrader::webui
