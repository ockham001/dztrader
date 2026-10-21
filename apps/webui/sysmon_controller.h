#ifndef DZTRADER_WEBUI_SYSMON_CONTROLLER_H_
#define DZTRADER_WEBUI_SYSMON_CONTROLLER_H_

#include <drogon/HttpController.h>

#include "controller_base.h"
#include "mirror_store.h"
#include "sysmon_domain_service.h"

namespace dztrader::webui {

/// 系统监控 REST（契约 rest §2.8，契约 sysmon）。
/// - GET /api/sysmon/snapshot：读取镜像当前快照；镜像尚未就绪回 503。
class SysmonCtrl : public drogon::HttpController<SysmonCtrl, false>, public ControllerBase {
public:
    SysmonCtrl(std::shared_ptr<Repository> repo, std::shared_ptr<MirrorStore> mirror)
        : ControllerBase(std::move(repo)),
          mirror_(std::move(mirror)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SysmonCtrl::get_snapshot, "/api/sysmon/snapshot", drogon::Get);
    METHOD_LIST_END

    void get_snapshot(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<MirrorStore> mirror_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_SYSMON_CONTROLLER_H_
