#include "shm_manager.h"

#include <dztrader/date_time/date_time.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/struct.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace dztrader::master {

void ShmManager::cleanup_old_pages() {
    if (!cleaner_) {
        return;
    }
    try {
        // 先上报 event reader 当前页索引 (卸载旧页映射 + 写 meta), 再清理:
        // 清理下限 = min(各 reader page_index, 活跃页), master 不上报则下限恒为 0,
        // 页文件永不删除 (磁盘只增不减)。
        event_reader_.release_old_pages();
        size_t deleted = cleaner_->cleanup();
        if (deleted > 0) {
            SPDLOG_INFO("event channel pages cleaned | count={}", deleted);
        }
        // 同时清理所有 md channel (已关闭通道 meta 为空, 跳过: 无写入者无新页,
        // 数据文件保留待重启复用, 重开后恢复清理)
        for (auto& [name, state] : md_channels_) {
            if (!state.meta) {
                continue;
            }
            shm::PageCleaner md_cleaner(state.meta, cleanup_policy_);
            size_t d = md_cleaner.cleanup();
            if (d > 0) {
                SPDLOG_INFO("md channel pages cleaned | channel={} count={}", name, d);
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("cleanup_old_pages failed | error=\"{}\"", e.what());
    }
}

void ShmManager::run_event_maintenance_tick() {
    // 1. prefetch_pages (master 自己写的页预热)
    if (event_shm_config_.check_pages() > 0) {
        event_writer_.prefetch_pages(static_cast<uint64_t>(event_shm_config_.check_pages()));
    }
    // 2. prefetch_for_bytes (按字节预热)
    if (event_shm_config_.check_bytes() > 0) {
        event_writer_.prefetch_for_bytes(event_shm_config_.check_bytes());
    }
    // 3. cleanup (清理旧页, event + md 通道)
    cleanup_old_pages();
    // 4. 广播 PRELOAD_EVENT_SHM (携带 check_pages/check_bytes 通知子进程预加载 event 通道)
    DzShmPreload params{};
    params.pages = event_shm_config_.check_pages();
    params.bytes = event_shm_config_.check_bytes();
    params.reserved = 0;
    platform::write_struct(event_writer_, DZ_FRAME_PRELOAD_EVENT_SHM, params);
    SPDLOG_DEBUG("event maintenance tick | check_pages={} check_bytes={}",
                 event_shm_config_.check_pages(), event_shm_config_.check_bytes());
}

void ShmManager::start_event_shm_maintenance(boost::asio::io_context& ioc) {
    if (!ioc_) ioc_ = &ioc;

    // 1. event 通道维护定时器 (周期: check_interval_min 分钟, 0 = 不周期检查)
    if (event_maintenance_timer_) {
        SPDLOG_INFO("event maintenance already started");
    } else if (event_shm_config_.check_interval_min() <= 0) {
        SPDLOG_INFO("event maintenance disabled | check_interval_min=0");
    } else {
        auto interval_min = event_shm_config_.check_interval_min();
        event_maintenance_timer_ =
            std::make_unique<boost::asio::steady_timer>(ioc, std::chrono::minutes(interval_min));
        auto recycle = std::make_shared<std::function<void(const boost::system::error_code&)>>();
        *recycle = [this, recycle](const boost::system::error_code& ec) {
            if (ec) return;  // 定时器被取消
            try {
                run_event_maintenance_tick();
            } catch (const std::exception& e) {
                SPDLOG_WARN("event maintenance failed | error=\"{}\"", e.what());
            }
            // 重新调度 (若 check_interval_min 运行期变为 0, 停止定时器)
            if (event_shm_config_.check_interval_min() <= 0) {
                SPDLOG_INFO("event maintenance stopped (check_interval_min=0)");
                event_maintenance_timer_->cancel();
                event_maintenance_timer_.reset();
                return;
            }
            event_maintenance_timer_->expires_after(
                std::chrono::minutes(event_shm_config_.check_interval_min()));
            event_maintenance_timer_->async_wait(*recycle);
        };
        event_maintenance_timer_->async_wait(*recycle);
        SPDLOG_INFO("event maintenance started | interval_min={} preload_points={}", interval_min,
                    event_shm_config_.preload_points().size());
    }

    // 2. preload_points 检查定时器 (每分钟触发, 独立于 check_interval_min)
    if (preload_points_timer_) {
        SPDLOG_INFO("preload points timer already started");
        return;
    }
    preload_points_timer_ =
        std::make_unique<boost::asio::steady_timer>(ioc, std::chrono::minutes(1));
    auto preload_recycle =
        std::make_shared<std::function<void(const boost::system::error_code&)>>();
    *preload_recycle = [this, preload_recycle](const boost::system::error_code& ec) {
        if (ec) return;
        try {
            on_event_preload_points_tick();
        } catch (const std::exception& e) {
            SPDLOG_WARN("preload points tick failed | error=\"{}\"", e.what());
        }
        preload_points_timer_->expires_after(std::chrono::minutes(1));
        preload_points_timer_->async_wait(*preload_recycle);
    };
    preload_points_timer_->async_wait(*preload_recycle);
}

void ShmManager::reschedule_event_shm_maintenance() {
    // check_interval_min 变为 0: 停止定时器
    if (event_shm_config_.check_interval_min() <= 0) {
        if (event_maintenance_timer_) {
            event_maintenance_timer_->cancel();
            event_maintenance_timer_.reset();
            SPDLOG_INFO("event maintenance stopped (check_interval_min=0)");
        }
        return;
    }
    // check_interval_min > 0 但 timer 未启动 (start 时为 0, 现在变为非 0):
    // 不在此创建 (创建需 io_context, reschedule 无 ioc 参数, 由下次 start 或重启时创建)
    if (!event_maintenance_timer_) {
        SPDLOG_WARN("event maintenance timer not started, cannot reschedule");
        return;
    }
    auto interval_min = event_shm_config_.check_interval_min();
    // 手动重启 recycle lambda (cancel + 等 recycle 自动恢复会因 `if (ec) return` 终止链)
    event_maintenance_timer_->expires_after(std::chrono::minutes(interval_min));

    auto recycle = std::make_shared<std::function<void(const boost::system::error_code&)>>();
    *recycle = [this, recycle](const boost::system::error_code& ec) {
        if (ec) return;  // 定时器被取消
        try {
            run_event_maintenance_tick();
        } catch (const std::exception& e) {
            SPDLOG_WARN("event maintenance failed | error=\"{}\"", e.what());
        }
        if (event_shm_config_.check_interval_min() <= 0) {
            SPDLOG_INFO("event maintenance stopped (check_interval_min=0)");
            event_maintenance_timer_->cancel();
            event_maintenance_timer_.reset();
            return;
        }
        event_maintenance_timer_->expires_after(
            std::chrono::minutes(event_shm_config_.check_interval_min()));
        event_maintenance_timer_->async_wait(*recycle);
    };
    event_maintenance_timer_->async_wait(*recycle);
    SPDLOG_INFO("event maintenance rescheduled | interval_min={}", interval_min);
}

void ShmManager::on_event_preload_points_tick() {
    // 取当前本地时间 HH:MM (用户按本地时间配置 preload_points, 如 "08:45" 表示 Asia/Shanghai 08:45)
    auto now = dztrader::DateTime::local_now();
    std::string hhmm = now.to_string("%H:%M");

    // 匹配 preload_points
    for (const auto& p : event_shm_config_.preload_points()) {
        if (p.time == hhmm) {
            try {
                // 额外预加载
                if (p.pages > 0) {
                    event_writer_.prefetch_pages(static_cast<uint64_t>(p.pages));
                }
                if (p.bytes > 0) {
                    event_writer_.prefetch_for_bytes(p.bytes);
                }
                // 广播 PRELOAD_EVENT_SHM (携带该时间点的 pages/bytes 通知子进程预加载)
                DzShmPreload params{};
                params.pages = p.pages;
                params.bytes = p.bytes;
                params.reserved = 0;
                platform::write_struct(event_writer_, DZ_FRAME_PRELOAD_EVENT_SHM, params);
                SPDLOG_INFO("event preload point triggered | time={} pages={} bytes={}", p.time,
                            p.pages, p.bytes);
            } catch (const std::exception& e) {
                SPDLOG_WARN("event preload point failed | time={} error=\"{}\"", p.time, e.what());
            }
        }
    }
}

}  // namespace dztrader::master
