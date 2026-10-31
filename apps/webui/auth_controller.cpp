#include "auth_controller.h"
#include "jwt.h"
#include "ws_controller.h"

#include <nlohmann/json.hpp>
#include <ctime>
#include <string>

namespace dztrader::webui {

using Json = nlohmann::json;

namespace {

/// 登录失败错误体（契约 rest §1/§2.1）：{"error": <human message>, "code": <machine code>}。
/// `error` 全站唯一语义是人类可读消息（小写英文、无句号），机器码只出现在 `code`——
/// 前端按 `code` 分支映射文案，绝不解析或回显 `error`。
Json login_error_body(const std::string& code, const std::string& message) {
    return Json{{"error", message}, {"code", code}};
}

/// 账户锁定错误体：login_error_body + `locked_until`（**绝对 Unix 秒级时间戳**，契约 rest §1）。
/// 两个锁定站点（已锁定 / 失败次数达阈值触发锁定）共用，避免形状漂移。
Json account_locked_body(int64_t locked_until) {
    Json body = login_error_body("account_locked", "account locked");
    body["locked_until"] = locked_until;
    return body;
}

}  // namespace

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
        callback(json_response(drogon::k401Unauthorized,
                               login_error_body("invalid_credentials", "invalid credentials")));
        return;
    }

    // 3. Check account status
    const auto now_epoch = static_cast<int64_t>(std::time(nullptr));
    if (user->status == "disabled") {
        callback(json_response(drogon::k401Unauthorized,
                               login_error_body("account_disabled", "account disabled")));
        return;
    }
    if (user->status == "locked" || user->locked_until > now_epoch) {
        // 契约 rest §1: 登录错误体 locked_until 为绝对 Unix 秒级时间戳（与 User.locked_until
        // 同语义），前端按 locked_until - now 计算剩余分钟
        callback(json_response(drogon::k423Locked, account_locked_body(user->locked_until)));
        return;
    }

    // 4. Check IP ban
    const std::string client_ip = req->getPeerAddr().toIp();
    const std::string user_agent = req->getHeader("User-Agent");
    auto security = repo_->get_security_config();
    if (security.access_mode == "blacklist" && repo_->is_ip_blacklisted(client_ip)) {
        callback(
            json_response(drogon::k401Unauthorized, login_error_body("ip_banned", "ip banned")));
        return;
    }
    if (security.access_mode == "whitelist" && !repo_->is_ip_whitelisted(client_ip)) {
        callback(
            json_response(drogon::k401Unauthorized, login_error_body("ip_banned", "ip banned")));
        return;
    }

    // 5. Verify password
    auto hash = repo_->get_password_hash(username);
    if (!hash.has_value()) {
        // Should not happen since we already fetched the user, but guard anyway
        callback(json_response(drogon::k401Unauthorized,
                               login_error_body("invalid_credentials", "invalid credentials")));
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
            callback(json_response(drogon::k423Locked,
                                   account_locked_body(now_epoch + config.lockout_duration_sec)));
            return;
        }
        repo_->add_login_history(username, client_ip, "web", false, user_agent,
                                 "invalid_credentials");
        notifier_.broadcast_data_changed("login_history");
        callback(json_response(drogon::k401Unauthorized,
                               login_error_body("invalid_credentials", "invalid credentials")));
        return;
    }

    // 7. Success
    repo_->reset_failed_login(username);
    repo_->update_last_login(user->id, client_ip);
    repo_->add_login_history(username, client_ip, "web", true, user_agent);
    notifier_.broadcast_data_changed("login_history");
    notifier_.broadcast_data_changed("users");

    std::string token =
        jwt_sign(username, cfg_->token_ttl_sec, cfg_->jwt_secret, user->token_version);
    // Re-fetch the user so the response reflects the updated last_login fields
    auto refreshed = repo_->get_user_by_username(username);
    Json user_json = user_to_json(refreshed ? *refreshed : *user);
    // 默认密码告警：文案唯一真相源是后端常量，前端直接展示（契约 rest §2.1 / webui-ws §2.3）。
    // 判定看**当前口令是否仍是系统默认口令**，而不是只看配置缺省标志——admin_password_is_default
    // 只说明配置文件没写 password，用户改密后它依然是 true；只看它会让告警在改密后继续谎报。
    const bool is_default_password = cfg_->admin_password_is_default && refreshed &&
                                     refreshed->role == "admin" &&
                                     refreshed->default_password_acknowledged == 0 &&
                                     repo_->password_matches(username, kDefaultAdminPassword);
    Json default_password_warning = nullptr;
    if (is_default_password) {
        default_password_warning = Json{{"message", kDefaultPasswordWarningMessage}};
    }
    // 契约 rest §1「强制改密」：被管理员重置过口令的账户登录后只能改密
    const bool must_change = refreshed ? refreshed->must_change_password != 0 : false;
    callback(
        json_response(drogon::k200OK, Json{{"token", token},
                                           {"expires_in", cfg_->token_ttl_sec},
                                           {"user", user_json},
                                           {"default_password_warning", default_password_warning},
                                           {"must_change_password", must_change}}));
}

}  // namespace dztrader::webui
