#ifndef DZTRADER_WEBUI_PROCESS_CONTROLLER_H_
#define DZTRADER_WEBUI_PROCESS_CONTROLLER_H_

#include <drogon/HttpController.h>
#include "controller_base.h"
#include "process_mirror.h"

namespace dztrader::webui {

class ProcessCtrl : public drogon::HttpController<ProcessCtrl, false>, public ControllerBase {
public:
    ProcessCtrl(std::shared_ptr<Repository> repo, std::shared_ptr<ProcessMirror> process_mirror)
        : ControllerBase(std::move(repo)), process_mirror_(std::move(process_mirror)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ProcessCtrl::get_all, "/api/processes", drogon::Get);
    METHOD_LIST_END

    void get_all(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<ProcessMirror> process_mirror_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_PROCESS_CONTROLLER_H_
