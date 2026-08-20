#ifndef DZTRADER_WEBUI_CONTROL_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_CONTROL_DOMAIN_SERVICE_H_

#include <drogon/drogon.h>
#include <dztrader/shm/writer.h>
#include <spdlog/spdlog.h>
#include <functional>
#include <string>

namespace dztrader::webui {

/// 控制领域服务：SHUTDOWN（契约 process）/ UPDATE_SHM_EVENT_SUBSCRIBER（契约 shm）。
/// 无镜像、无广播（后台控制帧，dzweb 自身消费）。
/// **线程安全（关键）**：本服务方法可能被 FrameRouter 在监听线程调用（register_raw），
/// 因此方法内部一律投递到 IO 循环 getIOLoop(0)（与 REST/WS 同线程）后执行实际动作——
/// 保证 drogon::app().quit() 与 MultiWriter::refresh_subscribers() 均在 IO 线程执行，
/// 与 REST/WS 连接回调串行，消除跨线程数据竞争（E1 修复）。
/// quit() 从 IO 线程调用线程安全（EventLoop::quit 原子 store + wakeup，trantor 保证）。
/// lambda 内部 try/catch 记 WARN，对齐原 EventMonitor::post_* 实现（Task 2 评审 minor ①）。
class ControlDomainService {
public:
    ControlDomainService(std::string self_process, shm::MultiWriter* event_writer)
        : self_process_(std::move(self_process)), event_writer_(event_writer) {}

    void on_shutdown(const std::string& source) {
        // 调用方（register_raw handler）已在监听线程拷贝 source 字符串值，投递安全
        post_to_io_loop([this, source]() {
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

    // 移到 IO 线程后，MultiWriter 的全部调用（REST write_* 与 refresh_subscribers）
    // 自此同线程串行，无跨线程竞争
    void on_update_subscribers() {
        post_to_io_loop([this]() {
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
    /// 投递到 IO 循环 getIOLoop(0)（与 REST/WS 同线程）；启动窗口（run() 前 IO 循环
    /// 未建，getIOLoop 返回 nullptr）兜底投主循环——该窗口无任何连接，安全
    static void post_to_io_loop(std::function<void()> f) {
        auto* io_loop = drogon::app().getIOLoop(0);
        if (io_loop != nullptr) {
            io_loop->queueInLoop(std::move(f));
        } else {
            drogon::app().getLoop()->queueInLoop(std::move(f));
        }
    }

    std::string self_process_;
    shm::MultiWriter* event_writer_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_CONTROL_DOMAIN_SERVICE_H_
