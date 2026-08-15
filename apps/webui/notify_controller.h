#ifndef DZTRADER_WEBUI_NOTIFY_CONTROLLER_H_
#define DZTRADER_WEBUI_NOTIFY_CONTROLLER_H_

#include <drogon/HttpController.h>
#include "controller_base.h"
#include "notify_cache.h"

namespace dztrader::webui {

class NotifyCtrl : public drogon::HttpController<NotifyCtrl, false>, public ControllerBase {
public:
    NotifyCtrl(std::shared_ptr<Repository> repo, std::shared_ptr<NotifyCache> notify_cache)
        : ControllerBase(std::move(repo)), notify_cache_(std::move(notify_cache)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(NotifyCtrl::get_recent, "/api/notifications", drogon::Get);
    METHOD_LIST_END

    void get_recent(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<NotifyCache> notify_cache_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_NOTIFY_CONTROLLER_H_
