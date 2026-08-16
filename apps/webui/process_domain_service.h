#ifndef DZTRADER_WEBUI_PROCESS_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_PROCESS_DOMAIN_SERVICE_H_

#include "process_mirror.h"
#include "ws_broadcaster.h"

#include <dztrader/platform/process.h>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace dztrader::webui {

/// 进程领域服务：RTN_PROCESS_STATUS / RTN_PROCESS_CONFIG 的镜像更新 + WS 广播（契约 process）。
/// 行为逐行对照原 WsController::handle_rtn_process_status / handle_rtn_process_config：
/// - 注册守卫：仅当 dztraderd.process_config 含该进程条目时才写入镜像（原实现语义原样保留）；
///   Stopped/Crashed 时 mark_stale（保留 process_status，清除 md_config/md_status）。
/// - 广播在守卫之外无条件执行（前端可感知未知进程状态，与原实现一致）。
/// - 广播 payload 逐字段序列化：name/state/pid/message/display_name/event
///   （event 为 nullopt 时序列化为 null，与原实现逐字节一致）。
/// 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），所有访问均发生在主循环串行执行，不加锁。
class ProcessDomainService {
public:
    ProcessDomainService(ProcessMirror& process_mirror, WsBroadcaster& ws)
        : process_mirror_(process_mirror), ws_(ws) {}

    void on_rtn_process_status(const nlohmann::json& payload) {
        try {
            auto status = payload.get<platform::ProcessStatus>();
            if (process_mirror_.get_process_config(status.name).has_value()) {  // 注册守卫
                process_mirror_.update_status(status.name, status);
                if (status.state == platform::ChildState::Stopped ||
                    status.state == platform::ChildState::Crashed) {
                    process_mirror_.mark_stale(status.name);
                    SPDLOG_INFO("process mirror marked stale | name={} state={}", status.name,
                                magic_enum::enum_name(status.state));
                }
            }
            // 广播（原实现：守卫外无条件广播；字段含 event 序列化）
            nlohmann::json msg = {
                {"name", status.name},
                {"state", status.state},
                {"pid", status.pid},
                {"message", status.message},
                {"display_name", status.display_name},
                {"event", status.event ? nlohmann::json(*status.event) : nlohmann::json()}};
            ws_.broadcast("process_status", "", msg);
            SPDLOG_INFO("process_status pushed | name={} state={} event={}", status.name,
                        magic_enum::enum_name(status.state),
                        status.event ? magic_enum::enum_name(*status.event) : "none");
        } catch (const std::exception& e) {
            SPDLOG_WARN("failed to parse RTN_PROCESS_STATUS | error={}", e.what());
        }
    }

    void on_rtn_process_config(const nlohmann::json& payload) {
        try {
            process_mirror_.update_process_configs(payload);
            ws_.broadcast("process_config", "", payload);
            SPDLOG_DEBUG("process_config pushed | entries={}", payload.size());
        } catch (const std::exception& e) {
            SPDLOG_WARN("failed to parse RTN_PROCESS_CONFIG | error={}", e.what());
        }
    }

private:
    ProcessMirror& process_mirror_;
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_PROCESS_DOMAIN_SERVICE_H_
