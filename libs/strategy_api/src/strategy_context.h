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
#include <dztrader/data_type.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/error.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "timer_heap.h"

namespace dztrader {

/// 策略进程统一身份名: stg.<strategy_id>（无 pid 后缀, 重启复用同名, 身份稳定）。
/// 与 master make_subscriber_name (process_supervisor.cpp) 的 Strategy 分支一致,
/// 是事件通道 reader/writer/信号量名与行情订阅 instance_id 的唯一构造点。
inline std::string strategy_identity(const std::string& strategy_id) {
    return std::format("{}.{}", STRATEGY_PREFIX, strategy_id);
}

/// SDK 内部失败诊断: SDK 无日志模块, 写 stderr 单行。
/// master 以管道捕获子进程 stderr 并逐行转发进其日志 (child_process.cpp
/// "forwarded child stderr", warn 级); 手工运行策略时直接显示在终端。
/// 内部失败不使用 LastError: dz_next_event/dz_wait 无失败返回语义,
/// 设置错误码既无可靠检查时机又会残留误导。LastError 仅用于 API 失败返回。
inline void internal_diag(const std::string& message) noexcept {
    std::fputs("[dzsdk] ", stderr);
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
}

/// wall clock: 到下一个本地时间点(距午夜毫秒)的延迟; 已过/恰好相等 -> 次日
inline std::chrono::milliseconds next_time_of_day_delay(int32_t time_of_day_ms) {
    const int32_t now_tod = DateTime::local_now().millisecs_since_midnight();
    int64_t delta = static_cast<int64_t>(time_of_day_ms) - now_tod;
    if (delta <= 0) {
        delta += 86'400'000;
    }
    return std::chrono::milliseconds{delta};
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
    // 用户定时器与 SDK 内部任务(预加载随机延迟)共用单堆:
    // 热路径(dz_wait/dz_next_event)只做 empty()/top()/pop()，
    // 调度与取消为冷路径; 防误删靠内部令牌与用户稳定 ID 分属不同取值空间。
    // 条目 16B 连续内存; 取消走物理移除(remove_token, O(n) 冷路径) —
    // 取消即消失, dz_wait 无幽灵唤醒; 内部替换同样物理移除(条目 ≤2)。
    dztrader::TimerHeap timers;

    /// 用户定时器注册表: 稳定 ID -> 状态。堆条目只存 {deadline, 稳定 ID}，
    /// 取消 = 删注册表(懒删除); 重排 = 推新堆条目(旧条目已弹出)。
    struct UserTimerEntry {
        enum class Kind : uint8_t { After, Every, AtOnce, Daily };
        Kind kind = Kind::After;
        int32_t time_of_day_ms = 0;  ///< AtOnce/Daily: 距午夜毫秒 (Daily 重排用)
        std::chrono::milliseconds interval{};  ///< Every: 触发间隔
        /// Every: 上次到期点 + interval 重排的基准 (漂移无关);
        /// After/AtOnce/Daily: 首次/下次到期点
        dztrader::TimerHeap::TimePoint next_deadline;
    };
    std::unordered_map<DzTimerId, UserTimerEntry> user_timers;
    DzTimerId next_user_timer_id = 1;

    /// SDK 内部预加载定时器参数槽: 有值 = 存活。触发时按令牌取参数执行后置空;
    /// 连续广播覆盖参数槽并物理替换堆条目 (只保留最新)。
    std::optional<DzShmPreload> internal_event_preload;
    std::optional<DzShmPreload> internal_md_preload;

    /// 本地定时器帧环形缓冲: 模拟 shm 帧布局 (DzFrameHeader + DzTimerEvent),
    /// 不写真实共享内存; 指针有效期至下一次 dz_next_event/dz_release。
    static constexpr uint32_t TIMER_FRAME_SLOTS = 32;
    struct alignas(8) TimerFrameSlot {
        DzFrameHeader header;
        DzTimerEvent payload;
    };
    static_assert(sizeof(TimerFrameSlot) == 16, "timer frame slot must be 16 bytes");
    std::array<TimerFrameSlot, TIMER_FRAME_SLOTS> timer_frames{};
    uint32_t timer_frame_head = 0;   ///< 下一个可写槽位
    uint32_t timer_frame_count = 0;  ///< 待领取帧数

    /// 缓冲满时推迟投递的触发 (稳定 ID, 保序)。领取腾槽时逐条补投, 绝不丢帧;
    /// 已取消 ID 在补投时按注册表过滤 (取消语义覆盖)。
    std::vector<DzTimerId> deferred_fires;

    // ── 定时器机制 (内联实现; 本头文件仅 api.cpp 消费, 与 dz_wait/dz_next_event 同 TU 便于内联) ──
    using TimerClock = dztrader::TimerHeap::Clock;

    /// 内部预加载定时器令牌 (与用户稳定 ID 分属不同取值空间:
    /// 用户 ID 从 1 单调递增, 内部令牌置第 63 位, 结构上不可混淆)
    static constexpr uint64_t INTERNAL_TOKEN_BASE = 1ull << 63;
    static constexpr uint64_t INTERNAL_TOKEN_EVENT = INTERNAL_TOKEN_BASE | 0;
    static constexpr uint64_t INTERNAL_TOKEN_MD = INTERNAL_TOKEN_BASE | 1;

    void tick_timers() {
        if (timers.empty()) {
            return;
        }
        const auto now = TimerClock::now();
        while (!timers.empty() && timers.top().deadline <= now) {
            const dztrader::TimerHeap::Node node = timers.pop();
            if (node.token == INTERNAL_TOKEN_EVENT) {
                if (internal_event_preload.has_value()) {
                    preload_event_channels(*internal_event_preload);
                    internal_event_preload.reset();
                }
                continue;  // 参数槽空: 已被清理/替换 (防御)
            }
            if (node.token == INTERNAL_TOKEN_MD) {
                if (internal_md_preload.has_value()) {
                    preload_md_channel(*internal_md_preload);
                    internal_md_preload.reset();
                }
                continue;  // 参数槽空: 已被清理/替换 (防御)
            }
            // 用户定时器: 注册表判定即为懒删除 (已取消的堆条目在此被丢弃)
            on_user_timer_fire(node.token, now);
        }
    }
    [[nodiscard]] bool has_pending_timers() const noexcept { return !timers.empty(); }
    /// 最近到期定时器的剩余等待毫秒数 (前置条件: !timers.empty())
    [[nodiscard]] uint32_t next_timer_wait_ms() const {
        // 常见路径 = 一次时钟读取 + 截断转换 + 一个分支 ([[unlikely]] 布局提示);
        // 实测 ceil<milliseconds> 比截断慢 ~2.5ns/次, 故不用 ceil 而用分支修正
        // 亚毫秒余量: 截断为 0 且未到期时返回 1ms, 避免 wait_for(0) 空转。
        const auto remaining = timers.top().deadline - TimerClock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        if (ms <= 0) [[unlikely]] {
            return remaining > TimerClock::duration::zero() ? 1u : 0u;
        }
        if (ms >= static_cast<long long>(std::numeric_limits<uint32_t>::max())) [[unlikely]] {
            return std::numeric_limits<uint32_t>::max();
        }
        return static_cast<uint32_t>(ms);
    }
    /// 取下一帧本地定时器帧 (缓冲空且无 deferred 时返回 nullptr)
    [[nodiscard]] const void* pop_timer_frame() {
        // 腾出槽位后优先补投 deferred (缓冲满时推迟的触发), 绝不丢帧
        while (timer_frame_count == 0 && !deferred_fires.empty()) {
            const DzTimerId id = deferred_fires.front();
            deferred_fires.erase(deferred_fires.begin());
            const auto it = user_timers.find(id);
            if (it == user_timers.end()) {
                continue;  // 已取消: 取消语义覆盖, 不再投递
            }
            deliver_timer_frame(id);
            if (it->second.kind == UserTimerEntry::Kind::After ||
                it->second.kind == UserTimerEntry::Kind::AtOnce) {
                user_timers.erase(it);
            }
        }
        if (timer_frame_count == 0) {
            return nullptr;
        }
        const uint32_t idx =
            (timer_frame_head + TIMER_FRAME_SLOTS - timer_frame_count) % TIMER_FRAME_SLOTS;
        --timer_frame_count;
        return &timer_frames[idx];
    }
    void deliver_timer_frame(DzTimerId timer_id) {
        auto& slot = timer_frames[timer_frame_head];
        slot.header.frame_size = sizeof(DzFrameHeader) + sizeof(DzTimerEvent);
        slot.header.frame_type = DZ_FRAME_STG_TIMER;
        slot.header.reserved[0] = 0;
        slot.header.reserved[1] = 0;
        slot.payload.timer_id = timer_id;
        timer_frame_head = (timer_frame_head + 1) % TIMER_FRAME_SLOTS;
        ++timer_frame_count;
    }
    void on_user_timer_fire(DzTimerId timer_id, dztrader::TimerHeap::TimePoint now) {
        const auto it = user_timers.find(timer_id);
        if (it == user_timers.end()) {
            return;  // 懒删除: 已取消/已触发的堆条目在此被丢弃
        }
        if (timer_frame_count >= TIMER_FRAME_SLOTS) {
            // 缓冲满: 推迟投递 (领取腾槽时补投), 一次性保持注册表直到真正投递。
            // 周期/每日先重排后入 deferred: rearm 异常时最坏丢一帧, 定时器不静默死亡。
            if (it->second.kind != UserTimerEntry::Kind::After &&
                it->second.kind != UserTimerEntry::Kind::AtOnce) {
                rearm_user_timer(it, now);
            }
            deferred_fires.push_back(timer_id);
            return;
        }
        deliver_timer_frame(timer_id);
        if (it->second.kind == UserTimerEntry::Kind::After ||
            it->second.kind == UserTimerEntry::Kind::AtOnce) {
            user_timers.erase(it);
            return;
        }
        rearm_user_timer(it, now);
    }
    void rearm_user_timer(std::unordered_map<DzTimerId, UserTimerEntry>::iterator it,
                          dztrader::TimerHeap::TimePoint now) {
        UserTimerEntry& entry = it->second;
        const DzTimerId id = it->first;
        if (entry.kind == UserTimerEntry::Kind::Every) {
            // 到期点 + interval 重排 (不漂移); 停滞期间错过的整周期不补触发
            entry.next_deadline += entry.interval;
            if (entry.next_deadline <= now) {
                entry.next_deadline = now + entry.interval;
            }
            timers.push(entry.next_deadline, id);
            return;
        }
        // Daily: 每次触发按 wall clock 重算次日 (自动适应时区/夏令时)
        entry.next_deadline = now + dztrader::next_time_of_day_delay(entry.time_of_day_ms);
        timers.push(entry.next_deadline, id);
    }
    DzTimerId schedule_user_timer(UserTimerEntry entry) {
        const DzTimerId id = next_user_timer_id++;
        if (id >= (1ull << 62)) {
            // 与内部令牌空间 (第 63 位) 的隔离守卫; 实际不可达 (需 2^62 次调度)
            --next_user_timer_id;
            throw dztrader::Exception(DZ_EC_INTERNAL, "timer id space exhausted");
        }
        timers.push(entry.next_deadline, id);
        user_timers.emplace(id, std::move(entry));
        return id;
    }
    bool cancel_user_timer(DzTimerId timer_id) {
        const auto it = user_timers.find(timer_id);
        if (it == user_timers.end()) {
            return false;
        }
        // 物理移除堆条目 (O(n) 冷路径, 平坦内存线性扫描): 取消即消失,
        // dz_wait 不会被残留条目幽灵唤醒; 堆性质由 remove_token 保证
        timers.remove_token(timer_id);
        user_timers.erase(it);
        return true;
    }
    void cancel_all_user_timers() {
        // 单遍过滤重建 (O(n) 冷路径): 移除全部用户条目, 内部令牌保留
        timers.remove_tokens_below(INTERNAL_TOKEN_BASE);
        user_timers.clear();
        deferred_fires.clear();
        timer_frame_head = 0;
        timer_frame_count = 0;
    }
    /// 内部任务排期 (替换语义: 物理移除旧条目后重排; 参数已写入对应槽)
    void schedule_internal_preload(uint64_t token, std::chrono::milliseconds delay) {
        // 替换语义: 物理移除旧条目 (内部条目 ≤2, 冷路径 O(n)) 后按新随机延迟重排
        timers.remove_token(token);
        timers.push(TimerClock::now() + delay, token);
    }
    /// SHUTDOWN 时 SDK 内部资源清理 (可扩展: 将来新增清理动作在此追加)
    void internal_cleanup_on_shutdown() {
        // 当前动作: 取消内部预加载定时器 + 清本地定时器帧缓冲。
        // 用户定时器不动 (由用户随进程退出自然消亡); 将来新增清理动作在此追加。
        internal_event_preload.reset();
        internal_md_preload.reset();
        timers.remove_token(INTERNAL_TOKEN_EVENT);
        timers.remove_token(INTERNAL_TOKEN_MD);
        timer_frame_head = 0;
        timer_frame_count = 0;
    }

private:
    /// 事件通道预加载 (reader + writer 半边, 对齐 md/td on_event_shm_timer 三件套)
    void preload_event_channels(const DzShmPreload& params) {
        try {
            if (params.pages > 0) {
                event_reader.prefetch_pages(params.pages);
            }
            if (params.bytes > 0) {
                event_reader.prefetch_for_bytes(params.bytes);
            }
            event_reader.release_old_pages();

            if (params.pages > 0) {
                event_writer.prefetch_pages(params.pages);
            }
            if (params.bytes > 0) {
                event_writer.prefetch_for_bytes(params.bytes);
            }
            event_writer.close_old_pages();
        } catch (const dztrader::Exception& e) {
            dztrader::internal_diag(std::string("event channel preload failed: ") + e.what());
        } catch (const std::exception& e) {
            dztrader::internal_diag(std::string("event channel preload failed: ") + e.what());
        } catch (...) {
            dztrader::internal_diag("event channel preload failed: unknown exception");
        }
    }

    /// 行情通道预加载 (策略为纯 reader, 仅 reader 半边)
    void preload_md_channel(const DzShmPreload& params) {
        try {
            if (params.pages > 0) {
                md_reader.prefetch_pages(params.pages);
            }
            if (params.bytes > 0) {
                md_reader.prefetch_for_bytes(params.bytes);
            }
            md_reader.release_old_pages();
        } catch (const dztrader::Exception& e) {
            dztrader::internal_diag(std::string("md channel preload failed: ") + e.what());
        } catch (const std::exception& e) {
            dztrader::internal_diag(std::string("md channel preload failed: ") + e.what());
        } catch (...) {
            dztrader::internal_diag("md channel preload failed: unknown exception");
        }
    }

    static std::string checked_md_source_name(const std::string& name) {
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
