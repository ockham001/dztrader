#include "auth_controller.h"
#include "jwt.h"
#include "ws_controller.h"

#include <nlohmann/json.hpp>
#include <ctime>
#include <string>

namespace dztrader::webui {

using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// POST /api/login
// ---------------------------------------------------------------------------

void LoginCtrl::login(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    // 1. Parse body
    // 400 统一出口（契约 rest §1：请求体形状非法一律 400 {"error":"bad request"}）
    const auto reply_bad_request = [&callback]() {
        callback(json_response(drogon::k400BadRequest, Json{{"error", "bad request"}}));
    };
    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (const std::exception&) {
        reply_bad_request();
        return;
    }
    // 契约 rest §1: 请求体必须是含 string 类型 username/password 的 JSON 对象；
    // 合法 JSON 但非对象（如 []）或字段类型错（如数字）同样是 bad request（不得 500）
    if (!body.is_object()) {
        reply_bad_request();
        return;
    }
    const auto username_it = body.find("username");
    const auto password_it = body.find("password");
    if (username_it == body.end() || !username_it->is_string() || password_it == body.end() ||
        !password_it->is_string()) {
        reply_bad_request();
        return;
    }
    const std::string username = username_it->get<std::string>();
    const std::string password = password_it->get<std::string>();
    if (username.empty() || password.empty()) {
        reply_bad_request();
        return;
    }

    // 2. Query user
    auto user = repo_->get_user_by_username(username);
    if (!user.has_value()) {
        callback(error_response(drogon::k401Unauthorized, "invalid_credentials"));
        return;
    }

    // 3. Check account status
    const auto now_epoch = static_cast<int64_t>(std::time(nullptr));
    if (user->status == "disabled") {
        callback(error_response(drogon::k401Unauthorized, "account_disabled"));
        return;
    }
    if (user->status == "locked" || user->locked_until > now_epoch) {
        // 契约 rest §1: 登录错误体 locked_until 为绝对 Unix 秒级时间戳（与 User.locked_until
        // 同语义），前端按 locked_until - now 计算剩余分钟
        callback(json_response(drogon::k423Locked, Json{{"error", "account_locked"},
                                                        {"locked_until", user->locked_until}}));
        return;
    }

    // 4. Check IP ban
    const std::string client_ip = req->getPeerAddr().toIp();
    const std::string user_agent = req->getHeader("User-Agent");
    auto security = repo_->get_security_config();
    if (security.access_mode == "blacklist" && repo_->is_ip_blacklisted(client_ip)) {
        callback(error_response(drogon::k401Unauthorized, "ip_banned"));
        return;
    }
    if (security.access_mode == "whitelist" && !repo_->is_ip_whitelisted(client_ip)) {
        callback(error_response(drogon::k401Unauthorized, "ip_banned"));
        return;
    }

    // 5. Verify password
    auto hash = repo_->get_password_hash(username);
    if (!hash.has_value()) {
        // Should not happen since we already fetched the user, but guard anyway
        callback(error_response(drogon::k401Unauthorized, "invalid_credentials"));
        return;
    }
    if (!verify_password(password, *hash)) {
        // 6. Failure handling
        repo_->increment_failed_login(username);
        auto updated = repo_->get_user_by_username(username);
        auto config = repo_->get_security_config();
        if (updated && config.login_lockout_enabled &&
            updated->failed_login_count >= config.max_failed_attempts) {
            repo_->lock_user(updated->id, now_epoch + config.lockout_duration_sec);
            repo_->add_login_history(username, client_ip, "web", false, user_agent,
                                     "account_locked");
            notifier_.broadcast_data_changed("login_history");
            notifier_.broadcast_data_changed("users");
            // 契约 rest §1: locked_until 为绝对 Unix 秒级时间戳
            callback(
                json_response(drogon::k423Locked,
                              Json{{"error", "account_locked"},
                                   {"locked_until", now_epoch + config.lockout_duration_sec}}));
            return;
        }
        repo_->add_login_history(username, client_ip, "web", false, user_agent,
                                 "invalid_credentials");
        notifier_.broadcast_data_changed("login_history");
        callback(error_response(drogon::k401Unauthorized, "invalid_credentials"));
        return;
    }

    // 7. Success
    repo_->reset_failed_login(username);
    repo_->update_last_login(user->id, client_ip);
    repo_->add_login_history(username, client_ip, "web", true, user_agent);
    notifier_.broadcast_data_changed("login_history");
    notifier_.broadcast_data_changed("users");

    std::string token = jwt_sign(username, cfg_->token_ttl_sec, cfg_->jwt_secret);
    // Re-fetch the user so the response reflects the updated last_login fields
    auto refreshed = repo_->get_user_by_username(username);
    Json user_json = user_to_json(refreshed ? *refreshed : *user);
    // 默认密码告警标志:仅 admin 用户、当前密码是默认密码、且未确认告警时返回 true
    bool is_default_password = cfg_->admin_password_is_default && refreshed &&
                               refreshed->role == "admin" &&
                               refreshed->default_password_acknowledged == 0;
    callback(json_response(drogon::k200OK, Json{{"token", token},
                                                {"expires_in", cfg_->token_ttl_sec},
                                                {"user", user_json},
                                                {"is_default_password", is_default_password}}));
}

}  // namespace dztrader::webui
