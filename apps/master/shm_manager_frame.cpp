#include "shm_manager.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/encoding.h>
#include <dztrader/data_type.h>
#include <dztrader/log/log.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/shm/frame_view.h>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include <optional>
#include <string>
#include <string_view>

#include "process_supervisor.h"

namespace dztrader::master {

namespace {

// event 与 action/成功 的映射（契约 process ProcessEvent 表）
platform::ProcessEvent process_event_of(platform::ProcessAction action, bool ok) {
    using platform::ProcessAction;
    using platform::ProcessEvent;
    switch (action) {
        case ProcessAction::Start:  return ok ? ProcessEvent::StartSucceeded : ProcessEvent::StartFailed;
        case ProcessAction::Stop:   return ok ? ProcessEvent::StopSucceeded : ProcessEvent::StopFailed;
        case ProcessAction::Remove: return ok ? ProcessEvent::RemoveSucceeded : ProcessEvent::RemoveFailed;
    }
    return ok ? ProcessEvent::StartSucceeded : ProcessEvent::StartFailed;  // unreachable
}

}  // namespace

void ShmManager::drain_event_channel() {
    // 信号量唤醒后排空已到达的帧，无上限
    // (master 只有一个事件源，不像 mdctp 需在 shm 帧与事件队列间分配时间片)
    while (true) {
        const auto* frame = event_reader_.next_frame();
        if (!frame) {
            break;
        }
        handle_frame(frame);
    }
}

void ShmManager::handle_frame(const std::byte* frame) {
    try {
        shm::FrameView view(frame);
        // 第一层: 无 instance_id 帧 (不做目标匹配)
        switch (view.type()) {
            case DZ_FRAME_QUERY_FULL_SNAPSHOT: {
                // 全量刷新: 上报 master 自己的配置 + 推送所有进程状态
                // 子进程收到 QUERY_FULL_SNAPSHOT 后各自上报自己的配置 (RTN_*_CONFIG)
                report_full_snapshot();
                return;
            }
            case DZ_FRAME_REQUEST_PROCESS_CONTROL: {
                handle_process_control(view);
                return;
            }
            case DZ_FRAME_SET_PROCESS_CONFIG: {
                handle_set_process_config(view);
                return;
            }
            case DZ_FRAME_REQUEST_MD_READER_REGISTER: {
                // 消费方为 master: instance_id = 目标行情通道名 (非 name_), 必须在第一层处理
                handle_md_reader_register(view);
                return;
            }
            case DZ_FRAME_REQUEST_MD_READER_UNREGISTER: {
                handle_md_reader_unregister(view);
                return;
            }
            case DZ_FRAME_NOTIFY_MD_STARTED: {
                // 行情进程就绪宣告 (instance_id=行情进程名=通道名, 非 name_,
                // 必须第一层处理): 置位通道就绪, 此后读者接入可通过校验 (契约 shm)
                mark_md_channel_ready(view.ext_inst_id());
                return;
            }
            case DZ_FRAME_SET_EVENT_SHM_CONFIG: {
                // 无 instance_id (DzExtFrameHeader): 目标唯一为 master。
                // 流程: decode -> set_shm_config (内部 validate+merge+save) -> rtn
                // 失败: catch -> 日志 + notify_ui -> rtn (旧值)
                try {
                    event_shm_config_.set_shm_config(
                        shm::decode_ext_json<nlohmann::json>(view));
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("set event shm config failed | error=\"{}\"", e.what());
                    notify_ui_.error(std::string("事件通道配置更新失败: ") + e.what());
                }
                event_shm_config_.rtn_shm_config();
                reschedule_event_shm_maintenance();
                return;
            }
            default:
                break;
        }
        // 网关: 含 instance_id 帧必须匹配 instance_id == name_ (与 exe stem 一致)
        if (std::string_view(view.ext_inst_id()) != name_) {
            return;
        }
        // 第二层: 含 instance_id 帧 (instance_id == name_)
        switch (view.type()) {
            case DZ_FRAME_SET_LOG_CONFIG: {
                // 帧头 instance_id == name_。payload = JSON patch。
                // 流程: decode -> set_log_config -> rtn
                // 失败: catch -> 日志 + notify_ui -> rtn (旧值)
                try {
                    log_config_.set_log_config(
                        shm::decode_ext_inst_json<nlohmann::json>(view));
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("set log config failed | error=\"{}\"", e.what());
                    notify_ui_.error(std::format("日志配置更新失败: {}", e.what()));
                }
                log_config_.rtn_log_config(event_writer_);
                return;
            }
            case DZ_FRAME_FLUSH_LOG: {
                spdlog::default_logger()->flush();
                SPDLOG_INFO("log flushed by request");
                return;
            }
            default:
                break;
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("handle_frame unexpected exception | error=\"{}\"", e.what());
    } catch (...) {
        SPDLOG_ERROR("handle_frame unexpected non-std exception");
    }
}

void ShmManager::handle_process_control(const shm::FrameView& view) {
    platform::ProcessControlReq req;
    try {
        req = shm::decode_ext_json<platform::ProcessControlReq>(view);
    } catch (const std::exception& e) {
        // 坏 payload（非 JSON / 缺 action/target / 枚举非法 / config=null）：
        // 无法配对 event（action/target 未知），仅日志 + NOTIFY_UI 弹窗；
        // 前端 pending 按总则 §7 超时兜底
        SPDLOG_ERROR("process control decode failed | error=\"{}\"", e.what());
        notify_ui_.error(std::format("进程控制请求无效: {}", e.what()));
        return;
    }
    SPDLOG_INFO("process control received | action={} target={}",
                magic_enum::enum_name(req.action), req.target);
    if (!supervisor_ || !process_config_store_) {
        const std::string err = std::format("process control unavailable | supervisor={} store={}",
                                            static_cast<bool>(supervisor_),
                                            static_cast<bool>(process_config_store_));
        SPDLOG_WARN("{}", err);
        if (supervisor_) {
            supervisor_->send_process_status(req.target, ChildState::Crashed, 0, err,
                                             process_event_of(req.action, false));
        }
        return;
    }
    switch (req.action) {
        case platform::ProcessAction::Start:
            handle_process_start(req);
            return;
        case platform::ProcessAction::Stop:
            handle_process_stop(req);
            return;
        case platform::ProcessAction::Remove:
            handle_process_remove(req);
            return;
    }
}

void ShmManager::handle_process_start(const platform::ProcessControlReq& req) {
    // 1. 未注册 target（契约 process修订）: 实时扫描 App Root, 找到同名网关 exe 则
    //    动态注册（registry + dztraderd.json + store 镜像）后继续启动流程;
    //    exe 不存在或非网关进程（策略有独立注册流程）才回 StartFailed。
    bool dynamically_registered = false;
    if (!supervisor_->find_registry_entry(req.target)) {
        const auto* scanned = supervisor_->find_exe_by_stem(req.target);
        if (!scanned) {
            const std::string err = std::format("target not registered and exe not found | target={}",
                                                req.target);
            SPDLOG_WARN("{}", err);
            notify_ui_.error(err);
            supervisor_->send_process_status(req.target, ChildState::Crashed, 0, err,
                                             platform::ProcessEvent::StartFailed);
            return;
        }
        if (scanned->category != Category::GatewayMd &&
            scanned->category != Category::GatewayTd) {
            // 非网关进程（策略等）有独立注册流程, 不在 Start 时动态注册
            const std::string err = std::format("target not registered and not a gateway | target={}",
                                                req.target);
            SPDLOG_WARN("{}", err);
            notify_ui_.error(err);
            supervisor_->send_process_status(req.target, ChildState::Crashed, 0, err,
                                             platform::ProcessEvent::StartFailed);
            return;
        }
        try {
            register_dynamic_gateway(*scanned);
            dynamically_registered = true;
        } catch (const std::exception& e) {
            const std::string err = std::format("dynamic register failed | target={} error=\"{}\"",
                                                req.target, e.what());
            SPDLOG_ERROR("{}", err);
            notify_ui_.error(err);
            supervisor_->send_process_status(req.target, ChildState::Crashed, 0, err,
                                             platform::ProcessEvent::StartFailed);
            return;
        }
    }
    // 2. 携带 config：先应用（等价 SET），帧顺序：先 118 后 116（契约 process）
    if (req.config) {
        try {
            process_config_store_->set_process_config(req.target, *req.config);
        } catch (const std::exception& e) {
            const std::string err = std::format("start config apply failed | target={} error=\"{}\"",
                                                req.target, e.what());
            SPDLOG_ERROR("{}", err);
            notify_ui_.error(err);
            process_config_store_->rtn_process_config();  // 回滚旧值（store 强保证内部不变）
            supervisor_->send_process_status(req.target, ChildState::Crashed, 0, err,
                                             platform::ProcessEvent::StartFailed);
            return;  // 不启动（契约 process）
        }
        process_config_store_->rtn_process_config();  // 全量新值
    } else if (dynamically_registered) {
        // 动态注册的新条目同步给 dzweb（注册守卫依赖 process_config 条目, 必须先于 116）
        process_config_store_->rtn_process_config();
    }
    // 幂等：已在运行（契约 process，重试安全；已 Running/Stopping 均视为成功）
    if (auto existing = supervisor_->find_child(req.target);
        existing && existing->state() != ChildState::Stopped) {
        supervisor_->send_process_status(req.target, existing->state(),
                                         static_cast<int>(existing->pid()), "",
                                         platform::ProcessEvent::StartSucceeded);
        return;
    }
    // 3. 启动（page_size 走配置文件读取，不再有 override）
    try {
        // md 通道由 launch_child 在启动 GatewayMd 时创建（唯一入口）
        bool started = supervisor_->start_process(req.target);
        notify_md_channel_subscriber_update(req.target);
        if (started) {
            // 真实 pid：从 supervisor 的 child 查询（契约示例第 162 行 pid: 12345）
            const auto child = supervisor_->find_child(req.target);
            supervisor_->send_process_status(req.target, ChildState::Running,
                                             child ? static_cast<int>(child->pid()) : 0, "",
                                             platform::ProcessEvent::StartSucceeded);
            return;
        }
        // spawn 失败：配置保留（契约 process；118 已推新值）
        const std::string err = std::format("start_process returned false | target={}", req.target);
        SPDLOG_WARN("{}", err);
        notify_ui_.error(err);
        supervisor_->send_process_status(req.target, ChildState::Crashed, 0, err,
                                         platform::ProcessEvent::StartFailed);
    } catch (const std::exception& e) {
        const std::string err = std::format("start failed | target={} error=\"{}\"",
                                            req.target, e.what());
        SPDLOG_ERROR("{}", err);
        notify_ui_.error(err);
        supervisor_->send_process_status(req.target, ChildState::Crashed, 0, err,
                                         platform::ProcessEvent::StartFailed);
    }
}

void ShmManager::handle_process_stop(const platform::ProcessControlReq& req) {
    if (!supervisor_->find_registry_entry(req.target)) {
        const std::string err = std::format("target not registered | target={}", req.target);
        SPDLOG_WARN("{}", err);
        notify_ui_.error(err);
        supervisor_->send_process_status(req.target, ChildState::Stopped, 0, err,
                                         platform::ProcessEvent::StopFailed);
        return;
    }
    // 幂等：无 child（未运行）→ 直接成功（契约 process）
    if (!supervisor_->find_child(req.target)) {
        supervisor_->send_process_status(req.target, ChildState::Stopped, 0, "",
                                         platform::ProcessEvent::StopSucceeded);
        return;
    }
    try {
        supervisor_->stop_process(req.target);
        auto child = supervisor_->find_child(req.target);
        supervisor_->send_process_status(req.target, ChildState::Stopping,
                                         child ? static_cast<int>(child->pid()) : 0, "",
                                         platform::ProcessEvent::StopSucceeded);
    } catch (const std::exception& e) {
        const std::string err = std::format("stop failed | target={} error=\"{}\"", req.target, e.what());
        SPDLOG_ERROR("{}", err);
        notify_ui_.error(err);
        supervisor_->send_process_status(req.target, ChildState::Stopping, 0, err,
                                         platform::ProcessEvent::StopFailed);
    }
}

void ShmManager::handle_process_remove(const platform::ProcessControlReq& req) {
    auto child = supervisor_->find_child(req.target);
    const auto* entry = supervisor_->find_registry_entry(req.target);
    if (!child && !entry) {
        const std::string err = std::format("target not found | name={}", req.target);
        SPDLOG_WARN("{}", err);
        // 契约 process: Remove 对未注册 target 必须 NOTIFY_UI 错误级别弹窗 (popup=true)
        notify_ui_.error(err);
        supervisor_->send_process_status(req.target, ChildState::Stopped, 0, err,
                                         platform::ProcessEvent::RemoveFailed);  // 不幂等（契约 process）
        return;
    }
    // 先删配置并推 118（条目消失 = 移除完成的权威信号，契约 process）
    try {
        process_config_store_->remove(req.target);
    } catch (const std::exception& e) {
        const std::string err = std::format("remove config failed | target={} error=\"{}\"",
                                            req.target, e.what());
        SPDLOG_ERROR("{}", err);
        notify_ui_.error(err);
        supervisor_->send_process_status(req.target, ChildState::Stopped, 0, err,
                                         platform::ProcessEvent::RemoveFailed);
        return;
    }
    process_config_store_->rtn_process_config();
    // 停止/兜底（防御性 catch：异常时仍回 RemoveFailed，契约 process）
    try {
        supervisor_->mark_remove_pending(req.target);
        if (!child) {
            supervisor_->notify_removed_for_inactive(req.target);
        } else {
            supervisor_->stop_process(req.target);
        }
        supervisor_->send_process_status(req.target, child ? ChildState::Stopping : ChildState::Stopped,
                                         child ? static_cast<int>(child->pid()) : 0, "",
                                         platform::ProcessEvent::RemoveSucceeded);
    } catch (const std::exception& e) {
        const std::string err = std::format("remove stop failed | target={} error=\"{}\"",
                                            req.target, e.what());
        SPDLOG_ERROR("{}", err);
        notify_ui_.error(err);
        supervisor_->send_process_status(req.target, child ? ChildState::Stopping : ChildState::Stopped,
                                         child ? static_cast<int>(child->pid()) : 0, err,
                                         platform::ProcessEvent::RemoveFailed);
    }
}

void ShmManager::handle_set_process_config(const shm::FrameView& view) {
    platform::SetProcessConfigReq req;
    try {
        req = shm::decode_ext_json<platform::SetProcessConfigReq>(view);
    } catch (const std::exception& e) {
        // 坏 payload（config 缺失/null、target 缺失/非字符串）：四件套
        // （日志 + NOTIFY_UI 弹窗 + 回 RTN_PROCESS_CONFIG 旧值，store 镜像不变）
        SPDLOG_ERROR("set process config decode failed | error=\"{}\"", e.what());
        notify_ui_.error(std::format("进程配置更新失败: {}", e.what()));
        if (process_config_store_) {
            process_config_store_->rtn_process_config();
        }
        return;
    }
    if (!process_config_store_) {
        SPDLOG_WARN("process config store not set, ignoring set process config");
        return;
    }
    try {
        process_config_store_->set_process_config(req.target, req.config);
        SPDLOG_INFO("process config updated | target={}", req.target);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("set process config failed | target={} error=\"{}\"", req.target, e.what());
        notify_ui_.error(std::format("进程配置更新失败: {}", e.what()));
        // store 强保证：内部不变，推 118 为回滚旧值
    }
    process_config_store_->rtn_process_config();  // 成功推新值 / 失败推旧值（契约 process）
}

void ShmManager::send_shutdown(std::string_view target) {
    // 写 REQUEST_SHUTDOWN 帧：instance_id 为 target（仅匹配的进程执行）
    // 必须通知订阅者: 子进程阻塞在 event_queue->wait() (信号量等待),
    // 不 notify 的话子进程永远收不到 REQUEST_SHUTDOWN, master 超时后强制 terminate,
    // exit_code 非 0 触发 restart 策略, 进程被重新拉起, 导致 UI "删除此行情源" 删不掉
    platform::write_ext_inst_raw(event_writer_, DZ_FRAME_REQUEST_SHUTDOWN, target);
}

void ShmManager::report_log_config() {
    log_config_.rtn_log_config(event_writer_);
}

void ShmManager::report_shm_config() {
    event_shm_config_.rtn_shm_config();
}

void ShmManager::report_full_snapshot() {
    report_log_config();
    report_shm_config();
    // 进程配置全量（118，契约 process）
    if (process_config_store_) {
        process_config_store_->rtn_process_config();
    }
    // 每个注册进程一条状态（116，event 缺失=自发，契约 process）
    if (supervisor_) {
        for (const auto& entry : supervisor_->registry_entries()) {
            auto child = supervisor_->find_child(entry.name);
            supervisor_->send_process_status(
                entry.name,
                child ? child->state() : ChildState::Stopped,
                child ? static_cast<int>(child->pid()) : 0);
        }
        SPDLOG_INFO("full snapshot reported | configs + {} processes",
                    supervisor_->registry_entries().size());
    } else {
        SPDLOG_WARN("full snapshot reported, supervisor not set, configs only");
    }
}

void ShmManager::write_process_status(const platform::ProcessStatus& status) {
    try {
        platform::write_ext_json(event_writer_, DZ_FRAME_RTN_PROCESS_STATUS, status);
    } catch (const std::exception& e) {
        SPDLOG_WARN("failed to write process status | name={} error=\"{}\"", status.name, e.what());
    }
}

void ShmManager::remove_reader(std::string_view name) {
    // 兜底清理订阅者: 仅调 event_meta_->remove_reader, 不通知子进程
    // (子进程已不运行, 无需通知; 与 unregister_subscriber 的差异见头文件注释)
    if (!event_meta_) {
        return;
    }
    try {
        event_meta_->remove_reader(std::string(name));
    } catch (const std::exception& e) {
        SPDLOG_WARN("failed to remove reader | name={} error=\"{}\"", name, e.what());
    }
}

}  // namespace dztrader::master
