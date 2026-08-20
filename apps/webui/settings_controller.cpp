#include "settings_controller.h"

#include <dztrader/core/json_section.h>
#include <spdlog/spdlog.h>

#include <fstream>

namespace dztrader::webui {

using Json = nlohmann::json;

// PUT /api/settings/event-shm-config
// 契约 shm: 直发 SET_EVENT_SHM_CONFIG 帧 (无 instance_id, RFC 7386 合并 patch 整体透传),
// HTTP 同步响应仅表示"已受理", 最终状态由 WS event_shm_config 推送决定
void SettingsCtrl::set_event_shm_config(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    if (!shm_writer_ || !shm_writer_->is_ready()) {
        callback(error_response(drogon::k503ServiceUnavailable, "shm not available"));
        return;
    }
    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    if (!body.is_object()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    // 整体透传, 字段校验由 master EventShmConfig 负责 (失败经日志/NOTIFY_UI 呈现)
    shm_writer_->write_set_event_shm_config(body);
    callback(json_response(drogon::k200OK, {{"ok", true}, {"message", "dispatched"}}));
}

// 下列 3 个方法为 Task 2 期的 stub（保证本 Task 独立可链接、可测），
// Task 3 实现 get_webui/set_webui、Task 4 实现 get_master 时整体替换。
void SettingsCtrl::get_master(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    callback(json_response(drogon::k200OK, Json::object()));
}

void SettingsCtrl::get_webui(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    callback(json_response(drogon::k200OK, Json::object()));
}

void SettingsCtrl::set_webui(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    callback(json_response(drogon::k200OK, {{"ok", true}}));
}

}  // namespace dztrader::webui
