#ifndef DZTRADER_WEBUI_CONTROL_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_CONTROL_DOMAIN_SERVICE_H_

#include <drogon/drogon.h>
#include <dztrader/shm/writer.h>
#include <spdlog/spdlog.h>
#include <string>

namespace dztrader::webui {

/// 控制领域服务：SHUTDOWN_ALL / SHUTDOWN（契约 process）/ UPDATE_SHM_EVENT_SUBSCRIBER（契约 shm）。
/// 无镜像、无广播（后台控制帧，dzweb 自身消费）。
/// **线程安全（关键）**：本服务方法可能被 FrameRouter 在监听线程调用（register_raw），
/// 因此方法内部一律 queueInLoop 投递到 drogon IO 线程后执行实际动作——
/// 原 EventMonitor::post_shutdown_all/post_shutdown/post_update_subscribers 的投递逻辑原样搬入，
/// 保证 drogon::app().quit() 与 MultiWriter::refresh_subscribers() 均在主线程执行。
/// queueInLoop lambda 内部 try/catch 记 WARN，对齐原 EventMonitor::post_* 实现（Task 2 评审 minor ①）。
class ControlDomainService {
public:
    ControlDomainService(std::string self_process, shm::MultiWriter* event_writer)
        : self_process_(std::move(self_process)), event_writer_(event_writer) {}

    void on_shutdown_all() {
        drogon::app().getLoop()->queueInLoop([]() {
            try {
                SPDLOG_INFO("shutdown request received (broadcast)");
                drogon::app().quit();
            } catch (const std::exception& e) {
                SPDLOG_WARN("shutdown_all failed | error={}", e.what());
            }
        });
    }

    void on_shutdown(const std::string& source) {
        // 调用方（register_raw handler）已在监听线程拷贝 source 字符串值，投递安全
        drogon::app().getLoop()->queueInLoop([this, source]() {
            try {
                if (source == self_process_) {
                    SPDLOG_INFO("shutdown request received | source={}", source);
                    drogon::app().quit();
                }
            } catch (const std::exception& e) {
                SPDLOG_WARN("shutdown failed | error={}", e.what());
            }
        });
    }

    void on_update_subscribers() {
        drogon::app().getLoop()->queueInLoop([this]() {
            try {
                if (event_writer_) {
                    event_writer_->refresh_subscribers();
                    SPDLOG_INFO("subscribers refreshed (broadcast)");
                }
            } catch (const std::exception& e) {
                SPDLOG_WARN("update_subscribers failed | error={}", e.what());
            }
        });
    }

private:
    std::string self_process_;
    shm::MultiWriter* event_writer_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_CONTROL_DOMAIN_SERVICE_H_
