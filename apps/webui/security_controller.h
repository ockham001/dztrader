#ifndef DZTRADER_WEBUI_SECURITY_CONTROLLER_H_
#define DZTRADER_WEBUI_SECURITY_CONTROLLER_H_

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>
#include <memory>
#include "repository.h"
#include "controller_base.h"
#include "data_change_notifier.h"

namespace dztrader::webui {

class SecurityCtrl : public drogon::HttpController<SecurityCtrl, false>, public ControllerBase {
public:
    SecurityCtrl(std::shared_ptr<Repository> repo, DataChangeNotifier& notifier)
        : ControllerBase(std::move(repo)), notifier_(notifier) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SecurityCtrl::get_config, "/api/security/config", drogon::Get);
    ADD_METHOD_TO(SecurityCtrl::set_config, "/api/security/config", drogon::Put);
    ADD_METHOD_TO(SecurityCtrl::list_blacklist, "/api/security/blacklist", drogon::Get);
    ADD_METHOD_TO(SecurityCtrl::add_blacklist, "/api/security/blacklist", drogon::Post);
    ADD_METHOD_TO(SecurityCtrl::remove_blacklist, "/api/security/blacklist/{1}", drogon::Delete);
    ADD_METHOD_TO(SecurityCtrl::list_whitelist, "/api/security/whitelist", drogon::Get);
    ADD_METHOD_TO(SecurityCtrl::add_whitelist, "/api/security/whitelist", drogon::Post);
    ADD_METHOD_TO(SecurityCtrl::remove_whitelist, "/api/security/whitelist/{1}", drogon::Delete);
    ADD_METHOD_TO(SecurityCtrl::login_history, "/api/security/login-history", drogon::Get);
    ADD_METHOD_TO(SecurityCtrl::ack_default_password, "/api/security/ack-default-password", drogon::Post);
    METHOD_LIST_END

    void get_config(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void set_config(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void list_blacklist(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void add_blacklist(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void remove_blacklist(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                          int64_t id);
    void list_whitelist(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void add_whitelist(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void remove_whitelist(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                          int64_t id);
    void login_history(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void ack_default_password(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    DataChangeNotifier& notifier_;

    static nlohmann::json ip_entry_to_json(const IpEntry& e);
    static nlohmann::json login_history_to_json(const LoginHistoryEntry& h);
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_SECURITY_CONTROLLER_H_
