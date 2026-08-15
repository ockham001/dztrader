#ifndef DZTRADER_WEBUI_CONTROLLER_BASE_H_
#define DZTRADER_WEBUI_CONTROLLER_BASE_H_

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include "repository.h"

namespace dztrader::webui {

/// Common base for all webui controllers. Provides shared helpers that were
/// previously duplicated across LoginCtrl, UserCtrl, SecurityCtrl and
/// MarketSourceCtrl.
class ControllerBase {
protected:
    std::shared_ptr<Repository> repo_;

    explicit ControllerBase(std::shared_ptr<Repository> repo)
        : repo_(std::move(repo)) {}

    /// Returns true if the authenticated user (set by the JWT advice) is an admin.
    bool is_admin(const drogon::HttpRequestPtr& req) {
        if (!req->getAttributes()->find("user_id")) return false;
        std::string user_id = req->getAttributes()->get<std::string>("user_id");
        if (user_id.empty()) return false;
        try {
            auto user = repo_->get_user_by_username(user_id);
            return user.has_value() && user->role == "admin";
        } catch (...) {
            return false;
        }
    }

    /// Build a JSON HTTP response.
    static drogon::HttpResponsePtr json_response(drogon::HttpStatusCode code,
                                                 const nlohmann::json& body) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(code);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        return resp;
    }

    /// Build a JSON error response: {"error": message}.
    static drogon::HttpResponsePtr error_response(drogon::HttpStatusCode code,
                                                  const std::string& message) {
        return json_response(code, nlohmann::json{{"error", message}});
    }

    /// 将 UTC ISO 字符串（如 "2026-07-13T02:30:00Z"）转为服务器本地时间字符串
    /// （如 "2026-07-13 10:30:00"）。前端直接显示，不做时区转换。
    /// 解析失败时原样返回。
    static std::string utc_to_local(const std::string& utc_iso) {
        if (utc_iso.empty()) return utc_iso;
        std::tm tm{};
        std::istringstream ss(utc_iso);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        if (ss.fail()) return utc_iso;
#ifdef _WIN32
        time_t t_utc = _mkgmtime(&tm);
#else
        time_t t_utc = timegm(&tm);
#endif
        if (t_utc == static_cast<time_t>(-1)) return utc_iso;
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &t_utc);
#else
        localtime_r(&t_utc, &local);
#endif
        std::ostringstream out;
        out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
        return out.str();
    }

    /// Serialize a User to JSON (without password_hash).
    /// 时间字段转为服务器本地时间字符串，前端直接显示。
    static nlohmann::json user_to_json(const User& u) {
        return {
            {"id", u.id},
            {"username", u.username},
            {"display_name", u.display_name},
            {"email", u.email},
            {"role", u.role},
            {"status", u.status},
            {"locked_until", u.locked_until},
            {"last_login_at", utc_to_local(u.last_login_at)},
            {"last_login_ip", u.last_login_ip},
            {"created_at", utc_to_local(u.created_at)},
        };
    }
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_CONTROLLER_BASE_H_
