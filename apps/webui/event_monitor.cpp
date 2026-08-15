#include "event_monitor.h"
#include "frame_router.h"

#include <dztrader/shm/frame_view.h>
#include <dztrader/core/this_process.h>
#include <spdlog/spdlog.h>
#include <chrono>

namespace dztrader::webui {

EventMonitor::EventMonitor(const std::filesystem::path& shm_dir, FrameRouter& router)
    : router_(router) {
    // 内部创建 SHM Reader（与 ShmWriter 模式一致：接收 shm_dir，内部 open channel）
    // 方便失败时降级：共享内存不存在时 reader_ 为空，start() 走 no-op（API-only 模式）
    try {
        auto meta = std::make_shared<shm::ChannelMeta>(
            shm::ChannelMeta::open_only(shm::channel_name("dzevent"), shm_dir));
        reader_ = std::make_shared<shm::Reader>(
            shm::Reader::create(shm::channel_name("dzevent"), shm_dir, "dzweb"));
    } catch (const std::exception& e) {
        SPDLOG_WARN("shm reader init failed | error={} mode=api-only advice=start_dztraderd",
                    e.what());
    }
}

EventMonitor::~EventMonitor() { stop(); }

void EventMonitor::start() {
    if (!reader_) {
        SPDLOG_INFO("event monitor skipped | reason=no_reader mode=api-only");
        return;
    }

    // 信号量/reader 名 = "dzweb" (无 pid 后缀), 与 master make_subscriber_name 注册名一致
    std::string sem_name = dztrader::this_process::exe_stem();
    sem_.emplace(sem_name);
    SPDLOG_INFO("event monitor starting | sem_name={}", sem_name);

    thread_ = std::thread([this]() {
        SPDLOG_INFO("event channel monitor thread started | mode=semaphore-driven");
        run();
        SPDLOG_INFO("event channel monitor thread exited");
    });
}

void EventMonitor::stop() {
    stop_flag_.store(true, std::memory_order_release);
    if (sem_) {
        sem_->notify();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void EventMonitor::run() {
    auto last_release = std::chrono::steady_clock::now();
    while (!stop_flag_.load(std::memory_order_acquire)) {
        sem_->wait();
        if (stop_flag_.load(std::memory_order_acquire)) {
            break;
        }
        drain_and_post();
        // 周期上报 reader 页索引 (60s): master 的 PageCleaner 以 min(reader page_index, 活跃页)
        // 为删除下限; webui 不上报则下限恒为 0, 事件通道页文件永不删除 (磁盘只增不减)。
        auto now = std::chrono::steady_clock::now();
        if (now - last_release >= std::chrono::seconds(60)) {
            if (reader_) {
                reader_->release_old_pages();
            }
            last_release = now;
        }
    }
}

void EventMonitor::drain_and_post() {
    for (;;) {
        const std::byte* raw = reader_->next_frame();
        if (!raw) {
            break;
        }
        const shm::FrameView view(raw);

        // 单帧处理失败（decode/handler 异常）在此统一兜底，不影响后续帧与监听线程
        try {
            router_.dispatch(view);
        } catch (const std::exception& e) {
            SPDLOG_WARN("decode frame failed | type={} error={}", static_cast<int>(view.type()),
                        e.what());
        }
    }
}

}  // namespace dztrader::webui
