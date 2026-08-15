#include "security_controller.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include "ws_controller.h"

namespace dztrader::webui {

using Json = nlohmann::json;

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

void SecurityCtrl::get_config(const drogon::HttpRequestPtr& req,
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

void SecurityCtrl::set_config(const drogon::HttpRequestPtr& req,
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

    SecurityConfig config;
    config.login_lockout_enabled = body.value("login_lockout_enabled", true);
    config.access_mode = body.value("access_mode", "auto");
    config.max_failed_attempts = body.value("max_failed_attempts", 5);
    config.lockout_duration_sec = body.value("lockout_duration_sec", 900);

    if (config.access_mode != "auto" && config.access_mode != "whitelist" &&
        config.access_mode != "blacklist") {
        callback(error_response(drogon::k400BadRequest, "invalid access_mode"));
        return;
    }

    if (!repo_->set_security_config(config)) {
        callback(error_response(drogon::k500InternalServerError,
                                "failed to update security config"));
        return;
    }

    const Json resp = {
        {"login_lockout_enabled", config.login_lockout_enabled},
        {"access_mode", config.access_mode},
        {"max_failed_attempts", config.max_failed_attempts},
        {"lockout_duration_sec", config.lockout_duration_sec},
    };
    if (g_broadcast_data_changed) {
        g_broadcast_data_changed("security_config");
    }
    callback(json_response(drogon::k200OK, resp));
}

// ---------------------------------------------------------------------------
// GET /api/security/blacklist
// ---------------------------------------------------------------------------

void SecurityCtrl::list_blacklist(const drogon::HttpRequestPtr& req,
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

void SecurityCtrl::add_blacklist(const drogon::HttpRequestPtr& req,
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
            if (g_broadcast_data_changed) {
                g_broadcast_data_changed("blacklist");
            }
            callback(json_response(drogon::k201Created, ip_entry_to_json(e)));
            return;
        }
    }
    if (g_broadcast_data_changed) {
        g_broadcast_data_changed("blacklist");
    }
    callback(json_response(drogon::k201Created, Json{{"ip", ip}, {"reason", reason}, {"source", "manual"}}));
}

// ---------------------------------------------------------------------------
// DELETE /api/security/blacklist/{id}
// ---------------------------------------------------------------------------

void SecurityCtrl::remove_blacklist(const drogon::HttpRequestPtr& req,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                                    int64_t id) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    if (!repo_->remove_from_blacklist(id)) {
        callback(error_response(drogon::k404NotFound, "entry not found"));
        return;
    }
    if (g_broadcast_data_changed) {
        g_broadcast_data_changed("blacklist");
    }
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    callback(resp);
}

// ---------------------------------------------------------------------------
// GET /api/security/whitelist
// ---------------------------------------------------------------------------

void SecurityCtrl::list_whitelist(const drogon::HttpRequestPtr& req,
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

void SecurityCtrl::add_whitelist(const drogon::HttpRequestPtr& req,
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
            if (g_broadcast_data_changed) {
                g_broadcast_data_changed("whitelist");
            }
            callback(json_response(drogon::k201Created, ip_entry_to_json(e)));
            return;
        }
    }
    if (g_broadcast_data_changed) {
        g_broadcast_data_changed("whitelist");
    }
    callback(json_response(drogon::k201Created, Json{{"ip", ip}, {"reason", reason}}));
}

// ---------------------------------------------------------------------------
// DELETE /api/security/whitelist/{id}
// ---------------------------------------------------------------------------

void SecurityCtrl::remove_whitelist(const drogon::HttpRequestPtr& req,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                                    int64_t id) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    if (!repo_->remove_from_whitelist(id)) {
        callback(error_response(drogon::k404NotFound, "entry not found"));
        return;
    }
    if (g_broadcast_data_changed) {
        g_broadcast_data_changed("whitelist");
    }
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    callback(resp);
}

// ---------------------------------------------------------------------------
// GET /api/security/login-history?page=&page_size=&days=&start=&end=
// ---------------------------------------------------------------------------

void SecurityCtrl::login_history(const drogon::HttpRequestPtr& req,
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

void SecurityCtrl::ack_default_password(const drogon::HttpRequestPtr& req,
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
        callback(error_response(drogon::k404NotFound, "user_not_found"));
        return;
    }

    if (!repo_->ack_default_password(user->id)) {
        callback(error_response(drogon::k500InternalServerError, "update_failed"));
        return;
    }
    callback(json_response(drogon::k200OK, nlohmann::json{{"ok", true}}));
}

}  // namespace dztrader::webui
