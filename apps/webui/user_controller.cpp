#include "user_controller.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <ctime>
#include <string>
#include "password_policy.h"
#include "ws_controller.h"

namespace dztrader::webui {

using Json = nlohmann::json;

namespace {

/// 口令强度不合格错误体（契约 rest §1「密码策略」）：机器码供前端分支，
/// error 是人类可读消息。文案不回显口令值。
Json password_too_weak_body() {
    return Json{{"error", "password too weak"},
                {"code", "password_too_weak"},
                {"min_length", kMinPasswordLength}};
}

Json old_password_invalid_body() {
    return Json{{"error", "invalid old password"}, {"code", "invalid_old_password"}};
}

}  // namespace

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
        callback(error_response(drogon::k404NotFound, "not found"));
        return;
    }
    callback(json_response(drogon::k200OK, user_to_json(*user)));
}

// ---------------------------------------------------------------------------
// GET /api/user/me - 当前登录用户自身信息（任意已认证用户）
// 角色降级后前端借此刷新已缓存角色（isAdmin 实时联动），与后端踢降级用户 WS 连接配套。
// JWT pre-handling 已向 attribute 注入 user_id（= 当前登录用户名，见 main.cpp）。
// ---------------------------------------------------------------------------
void UserCtrl::me(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!req->getAttributes()->find("user_id")) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    const std::string username = req->getAttributes()->get<std::string>("user_id");
    auto user = repo_->get_user_by_username(username);
    if (!user.has_value()) {
        callback(error_response(drogon::k404NotFound, "not found"));
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

    if (!is_password_acceptable(password)) {
        callback(json_response(drogon::k400BadRequest, password_too_weak_body()));
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
        callback(error_response(drogon::k404NotFound, "not found"));
        return;
    }
    const bool was_admin = before->role == "admin";

    if (!repo_->update_user(id, display_name, email, role)) {
        callback(error_response(drogon::k404NotFound, "not found"));
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
        callback(error_response(drogon::k404NotFound, "not found"));
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
        callback(error_response(drogon::k404NotFound, "not found"));
        return;
    }

    // 禁止禁用/锁定管理员：防止误操作导致系统无法管理
    if (target->role == "admin" && (status == "disabled" || status == "locked")) {
        callback(error_response(drogon::k403Forbidden, "cannot disable or lock admin user"));
        return;
    }

    if (status == "disabled") {
        repo_->update_user_status(id, "disabled");
        // 契约 rest §2.2：禁用时强制断开其 WS 连接并吊销既有 JWT（token_version 自增）
        repo_->revoke_user_tokens(id);
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
    if (!is_password_acceptable(new_password)) {
        callback(json_response(drogon::k400BadRequest, password_too_weak_body()));
        return;
    }

    // 契约 rest §2.2：管理员重置不需要旧密码，但会吊销该用户既有会话并置强制改密
    auto target = repo_->get_user_by_id(id);
    if (!target.has_value()) {
        callback(error_response(drogon::k404NotFound, "not found"));
        return;
    }
    if (!repo_->admin_reset_password(id, new_password)) {
        callback(error_response(drogon::k404NotFound, "not found"));
        return;
    }
    notifier_.kick_user(target->username);
    callback(json_response(drogon::k200OK, {{"ok", true}}));
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
        callback(error_response(drogon::k404NotFound, "not found"));
        return;
    }

    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }

    const std::string old_password = body.value("old_password", "");
    const std::string new_password = body.value("new_password", "");
    if (old_password.empty() || new_password.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing required fields"));
        return;
    }

    // 契约 rest §1「自助改密 vs 管理员重置」：自助改密必须校验旧密码
    auto hash = repo_->get_password_hash(username);
    if (!hash.has_value() || !verify_password(old_password, *hash)) {
        callback(json_response(drogon::k400BadRequest, old_password_invalid_body()));
        return;
    }

    if (!is_password_acceptable(new_password)) {
        callback(json_response(drogon::k400BadRequest, password_too_weak_body()));
        return;
    }

    // 契约 rest §2.2：改密吊销既有会话（含当前会话），并清除强制改密标志
    if (!repo_->change_own_password(user->id, new_password)) {
        callback(error_response(drogon::k500InternalServerError, "failed to change password"));
        return;
    }
    notifier_.kick_user(username);

    callback(json_response(drogon::k200OK, {{"ok", true}}));
}

}  // namespace dztrader::webui
