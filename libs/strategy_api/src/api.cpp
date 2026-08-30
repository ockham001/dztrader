#include <dztrader/api.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <unordered_set>
#include <string_view>
#include <cfloat>
#include <chrono>
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

/// dz_next_event 单次调用连续消费内部帧的上限 (防内部帧洪峰饿死 dz_next_md)
constexpr uint32_t kMaxInternalFramesPerCall = 32;

/// TD 回报定向过滤: payload.strategy_id == 本策略裸名才放行。
/// 空 strategy_id (外部单/手工单, 非任何策略所下) 与非本策略回报一律拦截。
template <typename ReportT>
bool is_own_report(const DzContext* ctx, const std::byte* frame) noexcept {
    const shm::FrameView view(frame);
    if (view.frame_size() < sizeof(DzFrameHeader) + sizeof(ReportT)) {
        return false;  // 截断帧防御: 读不出 payload 的帧一律拦截
    }
    const auto& rpt = view.payload<ReportT>();
    return std::string_view(rpt.strategy_id) == std::string_view(ctx->strategy_id);
}

/// NOTIFY_MD_STARTED 自动补订阅 (定义见 write_subscribe_req 之后)
void on_md_started_internal(DzContext* ctx, const std::byte* frame);

/// 单帧分派: 用户帧返回 true (放行给策略用户), 其余返回 false (SDK 内部消费/丢弃)。
/// 逐帧类型 switch, 替代原 is_user_frame 白名单 + handle_internal_frame 双开关。
bool dispatch_frame(DzContext* ctx, const std::byte* frame, DzFrameType type) {
    switch (type) {
        case DZ_FRAME_TD_ORDER_RPT:
            return is_own_report<DzOrderReport>(ctx, frame);
        case DZ_FRAME_TD_TRADE_RPT:
            return is_own_report<DzTradeReport>(ctx, frame);
        case DZ_FRAME_STG_USER_INPUT:
            // 定向帧: 仅 instance_id == 裸策略名 的属于本策略
            return std::string_view(shm::FrameView(frame).ext_inst_id()) == ctx->strategy_id;
        case DZ_FRAME_REQUEST_SHUTDOWN:
            if (std::string_view(shm::FrameView(frame).ext_inst_id()) == ctx->strategy_id) {
                ctx->internal_cleanup_on_shutdown();  // 内部清理后再放行
                return true;
            }
            return false;
        case DZ_FRAME_PRELOAD_EVENT_SHM: {
            const auto& params = shm::FrameView(frame).payload<DzShmPreload>();
            ctx->internal_event_preload = params;  // 覆盖参数槽 (只保留最新)
            ctx->schedule_internal_preload(DzContext::INTERNAL_TOKEN_EVENT,
                                           dztrader::core::random_jitter(0, 5000));
            return false;
        }
        case DZ_FRAME_PRELOAD_MD_SHM: {
            const shm::FrameView view(frame);
            // 仅本策略绑定的行情源 (instance_id = 行情通道名)
            if (std::string_view(view.ext_inst_id()) != ctx->md_source_name) {
                return false;
            }
            if (view.ext_inst_payload_size() < sizeof(DzShmPreload)) {
                return false;
            }
            const auto& params = *reinterpret_cast<const DzShmPreload*>(view.ext_inst_payload());
            ctx->internal_md_preload = params;  // 覆盖参数槽 (只保留最新)
            ctx->schedule_internal_preload(DzContext::INTERNAL_TOKEN_MD,
                                           dztrader::core::random_jitter(0, 5000));
            return false;
        }
        case DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER:
            ctx->event_writer.refresh_subscribers();
            return false;
        case DZ_FRAME_NOTIFY_MD_STARTED:
            on_md_started_internal(ctx, frame);
            return false;
        // 其余 TD 回报帧 2002-2017 (持仓/资金/费率/网关状态/合约等): 暂不按策略过滤, 全量放行
        case DZ_FRAME_TD_POSITION_INFO:
        case DZ_FRAME_TD_TRADING_ACCOUNT:
        case DZ_FRAME_TD_GATEWAY_STATUS:
        case DZ_FRAME_TD_INSTRUMENT:
        case DZ_FRAME_TD_INSTRUMENT_STATUS:
        case DZ_FRAME_TD_ERROR_RPT:
        case DZ_FRAME_TD_RISK_REJECT:
        case DZ_FRAME_TD_TRANSFER_REQ:
        case DZ_FRAME_TD_TRANSFER_RSP:
        case DZ_FRAME_TD_TRANSFER_RTN:
        case DZ_FRAME_TD_PASSWORD_UPDATE_REQ:
        case DZ_FRAME_TD_PASSWORD_UPDATE_RSP:
        case DZ_FRAME_TD_SETTLEMENT_INFO:
        case DZ_FRAME_TD_MARGIN_RATE:
        case DZ_FRAME_TD_COMMISSION_RATE:
        case DZ_FRAME_TD_POSITION_DETAIL:
            return true;
        default:
            return false;  // 其余平台帧 (日志/SHM 配置/进程控制/TD 控制帧等) 丢弃
    }
}

}  // namespace

DZ_API void dz_wait(DzContext* ctx) {
    // 纯等待: 不做任何定时器计算与触发 (唯一推进点在 dz_next_event)。
    // 定时器是事件流的一部分, 契约要求策略调用 dz_next_event 消费;
    // 不调用则不触发, 不设防。
    if (ctx->has_pending_timers()) {
        const uint32_t timeout_ms = ctx->next_timer_wait_ms();
        if (timeout_ms == 0) {
            return;  // 已到期: 免一次 syscall, 立即返回, 由 dz_next_event 触发
        }
        static_assert(noexcept(ctx->sem.wait_for(timeout_ms)));
        (void)ctx->sem.wait_for(timeout_ms);
    } else {
        static_assert(noexcept(ctx->sem.wait()));
        ctx->sem.wait();
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
        try {
            if (dispatch_frame(ctx, frame, type)) {
                return frame;  // 用户帧最高优先
            }
        } catch (const std::exception& e) {
            dz_diag((std::string("frame dispatch failed: ") + e.what()).c_str());
        } catch (...) {
            dz_diag("frame dispatch failed: unknown exception");
        }
        if (++internal_count >= kMaxInternalFramesPerCall) {
            break;  // 连续 32 条内部帧: 让位, 防饿死 dz_next_md
        }
    }
    // 无用户帧可给 (通道空或 32 让位): 服务计时器
    try {
        ctx->tick_timers();
    } catch (const std::exception& e) {
        dz_diag((std::string("timer tick failed: ") + e.what()).c_str());
    } catch (...) {
        dz_diag("timer tick failed: unknown exception");
    }
    return ctx->pop_timer_frame();
}

DZ_API const void* dz_next_md(DzContext* ctx) {
    static_assert(noexcept(ctx->md_reader.next_frame()));
    return ctx->md_reader.next_frame();
}

DZ_API const char* dz_md_source_name(DzContext* ctx) { return ctx->md_source_name; }

DZ_API void dz_notify_self(DzContext* ctx) { ctx->sem.notify(); }

/* ── 定时器 ── */

namespace {

bool valid_delay_ms(int32_t delay_ms) { return delay_ms > 0; }

bool valid_time_of_day_ms(int32_t time_of_day_ms) {
    return time_of_day_ms >= 0 && time_of_day_ms <= 86'399'999;
}

DzTimerId schedule_relative(DzContext* ctx,
                            int32_t delay_ms,
                            DzContext::UserTimerEntry::Kind kind) {
    try {
        DzContext::UserTimerEntry entry;
        entry.kind = kind;
        if (kind == DzContext::UserTimerEntry::Kind::Every) {
            entry.interval = std::chrono::milliseconds{delay_ms};
        }
        entry.next_deadline = DzContext::TimerClock::now() + std::chrono::milliseconds{delay_ms};
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

DzTimerId schedule_time_of_day(DzContext* ctx,
                               int32_t time_of_day_ms,
                               DzContext::UserTimerEntry::Kind kind) {
    try {
        DzContext::UserTimerEntry entry;
        entry.kind = kind;
        entry.time_of_day_ms = time_of_day_ms;
        entry.next_deadline = DzContext::TimerClock::now() + next_time_of_day_delay(time_of_day_ms);
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
    // 本函数经 extern "C" 边界导出, 不允许异常逃逸; 体内所有操作均为 noexcept,
    // 故无需 try/catch。static_assert 强制约束, 防止后续引入会抛异常的操作时
    // 异常静默穿越 C ABI 边界 (UB)。
    static_assert(
        noexcept(ctx->event_writer.open_frame(DZ_FRAME_TD_ORDER_REQ, sizeof(DzOrderReq))));
    static_assert(noexcept(ctx->order_id.generate()));
    static_assert(noexcept(ctx->event_writer.close_frame()));
    static_assert(noexcept(ctx->event_writer.notify_subscribers()));

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
}

DZ_API bool dz_cancel_order(DzContext* ctx, const char* account_id, DzOrderId order_id) {
    // 同 dz_place_order: extern "C" 边界不允许异常逃逸; 体内操作均 noexcept, 免 try/catch,
    // static_assert 防止后续引入会抛异常的操作。
    static_assert(noexcept(
        ctx->event_writer.open_frame(DZ_FRAME_TD_ORDER_CANCEL_REQ, sizeof(DzOrderCancelReq))));
    static_assert(noexcept(ctx->event_writer.close_frame()));
    static_assert(noexcept(ctx->event_writer.notify_subscribers()));

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

DZ_API bool dz_unsubscribe(DzContext* ctx, const char* const instruments[], uint32_t count) {
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
        dz_diag((std::string("auto resubscribe on md started failed: ") + e.what()).c_str());
    } catch (const std::exception& e) {
        dz_diag((std::string("auto resubscribe on md started failed: ") + e.what()).c_str());
    } catch (...) {
        dz_diag("auto resubscribe on md started failed: unknown exception");
    }
}

}  // namespace

/* ── 逻辑持仓 ── */

DZ_API bool dz_set_logical_position(DzContext* ctx,
                                    const char* account_id,
                                    const char* instrument_id,
                                    int32_t net_volume) {
    // 同 dz_place_order: extern "C" 边界不允许异常逃逸; 体内操作均 noexcept, 免 try/catch,
    // static_assert 防止后续引入会抛异常的操作。
    static_assert(noexcept(
        ctx->event_writer.open_frame(DZ_FRAME_SET_LOGICAL_POSITION, sizeof(DzLogicalPosition))));
    static_assert(noexcept(ctx->event_writer.close_frame()));
    static_assert(noexcept(ctx->event_writer.notify_subscribers()));

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

    // 同 dz_place_order: extern "C" 边界不允许异常逃逸; 体内操作均 noexcept, 免 try/catch,
    // static_assert 防止后续引入会抛异常的操作。
    static_assert(noexcept(output_ui_max_payload(ctx->event_writer.page_size())));
    static_assert(noexcept(ctx->event_writer.write_ext_inst_frame(
        DZ_FRAME_STG_USER_OUTPUT, ctx->strategy_id, reinterpret_cast<const std::byte*>(data), 0u)));
    static_assert(noexcept(ctx->event_writer.notify_subscribers()));

    const auto len = strlen(data);
    // 页感知上限
    const auto cap = output_ui_max_payload(ctx->event_writer.page_size());
    if (len > 0 && cap == 0) {
        LastError::set(DZ_EC_BUFFER_TOO_SMALL, "page too small for output frame");
        return false;
    }
    const auto data_len = static_cast<uint32_t>(std::min<uint64_t>(len, cap));
    if (!ctx->event_writer.write_ext_inst_frame(DZ_FRAME_STG_USER_OUTPUT, ctx->strategy_id,
                                                reinterpret_cast<const std::byte*>(data),
                                                data_len)) {
        // write_ext_inst_frame 为 noexcept bool: 唯一失败路径是 open_frame 返回 nullptr,
        // 其每条失败分支均已设置 LastError (writer.cpp), 此处直接透传, 不自设错误码
        return false;
    }
    ctx->event_writer.notify_subscribers();
    return true;
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

/* ── SDK 诊断输出 ── */

DZ_API void dz_diag(const char* message) {
    if (message == nullptr) {
        return;
    }
    std::fputs("[dzsdk] ", stderr);
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
}

/* ── 版本信息 ── */

DZ_API int32_t dz_version_major(void) { return DZ_VERSION_MAJOR; }
DZ_API int32_t dz_version_minor(void) { return DZ_VERSION_MINOR; }
DZ_API int32_t dz_version_patch(void) { return DZ_VERSION_PATCH; }
DZ_API const char* dz_version_string(void) { return DZ_VERSION_STRING; }
DZ_API int32_t dz_version_hex(void) { return DZ_VERSION_HEX; }
