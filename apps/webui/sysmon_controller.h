#ifndef DZTRADER_WEBUI_SYSMON_CONTROLLER_H_
#define DZTRADER_WEBUI_SYSMON_CONTROLLER_H_

#include <drogon/HttpController.h>

#include <filesystem>
#include <memory>

#include "controller_base.h"
#include "mirror_store.h"
#include "shm_writer.h"
#include "sysmon_domain_service.h"
#include "sysmon_history_service.h"

namespace dztrader::webui {

/// 系统监控 REST（《帧契约：REST》§2.8，《帧契约：系统监控》§6）。
/// 读端点（snapshot / history / config）只要求已认证：认证在 JWT advice 层完成，
/// handler 内**不**做角色判断；写端点（PUT config）在 handler 内校验 admin。
/// - GET /api/sysmon/snapshot：读取镜像当前快照；镜像尚未就绪回 503。
/// - GET /api/sysmon/history：只读查询历史库（《帧契约：系统监控》§6.1）；降级回 200 +
/// available=false。
/// - GET /api/sysmon/config：读取镜像中的当前生效配置；镜像尚未就绪回 503。
/// - PUT /api/sysmon/config：运行期配置 merge patch（admin），直发 SET_SYSMON_CONFIG。
class SysmonCtrl : public drogon::HttpController<SysmonCtrl, false>, public ControllerBase {
public:
    SysmonCtrl(std::shared_ptr<Repository> repo,
               std::shared_ptr<MirrorStore> mirror,
               std::shared_ptr<ShmWriter> shm_writer,
               std::filesystem::path history_db_path = {})
        : ControllerBase(std::move(repo)),
          mirror_(std::move(mirror)),
          shm_writer_(std::move(shm_writer)),
          history_db_path_(std::move(history_db_path)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SysmonCtrl::get_snapshot, "/api/sysmon/snapshot", drogon::Get);
    ADD_METHOD_TO(SysmonCtrl::get_history, "/api/sysmon/history", drogon::Get);
    ADD_METHOD_TO(SysmonCtrl::get_config, "/api/sysmon/config", drogon::Get);
    ADD_METHOD_TO(SysmonCtrl::set_config, "/api/sysmon/config", drogon::Put);
    METHOD_LIST_END

    void get_snapshot(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// GET /api/sysmon/history：历史曲线查询（《帧契约：系统监控》§6.1）。
    /// 参数非法回 400；历史库不可用回 200 + `{"available":false,"reason":...}`（不 5xx、不伪造）。
    void get_history(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// GET /api/sysmon/config：读取镜像中的当前生效配置（《帧契约：系统监控》§6）。
    /// 任意已认证用户可读；镜像未就绪（dzsysmon 尚未回推配置）回 503。
    void get_config(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// PUT /api/sysmon/config：body 为 RFC 7386 merge patch（admin）。
    /// 响应只表示"已写事件通道"；生效信号 = WS `sysmon_config`（契约 rest §1、契约 sysmon §3）。
    void set_config(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    /// 取镜像固定实例（`dzsysmon`）下的某个域并回 200；域缺失回 503 + @p error_message。
    /// 快照与配置两个读端点共用（避免逐字复制同一段镜像读取逻辑）。
    void reply_mirror_domain(const char* domain,
                             const char* error_message,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    std::shared_ptr<MirrorStore> mirror_;
    std::shared_ptr<ShmWriter> shm_writer_;
    /// 历史库路径（`$DZTRADER_HOME/db/sysmon.db`）；空 = 历史查询降级为不可用。
    std::filesystem::path history_db_path_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_SYSMON_CONTROLLER_H_
