#include <dztrader/api.h>

#include <algorithm>
#include <unordered_set>
#include <string_view>
#include <climits>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <limits>
#include <vector>

#include <dztrader/error.h>
#include <dztrader/data_type.h>
#include <dztrader/core/last_error.h>
#include <dztrader/core/random.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/version.h>

#include "strategy_context.h"
#include "result_set.h"
#include "vector_result_set.h"
#include "cursor_result_set.h"
#include "output_limit.h"

using namespace dztrader;

namespace {

/// 当前会话上下文登记: 仅 dz_init 重复调用检测与 dz_release 清登记使用;
/// 其余函数一律走 ctx 参数 (spec §4.2), 不得引用本变量。
/// 用函数内 static 而非文件级非 const 全局, 满足
/// cppcoreguidelines-avoid-non-const-global-variables。
/// 非线程安全: 生命周期由调用方单线程保证 (api.h 句柄契约)。
DzContext*& context_registry() {
    static DzContext* context = nullptr;
    return context;
}

}  // namespace

/* ── 生命周期 ── */

DZ_API DzContext* dz_init(void) {
    DzContext*& context = context_registry();
    if (context != nullptr) {
        LastError::set(DZ_EC_STRATEGY_ALREADY_INITIALIZED, "dz_init called twice");
        return nullptr;
    }
    try {
        context = new DzContext();  // NOLINT
        return context;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return nullptr;
}

DZ_API void dz_release(DzContext* ctx) {
    // 契约(api.h 句柄契约): dz_release 恰好一次, 且只能传当前会话句柄。
    // NULL 为 no-op; 非当前句柄/重复释放属 UB, 不设防。
    if (ctx == nullptr) {
        return;
    }
    DzContext*& context = context_registry();
    if (ctx == context) {
        context = nullptr;  // 先清登记再 delete
    }
    delete ctx;  // NOLINT
}

/* ── 等待 / 事件读取 ── */

namespace {

using TimerClock = dztrader::TimerHeap::Clock;

/// dz_next_event 单次调用连续消费内部帧的上限 (防内部帧洪峰饿死 dz_next_md)
constexpr uint32_t kMaxInternalFramesPerCall = 32;

/// 内部预加载定时器令牌 (与用户稳定 ID 分属不同取值空间:
/// 用户 ID 从 1 单调递增, 内部令牌置第 63 位, 结构上不可混淆)
constexpr uint64_t kInternalTokenBase = 1ull << 63;
constexpr uint64_t kInternalTokenEvent = kInternalTokenBase | 0;
constexpr uint64_t kInternalTokenMd = kInternalTokenBase | 1;

/// SDK 内部失败诊断: SDK 无日志模块, 写 stderr 单行。
/// master 以管道捕获子进程 stderr 并逐行转发进其日志 (child_process.cpp
/// "forwarded child stderr", warn 级); 手工运行策略时直接显示在终端。
/// 内部失败不使用 LastError: dz_next_event/dz_wait 无失败返回语义,
/// 设置错误码既无可靠检查时机又会残留误导。LastError 仅用于 API 失败返回。
void internal_diag(const std::string& message) noexcept {
    std::fputs("[dzsdk] ", stderr);
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
}

/// wall clock: 到下一个本地时间点(距午夜毫秒)的延迟; 已过/恰好相等 -> 次日
std::chrono::milliseconds next_time_of_day_delay(int32_t time_of_day_ms) {
    const int32_t now_tod = dztrader::DateTime::local_now().millisecs_since_midnight();
    int64_t delta = static_cast<int64_t>(time_of_day_ms) - now_tod;
    if (delta <= 0) {
        delta += 86'400'000;
    }
    return std::chrono::milliseconds{delta};
}

/// 策略用户可见帧白名单: TD 回报 2000-2017 / STG_USER_INPUT / SHUTDOWN(定向本策略)/ALL。
/// 其余帧一律内部消费 (见 handle_internal_frame)。
bool is_user_frame(const DzContext* ctx, DzFrameType type, const std::byte* frame) noexcept {
    if (type >= DZ_FRAME_TD_ORDER_RPT && type <= DZ_FRAME_TD_POSITION_DETAIL) {
        return true;
    }
    if (type == DZ_FRAME_STG_USER_INPUT) {
        return true;
    }
    if (type == DZ_FRAME_REQUEST_SHUTDOWN) {
        // 定向帧: 仅 instance_id == 裸策略名 的属于本策略
        return std::string_view(shm::FrameView(frame).ext_inst_id()) == ctx->strategy_id;
    }
    return false;
}

/// 事件通道预加载 (reader + writer 半边, 对齐 md/td on_event_shm_timer 三件套)
void preload_event_channels(DzContext* ctx, const DzShmPreload& params) {
    try {
        if (params.pages > 0) {
            ctx->event_reader.prefetch_pages(params.pages);
        }
        if (params.bytes > 0) {
            ctx->event_reader.prefetch_for_bytes(params.bytes);
        }
        ctx->event_reader.release_old_pages();

        if (params.pages > 0) {
            ctx->event_writer.prefetch_pages(params.pages);
        }
        if (params.bytes > 0) {
            ctx->event_writer.prefetch_for_bytes(params.bytes);
        }
        ctx->event_writer.close_old_pages();
    } catch (const Exception& e) {
        internal_diag(std::string("event channel preload failed: ") + e.what());
    } catch (const std::exception& e) {
        internal_diag(std::string("event channel preload failed: ") + e.what());
    } catch (...) {
        internal_diag("event channel preload failed: unknown exception");
    }
}

/// 行情通道预加载 (策略为纯 reader, 仅 reader 半边)
void preload_md_channel(DzContext* ctx, const DzShmPreload& params) {
    try {
        if (params.pages > 0) {
            ctx->md_reader.prefetch_pages(params.pages);
        }
        if (params.bytes > 0) {
            ctx->md_reader.prefetch_for_bytes(params.bytes);
        }
        ctx->md_reader.release_old_pages();
    } catch (const Exception& e) {
        internal_diag(std::string("md channel preload failed: ") + e.what());
    } catch (const std::exception& e) {
        internal_diag(std::string("md channel preload failed: ") + e.what());
    } catch (...) {
        internal_diag("md channel preload failed: unknown exception");
    }
}

/// NOTIFY_MD_STARTED 自动补订阅 (定义见 write_subscribe_req 之后)
void on_md_started_internal(DzContext* ctx, const std::byte* frame);

/// 内部帧处理: PRELOAD -> 随机延迟预加载; UPDATE_SUBSCRIBER -> 刷新订阅者;
/// NOTIFY_MD_STARTED -> 自动补订阅; 其余平台帧丢弃。全部不返回给策略用户。
void handle_internal_frame(DzContext* ctx, const std::byte* frame, DzFrameType type) {
    shm::FrameView view(frame);
    switch (type) {
        case DZ_FRAME_PRELOAD_EVENT_SHM: {
            const auto& params = view.payload<DzShmPreload>();
            ctx->internal_event_preload = params;  // 覆盖参数槽 (只保留最新)
            ctx->schedule_internal_preload(kInternalTokenEvent,
                                           dztrader::core::random_jitter(0, 5000));
            return;
        }
        case DZ_FRAME_PRELOAD_MD_SHM: {
            // 仅本策略绑定的行情源 (instance_id = 行情通道名)
            if (std::string_view(view.ext_inst_id()) != ctx->md_source_name) {
                return;
            }
            if (view.ext_inst_payload_size() < sizeof(DzShmPreload)) {
                return;
            }
            const auto& params = *reinterpret_cast<const DzShmPreload*>(view.ext_inst_payload());
            ctx->internal_md_preload = params;  // 覆盖参数槽 (只保留最新)
            ctx->schedule_internal_preload(kInternalTokenMd,
                                           dztrader::core::random_jitter(0, 5000));
            return;
        }
        case DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER:
            ctx->event_writer.refresh_subscribers();
            return;
        case DZ_FRAME_NOTIFY_MD_STARTED:
            on_md_started_internal(ctx, frame);
            return;
        default:
            return;  // 其余平台帧 (日志/SHM 配置/进程控制/TD 控制帧等) 丢弃
    }
}

}  // namespace

/* ── 定时器机制 (DzContext 成员定义) ── */

void DzContext::tick_timers() {
    if (timers.empty()) {
        return;
    }
    const auto now = TimerClock::now();
    while (!timers.empty() && timers.top().deadline <= now) {
        const dztrader::TimerHeap::Node node = timers.pop();
        if (node.token == kInternalTokenEvent) {
            if (internal_event_preload.has_value()) {
                preload_event_channels(this, *internal_event_preload);
                internal_event_preload.reset();
            }
            continue;  // 参数槽空: 已被清理/替换 (防御)
        }
        if (node.token == kInternalTokenMd) {
            if (internal_md_preload.has_value()) {
                preload_md_channel(this, *internal_md_preload);
                internal_md_preload.reset();
            }
            continue;  // 参数槽空: 已被清理/替换 (防御)
        }
        // 用户定时器: 注册表判定即为懒删除 (已取消的堆条目在此被丢弃)
        on_user_timer_fire(node.token, now);
    }
}

uint32_t DzContext::next_timer_wait_ms() const {
    // 前置条件: !timers.empty()
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

void DzContext::deliver_timer_frame(DzTimerId timer_id) {
    auto& slot = timer_frames[timer_frame_head];
    slot.header.frame_size = sizeof(DzFrameHeader) + sizeof(DzTimerEvent);
    slot.header.frame_type = DZ_FRAME_STG_TIMER;
    slot.header.reserved[0] = 0;
    slot.header.reserved[1] = 0;
    slot.payload.timer_id = timer_id;
    timer_frame_head = (timer_frame_head + 1) % kTimerFrameSlots;
    ++timer_frame_count;
}

const void* DzContext::pop_timer_frame() {
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
        (timer_frame_head + kTimerFrameSlots - timer_frame_count) % kTimerFrameSlots;
    --timer_frame_count;
    return &timer_frames[idx];
}

void DzContext::on_user_timer_fire(DzTimerId timer_id, TimerClock::time_point now) {
    const auto it = user_timers.find(timer_id);
    if (it == user_timers.end()) {
        return;  // 懒删除: 已取消/已触发的堆条目在此被丢弃
    }
    if (timer_frame_count >= kTimerFrameSlots) {
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

void DzContext::rearm_user_timer(std::unordered_map<DzTimerId, UserTimerEntry>::iterator it,
                                 TimerClock::time_point now) {
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
    entry.next_deadline = now + next_time_of_day_delay(entry.time_of_day_ms);
    timers.push(entry.next_deadline, id);
}

DzTimerId DzContext::schedule_user_timer(UserTimerEntry entry) {
    const DzTimerId id = next_user_timer_id++;
    if (id >= (1ull << 62)) {
        // 与内部令牌空间 (第 63 位) 的隔离守卫; 实际不可达 (需 2^62 次调度)
        --next_user_timer_id;
        throw Exception(DZ_EC_INTERNAL, "timer id space exhausted");
    }
    timers.push(entry.next_deadline, id);
    user_timers.emplace(id, std::move(entry));
    return id;
}

bool DzContext::cancel_user_timer(DzTimerId timer_id) {
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

void DzContext::cancel_all_user_timers() {
    // 单遍过滤重建 (O(n) 冷路径): 移除全部用户条目, 内部令牌保留
    timers.remove_tokens_below(kInternalTokenBase);
    user_timers.clear();
    deferred_fires.clear();
    timer_frame_head = 0;
    timer_frame_count = 0;
}

void DzContext::schedule_internal_preload(uint64_t token, std::chrono::milliseconds delay) {
    // 替换语义: 物理移除旧条目 (内部条目 ≤2, 冷路径 O(n)) 后按新随机延迟重排
    timers.remove_token(token);
    timers.push(TimerClock::now() + delay, token);
}

void DzContext::internal_cleanup_on_shutdown() {
    // 当前动作: 取消内部预加载定时器 + 清本地定时器帧缓冲。
    // 用户定时器不动 (由用户随进程退出自然消亡); 将来新增清理动作在此追加。
    internal_event_preload.reset();
    internal_md_preload.reset();
    timers.remove_token(kInternalTokenEvent);
    timers.remove_token(kInternalTokenMd);
    timer_frame_head = 0;
    timer_frame_count = 0;
}

DZ_API void dz_wait(DzContext* ctx) {
    if (ctx->has_pending_timers()) {
        const uint32_t timeout_ms = ctx->next_timer_wait_ms();
        static_assert(noexcept(ctx->sem.wait_for(timeout_ms)));
        (void)ctx->sem.wait_for(timeout_ms);
    } else {
        static_assert(noexcept(ctx->sem.wait()));
        ctx->sem.wait();
    }
    try {
        ctx->tick_timers();
    } catch (const std::exception& e) {
        internal_diag(std::string("timer tick failed: ") + e.what());
    } catch (...) {
        internal_diag("timer tick failed: unknown exception");
    }
}

DZ_API const void* dz_next_event(DzContext* ctx) {
    // 处理优先级: 用户帧 > 定时器帧 > 内部帧。
    // 用户帧路径零计时器开销 (不 tick 不 pop); 通道无用户帧 (空/32 让位) 时才
    // tick 定时器并返回定时器帧; 内部帧在扫描用户帧时顺带消费 (轻量处理,
    // 预加载重活已由随机延迟定时器承担)。
    uint32_t internal_count = 0;
    for (;;) {
        static_assert(noexcept(ctx->event_reader.next_frame()));
        const std::byte* frame = ctx->event_reader.next_frame();
        if (frame == nullptr) {
            break;  // 通道空: 服务计时器
        }
        const DzFrameType type = shm::FrameView(frame).type();
        if (is_user_frame(ctx, type, frame)) {
            if (type == DZ_FRAME_REQUEST_SHUTDOWN) {
                ctx->internal_cleanup_on_shutdown();  // 内部清理后再放行
            }
            return frame;  // 用户帧最高优先
        }
        try {
            handle_internal_frame(ctx, frame, type);
        } catch (const std::exception& e) {
            internal_diag(std::string("internal frame handling failed: ") + e.what());
        } catch (...) {
            internal_diag("internal frame handling failed: unknown exception");
        }
        if (++internal_count >= kMaxInternalFramesPerCall) {
            break;  // 连续 32 条内部帧: 让位, 防饿死 dz_next_md
        }
    }
    // 无用户帧可给 (通道空或 32 让位): 服务计时器
    try {
        ctx->tick_timers();
    } catch (const std::exception& e) {
        internal_diag(std::string("timer tick failed: ") + e.what());
    } catch (...) {
        internal_diag("timer tick failed: unknown exception");
    }
    return ctx->pop_timer_frame();
}

DZ_API const void* dz_next_md(DzContext* ctx) {
    static_assert(noexcept(ctx->md_reader.next_frame()));
    return ctx->md_reader.next_frame();
}

DZ_API const char* dz_md_source_name(DzContext* ctx) {
    return ctx->md_source_name;
}

DZ_API void dz_notify_self(DzContext* ctx) { ctx->sem.notify(); }

/* ── 定时器 ── */

namespace {

bool valid_delay_ms(int32_t delay_ms) { return delay_ms > 0; }

bool valid_time_of_day_ms(int32_t time_of_day_ms) {
    return time_of_day_ms >= 0 && time_of_day_ms <= 86'399'999;
}

DzTimerId schedule_relative(DzContext* ctx, int32_t delay_ms,
                            DzContext::UserTimerEntry::Kind kind) {
    try {
        DzContext::UserTimerEntry entry;
        entry.kind = kind;
        if (kind == DzContext::UserTimerEntry::Kind::Every) {
            entry.interval = std::chrono::milliseconds{delay_ms};
        }
        entry.next_deadline = TimerClock::now() + std::chrono::milliseconds{delay_ms};
        return ctx->schedule_user_timer(std::move(entry));
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return DZ_TIMER_INVALID;
}

DzTimerId schedule_time_of_day(DzContext* ctx, int32_t time_of_day_ms,
                               DzContext::UserTimerEntry::Kind kind) {
    try {
        DzContext::UserTimerEntry entry;
        entry.kind = kind;
        entry.time_of_day_ms = time_of_day_ms;
        entry.next_deadline = TimerClock::now() + next_time_of_day_delay(time_of_day_ms);
        return ctx->schedule_user_timer(std::move(entry));
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return DZ_TIMER_INVALID;
}

}  // namespace

DZ_API DzTimerId dz_schedule_after(DzContext* ctx, int32_t delay_ms) {
    if (!valid_delay_ms(delay_ms)) {
        LastError::set(DZ_EC_INVALID_PARAM, "delay_ms must be > 0");
        return DZ_TIMER_INVALID;
    }
    return schedule_relative(ctx, delay_ms, DzContext::UserTimerEntry::Kind::After);
}

DZ_API DzTimerId dz_schedule_every(DzContext* ctx, int32_t delay_ms) {
    if (!valid_delay_ms(delay_ms)) {
        LastError::set(DZ_EC_INVALID_PARAM, "delay_ms must be > 0");
        return DZ_TIMER_INVALID;
    }
    return schedule_relative(ctx, delay_ms, DzContext::UserTimerEntry::Kind::Every);
}

DZ_API DzTimerId dz_schedule_at(DzContext* ctx, int32_t time_of_day_ms) {
    if (!valid_time_of_day_ms(time_of_day_ms)) {
        LastError::set(DZ_EC_INVALID_PARAM, "time_of_day_ms must be in [0, 86_399_999]");
        return DZ_TIMER_INVALID;
    }
    return schedule_time_of_day(ctx, time_of_day_ms, DzContext::UserTimerEntry::Kind::AtOnce);
}

DZ_API DzTimerId dz_schedule_daily(DzContext* ctx, int32_t time_of_day_ms) {
    if (!valid_time_of_day_ms(time_of_day_ms)) {
        LastError::set(DZ_EC_INVALID_PARAM, "time_of_day_ms must be in [0, 86_399_999]");
        return DZ_TIMER_INVALID;
    }
    return schedule_time_of_day(ctx, time_of_day_ms, DzContext::UserTimerEntry::Kind::Daily);
}

DZ_API bool dz_schedule_cancel(DzContext* ctx, DzTimerId timer_id) {
    if (!ctx->cancel_user_timer(timer_id)) {
        LastError::set(DZ_EC_TIMER_NOT_FOUND, "timer not found");
        return false;
    }
    return true;
}

DZ_API bool dz_schedule_cancel_all(DzContext* ctx) {
    ctx->cancel_all_user_timers();
    return true;
}

DZ_API const char* dz_strategy_home(DzContext* ctx) { return ctx->strategy_home.c_str(); }

DZ_API const char* dz_strategy_id(DzContext* ctx) { return ctx->strategy_id; }

/* ── 交易接口 ── */

DZ_API DzOrderId dz_place_order(DzContext* ctx,
                                const char* account_id,
                                const char* instrument_id,
                                DzDirection direction,
                                DzPriceType price_type,
                                double price,
                                DzVolume volume,
                                DzPositionEffect position_effect) {
    try {
        auto* req = reinterpret_cast<DzOrderReq*>(
            ctx->event_writer.open_frame(DZ_FRAME_TD_ORDER_REQ, sizeof(DzOrderReq)));
        if (req == nullptr) {
            // open_frame 失败时已设置 LastError, 直接透传
            return -1;
        }
        const auto order_id = ctx->order_id.generate();
        copy_string(req->strategy_id, ctx->strategy_id, true);
        copy_string(req->account_id, account_id, true);
        copy_string(req->instrument_id, instrument_id, true);
        req->remark[0] = '\0';
        req->direction = direction;
        req->price_type = price_type;
        req->price = price;
        req->volume = volume;
        req->position_effect = position_effect;
        req->order_id = order_id;
        ctx->event_writer.close_frame();
        ctx->event_writer.notify_subscribers();
        return order_id;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return -1;
}

DZ_API bool dz_cancel_order(DzContext* ctx, const char* account_id, DzOrderId order_id) {
    try {
        auto* req = reinterpret_cast<DzOrderCancelReq*>(
            ctx->event_writer.open_frame(DZ_FRAME_TD_ORDER_CANCEL_REQ, sizeof(DzOrderCancelReq)));
        if (req == nullptr) {
            // open_frame 失败时已设置 LastError, 直接透传
            return false;
        }
        req->order_id = order_id;
        copy_string(req->account_id, account_id, true);
        ctx->event_writer.close_frame();
        ctx->event_writer.notify_subscribers();
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return false;
}

namespace {

bool write_subscribe_req(DzContext* ctx, SubscribeReq& req) {
    try {
        if (!shm::write_ext_inst_json(ctx->event_writer, DZ_FRAME_REQUEST_MD_SUBSCRIBE,
                                      ctx->md_source_name, req)) {
            return false;
        }
        ctx->event_writer.notify_subscribers();
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return false;
}

void fill_instruments(SubscribeReq& req, const char* const instruments[], uint32_t count) {
    req.instruments.reserve(count);
    std::unordered_set<std::string_view> seen;
    seen.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (instruments[i] == nullptr) {
            continue;
        }
        std::string_view inst(instruments[i]);
        if (seen.insert(inst).second) {
            req.instruments.emplace_back(inst);
        }
    }
}

}  // namespace

DZ_API bool dz_subscribe(DzContext* ctx,
                         const char* const instruments[],
                         uint32_t count,
                         bool replace_previous) {
    if (instruments == nullptr || count == 0) {
        LastError::set(DZ_EC_INVALID_PARAM, "instruments is null or count is 0");
        return false;
    }

    std::vector<std::string> new_instruments;
    new_instruments.reserve(count);
    {
        std::unordered_set<std::string_view> seen;
        seen.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (instruments[i] == nullptr) {
                continue;
            }
            const std::string_view inst(instruments[i]);
            if (seen.insert(inst).second) {
                new_instruments.emplace_back(inst);
            }
        }
    }
    if (new_instruments.empty()) {
        LastError::set(DZ_EC_INVALID_PARAM, "no valid instruments after dedup");
        return false;
    }

    // 候选期望集合：不直接提交，写帧成功后才替换。
    std::set<std::string> candidate =
        replace_previous ? std::set<std::string>{} : ctx->md_desired_instruments;
    candidate.insert(new_instruments.begin(), new_instruments.end());

    SubscribeReq req;
    req.instance_id = strategy_identity(ctx->strategy_id);
    req.action = SubscribeAction::Subscribe;
    req.replace = replace_previous;
    req.instruments.assign(candidate.begin(), candidate.end());
    if (req.instruments.empty()) {
        LastError::set(DZ_EC_INVALID_PARAM, "no valid instruments after dedup");
        return false;
    }

    if (!write_subscribe_req(ctx, req)) {
        return false;
    }
    ctx->md_desired_instruments = std::move(candidate);
    return true;
}

DZ_API bool dz_unsubscribe(DzContext* ctx,
                           const char* const instruments[],
                           uint32_t count) {
    SubscribeReq req;
    req.instance_id = strategy_identity(ctx->strategy_id);

    if (instruments == nullptr || count == 0) {
        req.action = SubscribeAction::UnsubscribeAll;
        if (!write_subscribe_req(ctx, req)) {
            return false;
        }
        ctx->md_desired_instruments.clear();
        return true;
    }

    req.action = SubscribeAction::Unsubscribe;
    fill_instruments(req, instruments, count);
    if (req.instruments.empty()) {
        LastError::set(DZ_EC_INVALID_PARAM, "no valid instruments after dedup");
        return false;
    }

    std::set<std::string> candidate = ctx->md_desired_instruments;
    for (const auto& inst : req.instruments) {
        candidate.erase(inst);
    }

    if (!write_subscribe_req(ctx, req)) {
        return false;
    }
    ctx->md_desired_instruments = std::move(candidate);
    return true;
}

namespace {

void on_md_started_internal(DzContext* ctx, const std::byte* frame) {
    // dz_next_event 内部自动补订阅: 仅本策略绑定行情源, 全量重放期望集合。
    try {
        const shm::FrameView view(frame);
        if (view.type() != DZ_FRAME_NOTIFY_MD_STARTED) {
            return;  // 防御: handle_internal_frame 已按类型分派
        }
        if (std::string_view(view.ext_inst_id()) != ctx->md_source_name) {
            return;  // 非本策略源，静默忽略
        }
        if (ctx->md_desired_instruments.empty()) {
            return;  // 无需补订阅
        }

        SubscribeReq req;
        req.instance_id = strategy_identity(ctx->strategy_id);
        req.action = SubscribeAction::Subscribe;
        req.replace = true;
        req.instruments.assign(ctx->md_desired_instruments.begin(),
                               ctx->md_desired_instruments.end());
        (void)write_subscribe_req(ctx, req);
    } catch (const Exception& e) {
        internal_diag(std::string("auto resubscribe on md started failed: ") + e.what());
    } catch (const std::exception& e) {
        internal_diag(std::string("auto resubscribe on md started failed: ") + e.what());
    } catch (...) {
        internal_diag("auto resubscribe on md started failed: unknown exception");
    }
}

}  // namespace

/* ── 逻辑持仓 ── */

DZ_API bool dz_set_logical_position(DzContext* ctx,
                                    const char* account_id,
                                    const char* instrument_id,
                                    int32_t net_volume) {
    try {
        auto* pos = reinterpret_cast<DzLogicalPosition*>(
            ctx->event_writer.open_frame(DZ_FRAME_SET_LOGICAL_POSITION, sizeof(DzLogicalPosition)));
        if (pos == nullptr) {
            // open_frame 失败时已设置 LastError, 直接透传
            return false;
        }
        copy_string(pos->account_id, account_id, true);
        copy_string(pos->instrument_id, instrument_id, true);
        copy_string(pos->strategy_id, ctx->strategy_id, true);
        pos->net_volume = net_volume;
        ctx->event_writer.close_frame();
        ctx->event_writer.notify_subscribers();
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return false;
}

namespace {

// DzNotifyLevel -> 字符串, 与 log level 规范全称一致 (契约 notify-ui level 字段)
const char* notify_level_to_string(DzNotifyLevel level) {
    switch (level) {
        case DZ_NOTIFY_INFO:
            return "info";
        case DZ_NOTIFY_WARN:
            return "warning";
        case DZ_NOTIFY_ERROR:
            return "error";
        default:
            return "error";
    }
}

}  // namespace

/* ── UI 通知 ── */

DZ_API bool dz_notify_ui(DzContext* ctx, DzNotifyLevel level, const char* message, bool popup) {
    if (!message) {
        LastError::set(DZ_EC_INVALID_PARAM, "message is null");
        return false;
    }
    if (level != DZ_NOTIFY_INFO && level != DZ_NOTIFY_WARN && level != DZ_NOTIFY_ERROR) {
        LastError::set(DZ_EC_INVALID_PARAM, "level is invalid");
        return false;
    }
    try {
        nlohmann::json payload = {
            {"source", ctx->strategy_id},
            {"level", notify_level_to_string(level)},
            {"message", message},
            {"timestamp", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())},
            {"popup", popup},
        };
        if (!shm::write_ext_json(ctx->event_writer, DZ_FRAME_NOTIFY_UI, payload)) {
            LastError::set(DZ_EC_INTERNAL, "frame write failed");
            return false;
        }
        ctx->event_writer.notify_subscribers();
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return false;
}

DZ_API bool dz_output_ui(DzContext* ctx, const char* data) {
    if (!data) {
        LastError::set(DZ_EC_INVALID_PARAM, "data is null");
        return false;
    }

    try {
        const auto len = strlen(data);
        // 页感知上限
        const auto cap = output_ui_max_payload(ctx->event_writer.page_size());
        if (len > 0 && cap == 0) {
            LastError::set(DZ_EC_BUFFER_TOO_SMALL, "page too small for output frame");
            return false;
        }
        const auto data_len = static_cast<uint32_t>(std::min<uint64_t>(len, cap));
        if (!ctx->event_writer.write_ext_inst_frame(DZ_FRAME_STG_USER_OUTPUT, ctx->strategy_id,
                                              reinterpret_cast<const std::byte*>(data), data_len)) {
            // write_ext_inst_frame 为 noexcept bool: 唯一失败路径是 open_frame 返回 nullptr,
            // 其每条失败分支均已设置 LastError (writer.cpp), 此处直接透传, 不自设错误码
            return false;
        }
        ctx->event_writer.notify_subscribers();
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return false;
}

/* ── 数据库接口 ── */

DZ_API DzDatabase* dz_db_open(const char* path) {
    (void)path;
    LastError::set(DZ_EC_INTERNAL, "db not implemented");
    return NULL;
}
DZ_API bool dz_db_close(DzDatabase* db) {
    (void)db;
    LastError::set(DZ_EC_INTERNAL, "db not implemented");
    return false;
}

/* ── DzResultSet ── */

DZ_API bool dz_resultset_next(DzResultSet* rs) { return rs && rs->impl ? rs->impl->next() : false; }

DZ_API int32_t dz_resultset_status(DzResultSet* rs) {
    return rs && rs->impl ? rs->impl->status() : DZ_EC_INTERNAL;
}

DZ_API uint32_t dz_resultset_column_count(DzResultSet* rs) {
    return rs && rs->impl ? rs->impl->column_count() : 0u;
}

DZ_API DzColumnType dz_resultset_column_type(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->column_type(index) : DZ_COL_TYPE_NULL;
}

DZ_API const char* dz_resultset_column_name(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->column_name(index) : "";
}

DZ_API bool dz_resultset_is_null(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->is_null(index) : true;
}

DZ_API int64_t dz_resultset_get_int64(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->get_int64(index) : INT64_MAX;
}

DZ_API double dz_resultset_get_float64(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->get_float64(index) : DBL_MAX;
}

DZ_API const char* dz_resultset_get_string(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->get_string(index) : "";
}

DZ_API bool dz_resultset_get_bool(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->get_bool(index) : false;
}

DZ_API void dz_resultset_close(DzResultSet* rs) {
    delete rs;  // NOLINT
    rs = nullptr;
}

/* ── 查询接口 ── */

DZ_API DzResultSet* dz_db_query_order(DzDatabase* db,
                                      const char* account_id,
                                      const char* instrument_id) {
    (void)db;
    (void)account_id;
    (void)instrument_id;
    LastError::set(DZ_EC_INTERNAL, "db not implemented");
    return NULL;
}
DZ_API DzResultSet* dz_db_query_trade(DzDatabase* db,
                                      const char* account_id,
                                      const char* instrument_id) {
    (void)db;
    (void)account_id;
    (void)instrument_id;
    LastError::set(DZ_EC_INTERNAL, "db not implemented");
    return NULL;
}
DZ_API DzResultSet* dz_db_query_position(DzDatabase* db,
                                         const char* account_id,
                                         const char* instrument_id) {
    (void)db;
    (void)account_id;
    (void)instrument_id;
    LastError::set(DZ_EC_INTERNAL, "db not implemented");
    return NULL;
}
DZ_API DzResultSet* dz_db_query_trading_account(DzDatabase* db, const char* account_id) {
    (void)db;
    (void)account_id;
    LastError::set(DZ_EC_INTERNAL, "db not implemented");
    return NULL;
}
DZ_API DzResultSet* dz_db_query_commission(DzDatabase* db,
                                           const char* account_id,
                                           const char* instrument_id) {
    (void)db;
    (void)account_id;
    (void)instrument_id;
    LastError::set(DZ_EC_INTERNAL, "db not implemented");
    return NULL;
}
DZ_API DzResultSet* dz_db_query_margin(DzDatabase* db,
                                       const char* account_id,
                                       const char* instrument_id) {
    (void)db;
    (void)account_id;
    (void)instrument_id;
    LastError::set(DZ_EC_INTERNAL, "db not implemented");
    return NULL;
}
DZ_API DzResultSet* dz_db_query_bar(DzDatabase* db,
                                    const char* instrument_id,
                                    int32_t bar_period,
                                    DzAdjustType adjust_type,
                                    DzDate start_date,
                                    DzDate end_date) {
    (void)db;
    (void)instrument_id;
    (void)bar_period;
    (void)adjust_type;
    (void)start_date;
    (void)end_date;
    LastError::set(DZ_EC_INTERNAL, "db not implemented");
    return NULL;
}
DZ_API DzResultSet* dz_db_query(DzDatabase* db,
                                const char* query,
                                const char* filter,
                                int32_t version) {
    (void)db;
    (void)query;
    (void)filter;
    (void)version;
    LastError::set(DZ_EC_INTERNAL, "db not implemented");
    return NULL;
}

/* ── 错误信息三函数 ── */

DZ_API int32_t dz_errcode(void) { return dztrader::LastError::code(); }
DZ_API const char* dz_errstr(int32_t errcode) { return dztrader::LastError::str(errcode); }
DZ_API const char* dz_errmsg(void) { return dztrader::LastError::msg(); }

/* ── 版本信息 ── */

DZ_API int32_t dz_version_major(void) { return DZ_VERSION_MAJOR; }
DZ_API int32_t dz_version_minor(void) { return DZ_VERSION_MINOR; }
DZ_API int32_t dz_version_patch(void) { return DZ_VERSION_PATCH; }
DZ_API const char* dz_version_string(void) { return DZ_VERSION_STRING; }
DZ_API int32_t dz_version_hex(void) { return DZ_VERSION_HEX; }
