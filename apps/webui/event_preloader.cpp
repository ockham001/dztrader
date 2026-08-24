#include "event_preloader.h"

#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <functional>
#include <limits>

namespace dztrader::webui {

EventChannelPreloader::EventChannelPreloader(std::shared_ptr<shm::Reader> reader,
                                             std::shared_ptr<shm::MultiWriter> event_writer)
    : reader_(std::move(reader)), event_writer_(std::move(event_writer)) {}

void EventChannelPreloader::schedule_event_shm_preload(const DzShmPreload& params,
                                                       std::chrono::milliseconds delay) {
    SPDLOG_DEBUG("event shm preload scheduled | delay={}ms pages={} bytes={}", delay.count(),
                 params.pages, params.bytes);
    timer_queue_.schedule_after_replace(
        "event_shm_maint", delay, [this, params]() { on_event_shm_timer(params); });
}

void EventChannelPreloader::on_event_shm_timer(const DzShmPreload& params) {
    // reader 半边: 监听线程独占, 就地维护 (对齐 md_api_scheduled.cpp on_event_shm_timer)
    try {
        if (reader_) {
            if (params.pages > 0) {
                reader_->prefetch_pages(params.pages);
            }
            if (params.bytes > 0) {
                reader_->prefetch_for_bytes(params.bytes);
            }
            reader_->release_old_pages();
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("event shm reader half failed | error=\"{}\"", e.what());
    }

    // writer 半边: 投递 IO 线程执行 (与全部写帧同线程串行); 按值捕获 shared_ptr 保活,
    // 不捕获 this —— 进程退出期悬挂定时器自然失效
    post_to_io_loop([writer = event_writer_, params]() {
        try {
            if (writer) {
                maintain_writer_shm(*writer, params);
                SPDLOG_DEBUG("event shm preload done | pages={} bytes={}", params.pages,
                             params.bytes);
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("event shm writer half failed | error=\"{}\"", e.what());
        }
    });
}

void EventChannelPreloader::maintain_writer_shm(shm::MultiWriter& writer,
                                                const DzShmPreload& params) {
    if (params.pages > 0) {
        writer.prefetch_pages(params.pages);
    }
    if (params.bytes > 0) {
        writer.prefetch_for_bytes(params.bytes);
    }
    writer.close_old_pages();
    writer.touch_write_position();
}

uint32_t EventChannelPreloader::next_wait_ms() const {
    // 模式对齐 td_api.cpp 主循环: native duration → ms, cast + clamp 防 uint32 溢出
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(timer_queue_.next_timeout())
            .count();
    constexpr auto kMaxMs = static_cast<long long>(std::numeric_limits<uint32_t>::max());
    return ms >= kMaxMs ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(ms);
}

void EventChannelPreloader::tick_due() {
    if (!timer_queue_.empty()) {
        timer_queue_.tick();
    }
}

void EventChannelPreloader::post_to_io_loop(std::function<void()> f) {
    auto* io_loop = drogon::app().getIOLoop(0);
    if (io_loop != nullptr) {
        io_loop->queueInLoop(std::move(f));
    } else {
        // 启动窗口兜底(run() 前 IO 循环未建): FIFO 保证早于 startListening 排队的任务在
        // 监听器开启前执行完, 零连接期无并发写帧, 安全 (依据同 control_domain_service.h)
        drogon::app().getLoop()->queueInLoop(std::move(f));
    }
}

}  // namespace dztrader::webui
