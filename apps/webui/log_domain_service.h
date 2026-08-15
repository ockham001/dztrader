#ifndef DZTRADER_WEBUI_LOG_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_LOG_DOMAIN_SERVICE_H_

#include "mirror_store.h"
#include "ws_broadcaster.h"
#include <dztrader/platform/log_config.h>
#include <dztrader/shm/writer.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/this_process.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/notify_ui.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace dztrader::webui {

/// 日志领域服务：RTN_LOG_CONFIG 的镜像更新 + WS 广播（契约 00）。
/// publish() 供 dzweb 自身直调（LogCtrl 路径）——镜像 + 广播，与 RTN 处理行为等价。
/// handle_log_control()：日志控制请求分发（HTTP set_level/flush 的日志配置核心，
/// 原 log_control.cpp 的 dispatch_log_control 迁入）：
///   target == 本进程(exe_stem)：直调 self_log_（不走 SHM），SET 成功后（或失败回退）
///     经 publish 回推 WS；失败时 event_writer_ 非空则发 NOTIFY_UI 错误弹窗（契约 00-log.md:36）。
///   否则写 SHM 帧（SET_LOG_CONFIG / FLUSH_LOG），event_writer_ 为空时跳过（API-only 下其他进程不可达）。
/// 职责边界与 libs/platform 的 LogConfig 一致：时机由外部控制，本服务不主动触发。
class LogDomainService {
public:
    LogDomainService(MirrorStore& mirror,
                     WsBroadcaster& ws,
                     dztrader::platform::LogConfig& self_log,
                     shm::MultiWriter* event_writer)
        : mirror_(mirror), ws_(ws), self_log_(self_log), event_writer_(event_writer) {}

    void on_rtn_log_config(const std::string& source, const nlohmann::json& payload) {
        mirror_.update(source, "log_config", payload);
        ws_.broadcast("log_config", source, payload);
    }

    void publish(const std::string& source, const nlohmann::json& payload) {
        on_rtn_log_config(source, payload);  // 同一语义：镜像 + 广播（幂等）
    }

    /// 日志控制请求分发（HTTP set_level/flush 的日志配置核心，原 log_control.cpp 迁入）。
    /// 返回 true 表示已处理（SET/FLUSH），false 表示未知类型。
    bool handle_log_control(const std::string& target,
                            DzFrameType type,
                            const nlohmann::json& payload) {
        const bool is_self = (target == dztrader::this_process::exe_stem());

        if (type == DZ_FRAME_SET_LOG_CONFIG) {
            if (is_self) {
                // dzweb 自身：直调 LogConfig（唯一校验/规范化/持久化/应用源），不走 SHM
                try {
                    self_log_.set_log_config(payload);
                } catch (const std::exception& e) {
                    // 契约 00-log.md:36 失败必须日志 + NOTIFY_UI 错误弹窗；API-only 无 writer 时跳过
                    SPDLOG_WARN("webui set log config failed | error={}", e.what());
                    if (event_writer_) {
                        dztrader::platform::NotifyUi(dztrader::this_process::exe_stem(), *event_writer_)
                            .error(std::string("日志配置更新失败: ") + e.what());
                    }
                }
                // 无论成败都回推当前生效值（成功=new，失败=rollback 旧值）
                publish(target, self_log_.current());
                return true;
            }
            // 其他进程：写 SHM SET_LOG_CONFIG 帧（fire-and-forget）
            if (event_writer_) {
                try {
                    if (dztrader::shm::write_ext_inst_json(
                            *event_writer_, DZ_FRAME_SET_LOG_CONFIG, target, payload)) {
                        event_writer_->notify_subscribers();
                    }
                } catch (const std::exception& e) {
                    SPDLOG_WARN("write set_log_config failed | target={} error={}", target, e.what());
                }
            }
            return true;
        }

        if (type == DZ_FRAME_FLUSH_LOG) {
            if (is_self) {
                spdlog::default_logger()->flush();
                return true;
            }
            if (event_writer_) {
                try {
                    dztrader::platform::write_ext_inst_raw(*event_writer_, DZ_FRAME_FLUSH_LOG, target);
                } catch (const std::exception& e) {
                    SPDLOG_WARN("write flush_log failed | target={} error={}", target, e.what());
                }
            }
            return true;
        }

        return false;
    }

private:
    MirrorStore& mirror_;
    WsBroadcaster& ws_;
    dztrader::platform::LogConfig& self_log_;
    shm::MultiWriter* event_writer_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_LOG_DOMAIN_SERVICE_H_
