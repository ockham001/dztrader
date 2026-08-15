#ifndef DZTRADER_WEBUI_USER_CONTROLLER_H_
#define DZTRADER_WEBUI_USER_CONTROLLER_H_

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>
#include <memory>
#include "repository.h"
#include "config.h"
#include "controller_base.h"

namespace dztrader::webui {

class UserCtrl : public drogon::HttpController<UserCtrl, false>, public ControllerBase {
public:
    UserCtrl(std::shared_ptr<Repository> repo, WebuiConfig cfg)
        : ControllerBase(std::move(repo)), cfg_(std::move(cfg)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UserCtrl::list, "/api/user", drogon::Get);
    ADD_METHOD_TO(UserCtrl::get, "/api/user/{1}", drogon::Get);
    ADD_METHOD_TO(UserCtrl::create, "/api/user", drogon::Post);
    ADD_METHOD_TO(UserCtrl::update, "/api/user/{1}", drogon::Put);
    ADD_METHOD_TO(UserCtrl::remove, "/api/user/{1}", drogon::Delete);
    ADD_METHOD_TO(UserCtrl::update_status, "/api/user/{1}/status", drogon::Put);
    ADD_METHOD_TO(UserCtrl::reset_password, "/api/user/{1}/password", drogon::Put);
    ADD_METHOD_TO(UserCtrl::get_permissions, "/api/user/{1}/permissions", drogon::Get);
    ADD_METHOD_TO(UserCtrl::update_permissions, "/api/user/{1}/permissions", drogon::Put);
    ADD_METHOD_TO(UserCtrl::change_password, "/api/auth/change-password", drogon::Post);
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void get(const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    void create(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void update(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    void remove(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    void update_status(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    void reset_password(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    void get_permissions(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    void update_permissions(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    void change_password(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    WebuiConfig cfg_;

    static nlohmann::json permission_to_json(const Permission& p);
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_USER_CONTROLLER_H_
