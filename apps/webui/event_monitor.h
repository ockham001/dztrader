#ifndef DZTRADER_WEBUI_EVENT_MONITOR_H_
#define DZTRADER_WEBUI_EVENT_MONITOR_H_

#include <dztrader/shm/reader.h>
#include <dztrader/shm/named_semaphore.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace dztrader::shm { class FrameView; }

namespace dztrader::webui {

class FrameRouter;

/// SHM 事件通道监听器：在独立线程中读取 SHM 帧，交给 FrameRouter 分发给领域服务。
///
/// 设计：
/// - 监听线程仅做 SHM 读取 + FrameRouter::dispatch（decode 由 router 内部按 handler 类型处理）
/// - 各帧类型对应的处理已注册到 FrameRouter（register_json / register_raw），
///   由领域服务在 IO 线程 getIOLoop(0)（register_json）或监听线程（register_raw，
///   ControlDomainService 内部投递到 IO 循环）执行
class EventMonitor {
public:
    EventMonitor(const std::filesystem::path& shm_dir,
                 FrameRouter& router);
    ~EventMonitor();

    /// 启动监听线程（reader 为空时 no-op，对应 API-only 模式）
    void start();

    /// 停止监听线程（唤醒并 join）
    void stop();

private:
    /// 监听线程主循环：阻塞等待信号量 -> drain_and_post
    void run();

    /// 排空 event channel，逐帧交给 FrameRouter 分发给已注册 handler
    void drain_and_post();

    std::shared_ptr<shm::Reader> reader_;
    FrameRouter& router_;

    std::optional<shm::NamedSemaphore> sem_;
    std::atomic<bool> stop_flag_{false};
    std::thread thread_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_EVENT_MONITOR_H_
