#include "security_controller.h"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include "ws_controller.h"

namespace dztrader::webui {

using Json = nlohmann::json;

namespace {

/// 读取可选整型字段: 缺失返回 true 且不改 out; 类型不符/超出 int 范围返回 false。
/// 显式分支（而非 get<int>() 直接转换）避免 uint64 溢出静默回绕或 out_of_range 逃逸。
bool read_optional_int(const nlohmann::json& body, const char* key, int& out) {
    if (!body.contains(key)) {
        return true;
    }
    const auto& v = body[key];
    int64_t value = 0;
    if (v.is_number_unsigned()) {
        const auto uv = v.get<uint64_t>();
        if (uv > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        value = static_cast<int64_t>(uv);
    } else if (v.is_number_integer()) {
        value = v.get<int64_t>();
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
            return false;
        }
    } else {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Json SecurityCtrl::ip_entry_to_json(const IpEntry& e) {
    return {
        {"id", e.id},
        {"ip", e.ip},
        {"reason", e.reason},
        {"source", e.source},
        {"created_at", utc_to_local(e.created_at)},
    };
}

Json SecurityCtrl::login_history_to_json(const LoginHistoryEntry& h) {
    return {
        {"id", h.id},
        {"username", h.username},
        {"ip", h.ip},
        {"device", h.device},
        {"success", h.success},
        {"created_at", utc_to_local(h.created_at)},
        {"user_agent", h.user_agent},
        {"reason", h.reason},
    };
}

// ---------------------------------------------------------------------------
// GET /api/security/config
// ---------------------------------------------------------------------------

void SecurityCtrl::get_config(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    auto config = repo_->get_security_config();
    const Json body = {
        {"login_lockout_enabled", config.login_lockout_enabled},
        {"access_mode", config.access_mode},
        {"max_failed_attempts", config.max_failed_attempts},
        {"lockout_duration_sec", config.lockout_duration_sec},
    };
    callback(json_response(drogon::k200OK, body));
}

// ---------------------------------------------------------------------------
// PUT /api/security/config
// ---------------------------------------------------------------------------

void SecurityCtrl::set_config(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
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

    // 字段类型/范围防御: body.value(...) 在类型不符时抛 type_error 逃逸为 500;
    // 显式检查后统一回 400（整型溢出同样拒绝，不静默回绕）
    SecurityConfig config;
    if (body.contains("login_lockout_enabled")) {
        if (!body["login_lockout_enabled"].is_boolean()) {
            callback(error_response(drogon::k400BadRequest, "invalid login_lockout_enabled"));
            return;
        }
        config.login_lockout_enabled = body["login_lockout_enabled"].get<bool>();
    }
    if (body.contains("access_mode")) {
        if (!body["access_mode"].is_string()) {
            callback(error_response(drogon::k400BadRequest, "invalid access_mode"));
            return;
        }
        config.access_mode = body["access_mode"].get<std::string>();
    } else {
        config.access_mode = "auto";
    }
    if (!read_optional_int(body, "max_failed_attempts", config.max_failed_attempts)) {
        callback(error_response(drogon::k400BadRequest, "invalid max_failed_attempts"));
        return;
    }
    if (!read_optional_int(body, "lockout_duration_sec", config.lockout_duration_sec)) {
        callback(error_response(drogon::k400BadRequest, "invalid lockout_duration_sec"));
        return;
    }

    if (config.access_mode != "auto" && config.access_mode != "whitelist" &&
        config.access_mode != "blacklist") {
        callback(error_response(drogon::k400BadRequest, "invalid access_mode"));
        return;
    }

    if (!repo_->set_security_config(config)) {
        callback(
            error_response(drogon::k500InternalServerError, "failed to update security config"));
        return;
    }

    const Json resp = {
        {"login_lockout_enabled", config.login_lockout_enabled},
        {"access_mode", config.access_mode},
        {"max_failed_attempts", config.max_failed_attempts},
        {"lockout_duration_sec", config.lockout_duration_sec},
    };
    notifier_.broadcast_data_changed("security_config");
    callback(json_response(drogon::k200OK, resp));
}

// ---------------------------------------------------------------------------
// GET /api/security/blacklist
// ---------------------------------------------------------------------------

void SecurityCtrl::list_blacklist(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    auto entries = repo_->list_blacklist();
    Json arr = Json::array();
    for (const auto& e : entries) {
        arr.push_back(ip_entry_to_json(e));
    }
    callback(json_response(drogon::k200OK, arr));
}

// ---------------------------------------------------------------------------
// POST /api/security/blacklist
// ---------------------------------------------------------------------------

void SecurityCtrl::add_blacklist(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }

    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }

    std::string ip = body.value("ip", "");
    std::string reason = body.value("reason", "");
    if (ip.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing ip"));
        return;
    }

    if (!repo_->add_to_blacklist(ip, reason, "manual")) {
        callback(error_response(drogon::k409Conflict, "ip already blacklisted"));
        return;
    }

    // Return the newly-added entry by listing and finding the matching ip.
    auto entries = repo_->list_blacklist();
    for (const auto& e : entries) {
        if (e.ip == ip) {
            notifier_.broadcast_data_changed("blacklist");
            callback(json_response(drogon::k201Created, ip_entry_to_json(e)));
            return;
        }
    }
    notifier_.broadcast_data_changed("blacklist");
    callback(json_response(drogon::k201Created,
                           Json{{"ip", ip}, {"reason", reason}, {"source", "manual"}}));
}

// ---------------------------------------------------------------------------
// DELETE /api/security/blacklist/{id}
// ---------------------------------------------------------------------------

void SecurityCtrl::remove_blacklist(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
    int64_t id) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    if (!repo_->remove_from_blacklist(id)) {
        callback(error_response(drogon::k404NotFound, "not found"));
        return;
    }
    notifier_.broadcast_data_changed("blacklist");
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    callback(resp);
}

// ---------------------------------------------------------------------------
// GET /api/security/whitelist
// ---------------------------------------------------------------------------

void SecurityCtrl::list_whitelist(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    auto entries = repo_->list_whitelist();
    Json arr = Json::array();
    for (const auto& e : entries) {
        arr.push_back(ip_entry_to_json(e));
    }
    callback(json_response(drogon::k200OK, arr));
}

// ---------------------------------------------------------------------------
// POST /api/security/whitelist
// ---------------------------------------------------------------------------

void SecurityCtrl::add_whitelist(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }

    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }

    std::string ip = body.value("ip", "");
    std::string reason = body.value("reason", "");
    if (ip.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing ip"));
        return;
    }

    if (!repo_->add_to_whitelist(ip, reason)) {
        callback(error_response(drogon::k409Conflict, "ip already whitelisted"));
        return;
    }

    auto entries = repo_->list_whitelist();
    for (const auto& e : entries) {
        if (e.ip == ip) {
            notifier_.broadcast_data_changed("whitelist");
            callback(json_response(drogon::k201Created, ip_entry_to_json(e)));
            return;
        }
    }
    notifier_.broadcast_data_changed("whitelist");
    callback(json_response(drogon::k201Created, Json{{"ip", ip}, {"reason", reason}}));
}

// ---------------------------------------------------------------------------
// DELETE /api/security/whitelist/{id}
// ---------------------------------------------------------------------------

void SecurityCtrl::remove_whitelist(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
    int64_t id) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    if (!repo_->remove_from_whitelist(id)) {
        callback(error_response(drogon::k404NotFound, "not found"));
        return;
    }
    notifier_.broadcast_data_changed("whitelist");
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    callback(resp);
}

// ---------------------------------------------------------------------------
// GET /api/security/login-history?page=&page_size=&days=&start=&end=
// ---------------------------------------------------------------------------

void SecurityCtrl::login_history(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }

    auto params = req->getParameters();
    int page = 1;
    int page_size = 20;
    int days = 1;
    try {
        if (params.contains("page")) {
            page = std::stoi(params.at("page"));
        }
        if (params.contains("page_size")) {
            page_size = std::stoi(params.at("page_size"));
        }
        if (params.contains("days")) {
            days = std::stoi(params.at("days"));
        }
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "invalid pagination parameters"));
        return;
    }
    page = (std::max)(page, 1);
    if (page_size < 1) {
        page_size = 20;
    }
    days = (std::max)(days, 1);

    const std::string start = params.contains("start") ? params.at("start") : "";
    const std::string end = params.contains("end") ? params.at("end") : "";

    auto result = repo_->get_login_history(page, page_size, days, start, end);
    Json arr = Json::array();
    for (const auto& h : result.entries) {
        arr.push_back(login_history_to_json(h));
    }
    callback(json_response(drogon::k200OK, {{"entries", arr}, {"total", result.total}}));
}

// ---------------------------------------------------------------------------
// POST /api/security/ack-default-password
// ---------------------------------------------------------------------------

void SecurityCtrl::ack_default_password(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    // 仅 admin 可调用
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "admin_only"));
        return;
    }

    // 从 JWT advice 设置的 attribute 取 username,查 user 拿 id
    auto user_id_attr = req->getAttributes()->find("user_id");
    if (!user_id_attr) {
        callback(error_response(drogon::k401Unauthorized, "no_user"));
        return;
    }
    const std::string username = req->getAttributes()->get<std::string>("user_id");
    auto user = repo_->get_user_by_username(username);
    if (!user.has_value()) {
        callback(error_response(drogon::k404NotFound, "not found"));
        return;
    }

    if (!repo_->ack_default_password(user->id)) {
        callback(error_response(drogon::k500InternalServerError, "update_failed"));
        return;
    }
    callback(json_response(drogon::k200OK, nlohmann::json{{"ok", true}}));
}

}  // namespace dztrader::webui
