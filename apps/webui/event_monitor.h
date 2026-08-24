#ifndef DZTRADER_WEBUI_EVENT_MONITOR_H_
#define DZTRADER_WEBUI_EVENT_MONITOR_H_

#include <dztrader/shm/reader.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/struct.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "event_preloader.h"

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
/// - 组合 EventChannelPreloader：被动响应 master 的 PRELOAD_EVENT_SHM 广播
///   （随机延迟后 reader 半边就地 / writer 半边投递 IO 线程，对齐 md_api_scheduled.cpp）
class EventMonitor {
public:
    /// @param event_writer 本进程的事件通道 writer（与 ShmWriter/领域服务同一实例）
    EventMonitor(const std::filesystem::path& shm_dir,
                 FrameRouter& router,
                 std::shared_ptr<shm::MultiWriter> event_writer);
    ~EventMonitor();

    /// 启动监听线程（reader 为空时 no-op，对应 API-only 模式）
    void start();

    /// 停止监听线程（唤醒并 join）
    void stop();

    /// 收到 PRELOAD_EVENT_SHM 广播（register_raw 回调, 监听线程）:
    /// 随机延迟 0-5s 后执行预加载。api-only 模式（无 reader）为 no-op。
    void schedule_event_shm_preload(const DzShmPreload& params);

private:
    /// 监听线程主循环：信号量等待(定时器驱动 wait_for) -> tick -> drain_and_post
    void run();

    /// 排空 event channel，逐帧交给 FrameRouter 分发给已注册 handler
    void drain_and_post();

    std::shared_ptr<shm::Reader> reader_;
    FrameRouter& router_;
    std::unique_ptr<EventChannelPreloader> preloader_;

    std::optional<shm::NamedSemaphore> sem_;
    std::atomic<bool> stop_flag_{false};
    std::thread thread_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_EVENT_MONITOR_H_
