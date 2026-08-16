#include "md/md_api.h"

#include <chrono>

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include <dztrader/core/random.h>
#include <dztrader/data_type.h>
#include <dztrader/struct.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/log/log.h>
#include <dztrader/platform/frame_codec.h>

namespace dztrader::ctp {

using namespace dztrader::shm;

// md_api_scheduled.cpp: 定时任务
// - 事件通道 SHM 预加载 (被动, 由 DZ_FRAME_PRELOAD_EVENT_SHM 触发)
// - 行情数据通道 SHM 维护 (主动, 周期定时器)
// - 自动调度 (登录/登出, 对齐分钟 25 秒)

void MdApi::schedule_event_shm_preload(const DzShmPreload& params) {
    // 随机延迟 0-5 秒, 避免多进程同时文件 I/O
    auto delay_ms = core::random_jitter(0, 5000);
    SPDLOG_DEBUG("event shm preload scheduled | delay={}ms pages={} bytes={}", delay_ms.count(),
                 params.pages, params.bytes);
    timer_queue_.schedule_after_replace(
        "event_shm_maint", delay_ms, [this, params]() { on_event_shm_timer(params); });
}

void MdApi::on_event_shm_timer(const DzShmPreload& params) {
    try {
        // 事件通道 reader_ 维护
        if (params.pages > 0) {
            reader_.prefetch_pages(params.pages);
        }
        if (params.bytes > 0) {
            reader_.prefetch_for_bytes(params.bytes);
        }
        reader_.release_old_pages();
        reader_.touch_read_position();

        // 事件通道 event_writer_ 维护
        if (params.pages > 0) {
            event_writer_.prefetch_pages(params.pages);
        }
        if (params.bytes > 0) {
            event_writer_.prefetch_for_bytes(params.bytes);
        }
        event_writer_.close_old_pages();
        event_writer_.touch_write_position();

        SPDLOG_DEBUG("event shm preload done | pages={} bytes={}", params.pages,
                     params.bytes);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("event shm timer failed | error=\"{}\"", e.what());
    }
}

void MdApi::schedule_md_shm_maintenance() {
    if (md_shm_config_.check_interval_min() <= 0) {
        return;
    }
    auto delay = std::chrono::minutes(md_shm_config_.check_interval_min());
    timer_queue_.schedule_after_replace("md_shm_maint", delay,
                                        [this]() { on_md_shm_timer(); });
}

void MdApi::maintain_md_shm(uint32_t pages, uint64_t bytes) {
    // 每次 spi_.xxx() 调用各自加 SpinLock (见 md_spi.cpp 实现),
    // 不是整体一个锁 -- 这样 SPI 线程在两次维护操作之间有机会写入 tick。
    if (pages > 0) {
        spi_.prefetch_pages(pages);
    }
    if (bytes > 0) {
        spi_.prefetch_for_bytes(bytes);
    }
    spi_.close_old_pages();
    spi_.touch_write_position();
    DzShmPreload params{};
    params.pages = pages;
    params.bytes = bytes;
    params.reserved = 0;
    broadcast_md_preload(params);
}

void MdApi::on_md_shm_timer() {
    try {
        maintain_md_shm(md_shm_config_.check_pages(), md_shm_config_.check_bytes());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("md shm timer failed | error=\"{}\"", e.what());
    }
    try {
        schedule_md_shm_maintenance();  // 无论成功失败都重排
    } catch (const std::exception& e) {
        SPDLOG_ERROR("schedule_md_shm_maintenance failed | error=\"{}\"", e.what());
    }
}

void MdApi::broadcast_md_preload(const DzShmPreload& params) {
    // 带 instance_id 的广播帧: 订阅了本行情源 (name_) 的进程收到后预加载对应 md 通道
    const auto* raw = reinterpret_cast<const std::byte*>(&params);
    platform::write_ext_inst_raw(event_writer_, DZ_FRAME_PRELOAD_MD_SHM, name_,
                                 raw, static_cast<uint32_t>(sizeof(params)));
    SPDLOG_DEBUG("md shm preload broadcast | instance={} pages={} bytes={}", name_,
                 params.pages, params.bytes);
}

void MdApi::schedule_auto_sched_timer() {
    // 计算到下个分钟 25 秒的延迟 (错峰: 避开整分钟 0 秒的繁忙时段)
    auto now = DateTime::local_now();
    int current_sec = now.second();
    int delay_sec = 0;
    if (current_sec < 25) {
        delay_sec = 25 - current_sec;  // 当前分钟的 25 秒还没到
    } else {
        delay_sec = (60 - current_sec) + 25;  // 到下个分钟的 25 秒
    }

    timer_queue_.schedule_after_replace("auto_sched", std::chrono::seconds(delay_sec),
                                        [this]() { on_sched_timer(); });
}

void MdApi::on_sched_timer() {
    try {
        // 读 system_clock 获取当前本地时间 (用户需确保 OS 时区与交易所时区一致)
        auto now = DateTime::local_now();
        auto hh_mm = now.to_string("%H:%M");
        int weekday = static_cast<int>(now.weekday());  // ISO 8601: 1=周一...7=周日

        // 评估动作 (每次唤醒全量重评估, 不依赖上次状态)
        // 排程单一真相源：auto_login_config_（契约 auto-login 迁移完成）
        bool is_logged_in = (state_machine_.state() == MdState::LoggedIn);
        auto sched = to_sched_view(auto_login_config_.config());
        auto action = evaluate_sched_action(sched, weekday, hh_mm, is_logged_in);

        switch (action) {
            case AutoSchedAction::Login:
                SPDLOG_INFO("auto sched login | time={} weekday={}", hh_mm, weekday);
                if (state_machine_.state() == MdState::Idle) {
                    connect();           // 需要先建立前置连接
                } else if (state_machine_.state() == MdState::Connected) {
                    req_user_login();    // 前置已连接, 直接重新登录 (服务器登出恢复路径)
                } else {
                    SPDLOG_WARN("auto sched login skipped | state={}",
                                magic_enum::enum_name(state_machine_.state()));
                }
                break;
            case AutoSchedAction::Logout:
                SPDLOG_INFO("auto sched logout | time={} weekday={}", hh_mm, weekday);
                disconnect();
                break;
            case AutoSchedAction::None:
                break;
        }

        // 时间点预加载: 检查当前 HH:MM 是否匹配 preload_points
        for (const auto& p : md_shm_config_.preload_points()) {
            if (p.time == hh_mm) {
                SPDLOG_INFO("md shm preload point matched | time={} pages={} bytes={}", p.time,
                            p.pages, p.bytes);
                maintain_md_shm(p.pages, p.bytes);
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("sched timer failed | error=\"{}\"", e.what());
    }
    // 重排: 重新对齐到下个分钟 25 秒 (避免回调耗时累积导致漂移)
    try {
        schedule_auto_sched_timer();  // 无论成功失败都重排
    } catch (const std::exception& e) {
        SPDLOG_ERROR("schedule_auto_sched_timer failed | error=\"{}\"", e.what());
    }
}

}  // namespace dztrader::ctp
