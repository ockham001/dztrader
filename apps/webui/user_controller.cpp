#include "user_controller.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <ctime>
#include <string>
#include "ws_controller.h"

namespace dztrader::webui {

using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Json UserCtrl::permission_to_json(const Permission& p) {
    return {
        {"type", p.type},
        {"id", p.id},
        {"granted", p.granted},
    };
}

// ---------------------------------------------------------------------------
// GET /api/user - list users with filters and pagination
// ---------------------------------------------------------------------------

void UserCtrl::list(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }

    auto params = req->getParameters();
    const std::string search = params.contains("search") ? params.at("search") : "";
    const std::string role = params.contains("role") ? params.at("role") : "";
    const std::string status = params.contains("status") ? params.at("status") : "";

    int page = 1;
    int page_size = 20;
    try {
        if (params.contains("page")) {
            page = std::stoi(params.at("page"));
        }
        if (params.contains("page_size")) {
            page_size = std::stoi(params.at("page_size"));
        }
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "invalid pagination parameters"));
        return;
    }
    page = (std::max)(page, 1);
    if (page_size < 1) {
        page_size = 20;
    }

    auto result = repo_->list_users(search, role, status, page, page_size);
    Json users_json = Json::array();
    for (const auto& u : result.users) {
        users_json.push_back(user_to_json(u));
    }
    callback(json_response(drogon::k200OK, {{"users", users_json}, {"total", result.total}}));
}

// ---------------------------------------------------------------------------
// GET /api/user/{id} - get user details
// ---------------------------------------------------------------------------

void UserCtrl::get(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                   int64_t id) {
    if (!is_admin(req)) {
        // Non-admin users can only view their own profile.
        if (!req->getAttributes()->find("user_id")) {
            callback(error_response(drogon::k403Forbidden, "forbidden"));
            return;
        }
        const std::string current_username = req->getAttributes()->get<std::string>("user_id");
        auto current = repo_->get_user_by_username(current_username);
        if (!current.has_value() || current->id != id) {
            callback(error_response(drogon::k403Forbidden, "forbidden"));
            return;
        }
    }

    auto user = repo_->get_user_by_id(id);
    if (!user.has_value()) {
        callback(error_response(drogon::k404NotFound, "user not found"));
        return;
    }
    callback(json_response(drogon::k200OK, user_to_json(*user)));
}

// ---------------------------------------------------------------------------
// POST /api/user - create user (admin only)
// ---------------------------------------------------------------------------

void UserCtrl::create(const drogon::HttpRequestPtr& req,
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

    const std::string username = body.value("username", "");
    const std::string display_name = body.value("display_name", "");
    const std::string email = body.value("email", "");
    const std::string password = body.value("password", "");
    const std::string role = body.value("role", "user");

    if (username.empty() || password.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing required fields"));
        return;
    }

    if (repo_->get_user_by_username(username).has_value()) {
        callback(error_response(drogon::k409Conflict, "username already exists"));
        return;
    }

    int64_t const id = repo_->create_user(username, display_name, email, password, role);
    auto user = repo_->get_user_by_id(id);
    notifier_.broadcast_data_changed("users");
    callback(json_response(drogon::k201Created, user_to_json(*user)));
}

// ---------------------------------------------------------------------------
// PUT /api/user/{id} - update user (admin only)
// ---------------------------------------------------------------------------

void UserCtrl::update(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                      int64_t id) {
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

    const std::string display_name = body.value("display_name", "");
    const std::string email = body.value("email", "");
    const std::string role = body.value("role", "user");

    // 契约 webui-ws §2.3：admin 被降级为新非 admin 时，须强制断开其已建 WS 连接
    // （Session.is_admin 连接时缓存，降权后旧连接仍可绕过 md_connect 等 admin 预检）。
    auto before = repo_->get_user_by_id(id);
    if (!before.has_value()) {
        callback(error_response(drogon::k404NotFound, "user not found"));
        return;
    }
    const bool was_admin = before->role == "admin";

    if (!repo_->update_user(id, display_name, email, role)) {
        callback(error_response(drogon::k404NotFound, "user not found"));
        return;
    }

    if (was_admin && role != "admin") {
        notifier_.kick_user(before->username);
    }

    auto user = repo_->get_user_by_id(id);
    notifier_.broadcast_data_changed("users");
    callback(json_response(drogon::k200OK, user_to_json(*user)));
}

// ---------------------------------------------------------------------------
// DELETE /api/user/{id} - delete user (admin only)
// ---------------------------------------------------------------------------

void UserCtrl::remove(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                      int64_t id) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }

    // 查目标用户：用于自删检测、admin 保护、删后踢人
    auto target = repo_->get_user_by_id(id);
    if (!target.has_value()) {
        callback(error_response(drogon::k404NotFound, "user not found"));
        return;
    }

    // 禁止删除自己：当前登录用户 username 来自 JWT attribute
    std::string current_username;
    if (req->getAttributes()->find("user_id")) {
        current_username = req->getAttributes()->get<std::string>("user_id");
    }
    if (!current_username.empty() && target->username == current_username) {
        callback(error_response(drogon::k403Forbidden, "cannot delete yourself"));
        return;
    }

    // 禁止删除管理员：当前系统仅支持单一超级管理员，删除将导致无法管理
    if (target->role == "admin") {
        callback(error_response(drogon::k403Forbidden, "cannot delete admin user"));
        return;
    }

    if (!repo_->delete_user(id)) {
        callback(error_response(drogon::k500InternalServerError, "delete failed"));
        return;
    }

    // 强制断开被删用户的所有 WS 连接
    notifier_.kick_user(target->username);

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    notifier_.broadcast_data_changed("users");
    callback(resp);
}

// ---------------------------------------------------------------------------
// PUT /api/user/{id}/status - update status (admin only)
// ---------------------------------------------------------------------------

void UserCtrl::update_status(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
    int64_t id) {
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

    const std::string status = body.value("status", "");

    if (status != "disabled" && status != "enabled" && status != "locked" && status != "unlocked") {
        callback(error_response(drogon::k400BadRequest, "invalid status"));
        return;
    }

    auto target = repo_->get_user_by_id(id);
    if (!target.has_value()) {
        callback(error_response(drogon::k404NotFound, "user not found"));
        return;
    }

    // 禁止禁用/锁定管理员：防止误操作导致系统无法管理
    if (target->role == "admin" && (status == "disabled" || status == "locked")) {
        callback(error_response(drogon::k403Forbidden, "cannot disable or lock admin user"));
        return;
    }

    if (status == "disabled") {
        repo_->update_user_status(id, "disabled");
        // 强制断开被禁用用户的所有 WS 连接（契约 rest §2.2：禁用时强制断开）
        notifier_.kick_user(target->username);
    } else if (status == "enabled") {
        repo_->update_user_status(id, "offline");
    } else if (status == "locked") {
        auto config = repo_->get_security_config();
        const auto now = static_cast<int64_t>(std::time(nullptr));
        repo_->lock_user(id, now + config.lockout_duration_sec);
    } else if (status == "unlocked") {
        auto user = repo_->get_user_by_id(id);
        repo_->reset_failed_login(user->username);
        repo_->lock_user(id, 0);
        repo_->update_user_status(id, "offline");
    }

    auto user = repo_->get_user_by_id(id);
    notifier_.broadcast_data_changed("users");
    callback(json_response(drogon::k200OK, user_to_json(*user)));
}

// ---------------------------------------------------------------------------
// PUT /api/user/{id}/password - reset password (admin only)
// ---------------------------------------------------------------------------

void UserCtrl::reset_password(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
    int64_t id) {
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

    const std::string new_password = body.value("new_password", "");
    if (new_password.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing new_password"));
        return;
    }

    if (!repo_->reset_password(id, new_password)) {
        callback(error_response(drogon::k404NotFound, "user not found"));
        return;
    }

    callback(json_response(drogon::k200OK, {{"ok", true}}));
}

// ---------------------------------------------------------------------------
// GET /api/user/{id}/permissions - get user permissions
// ---------------------------------------------------------------------------

void UserCtrl::get_permissions(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
    int64_t id) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }

    if (!repo_->get_user_by_id(id).has_value()) {
        callback(error_response(drogon::k404NotFound, "user not found"));
        return;
    }
    auto perms = repo_->get_user_permissions(id);
    Json arr = Json::array();
    for (const auto& p : perms) {
        arr.push_back(permission_to_json(p));
    }
    callback(json_response(drogon::k200OK, arr));
}

// ---------------------------------------------------------------------------
// PUT /api/user/{id}/permissions - update permissions (admin only)
// ---------------------------------------------------------------------------

void UserCtrl::update_permissions(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
    int64_t id) {
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

    if (!body.contains("permissions") || !body["permissions"].is_array()) {
        callback(error_response(drogon::k400BadRequest, "missing permissions array"));
        return;
    }

    if (!repo_->get_user_by_id(id).has_value()) {
        callback(error_response(drogon::k404NotFound, "user not found"));
        return;
    }

    for (const auto& perm : body["permissions"]) {
        std::string type = perm.value("type", "");
        std::string perm_id = perm.value("id", "");
        const bool granted = perm.value("granted", false);
        if (type.empty() || perm_id.empty()) {
            continue;
        }
        if (!repo_->set_user_permission(id, type, perm_id, granted)) {
            SPDLOG_WARN("set_user_permission failed | user={} type={} id={}", id, type, perm_id);
            callback(
                error_response(drogon::k500InternalServerError, "failed to set user permission"));
            return;
        }
    }

    auto perms = repo_->get_user_permissions(id);
    Json arr = Json::array();
    for (const auto& p : perms) {
        arr.push_back(permission_to_json(p));
    }
    notifier_.broadcast_data_changed("users");
    callback(json_response(drogon::k200OK, arr));
}

// ---------------------------------------------------------------------------
// POST /api/auth/change-password - change own password (any authenticated user)
// ---------------------------------------------------------------------------

void UserCtrl::change_password(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    // Resolve the current user from the JWT attribute (stores username).
    if (!req->getAttributes()->find("user_id")) {
        callback(error_response(drogon::k401Unauthorized, "unauthorized"));
        return;
    }
    const std::string username = req->getAttributes()->get<std::string>("user_id");
    auto user = repo_->get_user_by_username(username);
    if (!user.has_value()) {
        callback(error_response(drogon::k404NotFound, "user not found"));
        return;
    }

    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }

    const std::string new_password = body.value("new_password", "");
    if (new_password.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing new_password"));
        return;
    }

    if (!repo_->reset_password(user->id, new_password)) {
        callback(error_response(drogon::k500InternalServerError, "failed to change password"));
        return;
    }

    callback(json_response(drogon::k200OK, {{"ok", true}}));
}

}  // namespace dztrader::webui
