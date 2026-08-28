/**
 * @file strategy_context.h
 * @brief 策略运行时上下文（DzContext 内部实现，不暴露给 C API 头文件）
 */
#ifndef DZTRADER_STRATEGY_API_STRATEGY_CONTEXT_H_
#define DZTRADER_STRATEGY_API_STRATEGY_CONTEXT_H_

#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/writer.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/order_id_meta.h>
#include <dztrader/core/env.h>
#include <dztrader/core/exception.h>
#include <dztrader/core/string_util.h>
#include <dztrader/core/this_process.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/path.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/timer_queue.h>
#include <dztrader/error.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace dztrader {

/// 策略进程统一身份名: stg.<strategy_id>（无 pid 后缀, 重启复用同名, 身份稳定）。
/// 与 master make_subscriber_name (process_supervisor.cpp) 的 Strategy 分支一致,
/// 是事件通道 reader/writer/信号量名与行情订阅 instance_id 的唯一构造点。
inline std::string strategy_identity(const std::string& strategy_id) {
    return std::format("{}.{}", STRATEGY_PREFIX, strategy_id);
}

}  // namespace dztrader

/// 策略运行环境（api.h 不透明句柄 DzContext 的实现体）。
/// 全局作用域 tag: C typedef `typedef struct DzContext DzContext` 必须指向本类型。
struct DzContext {
    DzContext(const std::string& strategy_id,
              const std::string& md_source_name,
              const std::string& chn_event_name,
              const std::filesystem::path& chn_event_dir,
              const std::string& chn_order_id_name,
              const std::filesystem::path& chn_order_id_dir,
              const std::filesystem::path& chn_md_dir)
        : sem{dztrader::strategy_identity(strategy_id)},
          md_reader{dztrader::shm::Reader::create(
              checked_md_source_name(md_source_name), chn_md_dir,
              dztrader::strategy_identity(strategy_id))},
          md_source_name{},
          event_reader{dztrader::shm::Reader::create(chn_event_name, chn_event_dir,
                                                      dztrader::strategy_identity(strategy_id))},
          event_writer{dztrader::shm::MultiWriter::create(chn_event_name, chn_event_dir,
                                                           dztrader::strategy_identity(strategy_id))},
          order_id{dztrader::shm::OrderIdMeta::open_or_create(chn_order_id_name, chn_order_id_dir)} {
        dztrader::copy_string(this->md_source_name, checked_md_source_name(md_source_name).c_str(), true);
        dztrader::copy_string(this->strategy_id, strategy_id.c_str(), true);
        strategy_home = dztrader::this_process::exe_dir().string();
    }

    DzContext()
        : DzContext(dztrader::this_process::exe_stem(),
                    default_md_source(),
                    dztrader::CHANNEL_NAME_EVENT,
                    dztrader::paths::shm(),
                    dztrader::CHANNEL_NAME_ORDER_ID,
                    dztrader::paths::shm(),
                    dztrader::paths::shm()) {}

    // ── 第一热区：等待 + 行情 ────────────────────────────────
    dztrader::shm::NamedSemaphore sem;
    dztrader::shm::Reader md_reader;
    DzInstanceId md_source_name{};

    // ── 第二热区：事件读取 ──────────────────────────────────
    dztrader::shm::Reader event_reader;

    // ── 第三热区：事件写入 / 交易 ────────────────────────────
    dztrader::shm::MultiWriter event_writer;
    dztrader::shm::OrderIdMeta order_id;
    DzStrategyId strategy_id{};

    // ── 冷区 ────────────────────────────────────────────────
    std::string strategy_home;
    std::set<std::string> md_desired_instruments;

    // ── 定时器区 ────────────────────────────────────────────
    // 用户定时器与 SDK 内部任务(预加载随机延迟)共用单队列:
    // 热路径(dz_wait/dz_next_event)只做 empty()/next_timeout()/tick(),
    // 调度与取消为冷路径, 防误删靠内部 ID 对用户不可见。
    dztrader::core::TimerQueue timers;

    /// 用户定时器注册表: 稳定 ID -> 状态。周期定时器重排会更换队列条目,
    /// 但稳定 ID 不变, 用户始终用首次返回的 ID 取消。
    struct UserTimerEntry {
        enum class Kind : uint8_t { After, Every, AtOnce, Daily };
        Kind kind = Kind::After;
        int32_t time_of_day_ms = 0;  ///< AtOnce/Daily: 距午夜毫秒 (Daily 重排用)
        std::chrono::milliseconds interval{};  ///< Every: 触发间隔
        /// Every: 上次到期点 + interval 重排的基准 (漂移无关);
        /// After/AtOnce/Daily: 首次/下次到期点
        dztrader::core::TimerQueue::TimePoint next_deadline{};
        dztrader::core::TimerQueue::TimerId queue_id = 0;  ///< 当前队列条目 (重排会换)
    };
    std::unordered_map<DzTimerId, UserTimerEntry> user_timers;
    DzTimerId next_user_timer_id = 1;

    /// SDK 内部预加载定时器: tag -> 当前队列 ID (替换语义, 只保留最新)。
    /// 值即内部 ID 全集, 对用户 API 不可见, cancel/cancel_all 结构上无法误删。
    std::unordered_map<std::string, dztrader::core::TimerQueue::TimerId> internal_preload_tags;

    /// 本地定时器帧环形缓冲: 模拟 shm 帧布局 (DzFrameHeader + DzTimerEvent),
    /// 不写真实共享内存; 指针有效期至下一次 dz_next_event/dz_release。
    static constexpr uint32_t kTimerFrameSlots = 32;
    struct alignas(8) TimerFrameSlot {
        DzFrameHeader header;
        DzTimerEvent payload;
    };
    static_assert(sizeof(TimerFrameSlot) == 16, "timer frame slot must be 16 bytes");
    std::array<TimerFrameSlot, kTimerFrameSlots> timer_frames{};
    uint32_t timer_frame_head = 0;   ///< 下一个可写槽位
    uint32_t timer_frame_count = 0;  ///< 待领取帧数

    /// 缓冲满时推迟投递的触发 (稳定 ID, 保序)。领取腾槽时逐条补投, 绝不丢帧;
    /// 已取消 ID 在补投时按注册表过滤 (取消语义覆盖)。
    std::vector<DzTimerId> deferred_fires;

    // ── 定时器机制 (定义见 api.cpp, 与 dz_wait/dz_next_event 同 TU 便于内联) ──
    void tick_timers();
    [[nodiscard]] bool has_pending_timers() const noexcept { return !timers.empty(); }
    /// 最近到期定时器的剩余等待毫秒数 (前置条件: !timers.empty())
    [[nodiscard]] uint32_t next_timer_wait_ms() const;
    /// 取下一帧本地定时器帧 (缓冲空且无 deferred 时返回 nullptr)
    [[nodiscard]] const void* pop_timer_frame();
    void deliver_timer_frame(DzTimerId timer_id);
    void on_user_timer_fire(DzTimerId timer_id);
    void rearm_user_timer(std::unordered_map<DzTimerId, UserTimerEntry>::iterator it,
                          dztrader::core::TimerQueue::TimePoint now);
    DzTimerId schedule_user_timer(UserTimerEntry entry);
    bool cancel_user_timer(DzTimerId timer_id);
    void cancel_all_user_timers();
    /// 内部任务排期 (tag 去重, 替换语义; action 自行保证不抛异常)
    void schedule_internal_preload(const std::string& tag, std::chrono::milliseconds delay,
                                   std::function<void()> action);
    /// SHUTDOWN 时 SDK 内部资源清理 (可扩展: 将来新增清理动作在此追加)
    void internal_cleanup_on_shutdown();

private:
    static const std::string& checked_md_source_name(const std::string& name) {
        if (name.empty()) {
            throw dztrader::Exception(DZ_EC_MD_SOURCE_NOT_CONFIGURED,
                                      "DZTRADER_MD_SOURCE is empty");
        }
        if (name.size() >= sizeof(DzInstanceId)) {
            throw dztrader::Exception(DZ_EC_INVALID_PARAM,
                                      "market source name too long (max 63)");
        }
        for (char c : name) {
            if (c == '/' || c == '\\' || (static_cast<unsigned char>(c) < 0x20)) {
                throw dztrader::Exception(DZ_EC_INVALID_PARAM,
                                          "market source name contains invalid character");
            }
        }
        return name;
    }

    static std::string default_md_source() {
        auto value = dztrader::env::get("DZTRADER_MD_SOURCE");
        if (!value || value->empty()) {
            throw dztrader::Exception(DZ_EC_MD_SOURCE_NOT_CONFIGURED,
                                      "DZTRADER_MD_SOURCE is not set");
        }
        return *value;
    }
};

#endif  // DZTRADER_STRATEGY_API_STRATEGY_CONTEXT_H_
