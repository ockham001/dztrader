#ifndef DZTRADER_WEBUI_EVENT_PRELOADER_H_
#define DZTRADER_WEBUI_EVENT_PRELOADER_H_

#include <dztrader/core/timer_queue.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/struct.h>

#include <chrono>
#include <cstdint>
#include <memory>

namespace dztrader::webui {

/// 事件通道预加载执行器（被动，响应 master 的 DZ_FRAME_PRELOAD_EVENT_SHM 广播）。
/// 与 md_api_scheduled.cpp 的 schedule_event_shm_preload / on_event_shm_timer 对齐：
/// 收到广播后随机延迟执行 reader/writer 四件套（prefetch + release|close + touch）。
///
/// 线程模型（见 docs/components/dzweb.md）：
/// - schedule_event_shm_preload / on_event_shm_timer / tick_due 仅在监听线程调用
///   （schedule 经 register_raw handler、tick 由 run() 驱动）；maintain_writer_shm 为
///   static，在 IO 线程执行（或测试直调）
/// - reader 半边在监听线程就地执行（reader_ 归监听线程独占）
/// - writer 半边投递到 IO 线程 getIOLoop(0) 执行（MultiWriter 全部操作与 REST/WS 写帧
///   同线程串行，E1 修复确立的归属；启动窗口兜底主循环——FIFO 保证先于 startListening，
///   零连接期完成，依据同 control_domain_service.h）
class EventChannelPreloader {
public:
    EventChannelPreloader(std::shared_ptr<shm::Reader> reader,
                          std::shared_ptr<shm::MultiWriter> event_writer);

    /// 随机延迟后排定一次预加载。tag 固定 "event_shm_maint"：连续广播自动替换，
    /// 只保留最新一次（对齐 md_api_scheduled.cpp 的 timer_queue_ 语义）。
    /// delay 由调用方注入（生产传 core::random_jitter(0,5000)，测试传 0 便于同步断言）。
    void schedule_event_shm_preload(const DzShmPreload& params,
                                    std::chrono::milliseconds delay);

    /// TimerQueue 到期回调：reader 半边就地执行 + writer 半边投递 IO 线程。
    /// 名字对齐 md_api_scheduled.cpp 的 on_event_shm_timer(params)。
    void on_event_shm_timer(const DzShmPreload& params);

    /// writer 半边四件套。static：投递 lambda 不捕获 this（仅按值持 shared_ptr 保活），
    /// 进程退出期即使投递任务悬挂在未运行的循环里也因不捕获 this 而自然无害；
    /// 同时供单元测试直调。
    static void maintain_writer_shm(shm::MultiWriter& writer, const DzShmPreload& params);

    // === 监听线程 run() 驱动（模式对齐 td_api.cpp 主循环的 timer_queue 用法）===

    /// 队列是否为空（true 时 run() 用 wait() 无限等，否则 wait_for(next_wait_ms())）
    [[nodiscard]] bool idle() const { return timer_queue_.empty(); }

    /// 最近到期定时器的剩余毫秒数（前置条件: !idle()；已到期返回 0）
    [[nodiscard]] uint32_t next_wait_ms() const;

    /// 触发所有已到期定时器（空队列安全）
    void tick_due();

private:
    /// 投递到 IO 循环 getIOLoop(0)（与 REST/WS 同线程）；启动窗口兜底投主循环。
    /// 逐字对齐 control_domain_service.h 的 post_to_io_loop。
    static void post_to_io_loop(std::function<void()> f);

    core::TimerQueue timer_queue_;
    std::shared_ptr<shm::Reader> reader_;             ///< 监听线程独占
    std::shared_ptr<shm::MultiWriter> event_writer_;  ///< IO 线程使用
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_EVENT_PRELOADER_H_
