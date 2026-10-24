#ifndef DZTRADER_WEBUI_SYSMON_CONTROLLER_H_
#define DZTRADER_WEBUI_SYSMON_CONTROLLER_H_

#include <drogon/HttpController.h>

#include <memory>

#include "controller_base.h"
#include "mirror_store.h"
#include "shm_writer.h"
#include "sysmon_domain_service.h"

namespace dztrader::webui {

/// 系统监控 REST（契约 rest §2.8，契约 sysmon）。
/// - GET /api/sysmon/snapshot：读取镜像当前快照；镜像尚未就绪回 503。
/// - PUT /api/sysmon/config：运行期配置 merge patch（admin），直发 SET_SYSMON_CONFIG。
class SysmonCtrl : public drogon::HttpController<SysmonCtrl, false>, public ControllerBase {
public:
    SysmonCtrl(std::shared_ptr<Repository> repo,
               std::shared_ptr<MirrorStore> mirror,
               std::shared_ptr<ShmWriter> shm_writer)
        : ControllerBase(std::move(repo)),
          mirror_(std::move(mirror)),
          shm_writer_(std::move(shm_writer)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SysmonCtrl::get_snapshot, "/api/sysmon/snapshot", drogon::Get);
    ADD_METHOD_TO(SysmonCtrl::set_config, "/api/sysmon/config", drogon::Put);
    METHOD_LIST_END

    void get_snapshot(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// PUT /api/sysmon/config：body 为 RFC 7386 merge patch（admin）。
    /// 响应只表示"已写事件通道"；生效信号 = WS `sysmon_config`（契约 rest §1、契约 sysmon §3）。
    void set_config(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<MirrorStore> mirror_;
    std::shared_ptr<ShmWriter> shm_writer_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_SYSMON_CONTROLLER_H_
