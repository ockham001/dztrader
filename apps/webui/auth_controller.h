#ifndef DZTRADER_WEBUI_AUTH_CONTROLLER_H_
#define DZTRADER_WEBUI_AUTH_CONTROLLER_H_

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>
#include <memory>
#include "config.h"
#include "repository.h"
#include "controller_base.h"

namespace dztrader::webui {

class LoginCtrl : public drogon::HttpController<LoginCtrl, false>, public ControllerBase {
public:
    LoginCtrl(std::shared_ptr<WebuiConfig> cfg, std::shared_ptr<Repository> repo)
        : ControllerBase(std::move(repo)), cfg_(std::move(cfg)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(LoginCtrl::login, "/api/login", drogon::Post);
    METHOD_LIST_END

    void login(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<WebuiConfig> cfg_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_AUTH_CONTROLLER_H_
