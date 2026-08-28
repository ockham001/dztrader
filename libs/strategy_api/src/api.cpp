#include <dztrader/api.h>

#include <algorithm>
#include <unordered_set>
#include <string_view>
#include <climits>
#include <cfloat>
#include <chrono>

#include <dztrader/error.h>
#include <dztrader/data_type.h>
#include <dztrader/core/last_error.h>
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

DZ_API void dz_wait(DzContext* ctx) {
    static_assert(noexcept(ctx->sem.wait()));
    ctx->sem.wait();
}

DZ_API bool dz_wait_for(DzContext* ctx, uint32_t timeout_ms) {
    return ctx->sem.wait_for(timeout_ms);
}

DZ_API const void* dz_next_event(DzContext* ctx) {
    static_assert(noexcept(ctx->event_reader.next_frame()));
    return ctx->event_reader.next_frame();
}

DZ_API const void* dz_next_md(DzContext* ctx) {
    static_assert(noexcept(ctx->md_reader.next_frame()));
    return ctx->md_reader.next_frame();
}

DZ_API const char* dz_md_source_name(DzContext* ctx) {
    return ctx->md_source_name;
}

DZ_API void dz_notify_self(DzContext* ctx) { ctx->sem.notify(); }

DZ_API const char* dz_strategy_home(DzContext* ctx) { return ctx->strategy_home.c_str(); }

DZ_API const char* dz_strategy_id(DzContext* ctx) { return ctx->strategy_id; }

DZ_API bool dz_preload_event(DzContext* ctx, const void* preload) {
    try {
        // 事件通道 reader 半边 (对齐 md/td on_event_shm_timer)
        // preload 为不透明透传: 布局为 DzShmPreload{bytes,pages,reserved},
        // 调用方无需解析, 从平台 DZ_FRAME_PRELOAD_EVENT_SHM 帧 payload 原样转发。
        const auto* spec = static_cast<const DzShmPreload*>(preload);
        if (spec != nullptr && spec->pages > 0) {
            ctx->event_reader.prefetch_pages(spec->pages);
        }
        if (spec != nullptr && spec->bytes > 0) {
            ctx->event_reader.prefetch_for_bytes(spec->bytes);
        }
        ctx->event_reader.release_old_pages();

        // 事件通道 writer 半边
        if (spec != nullptr && spec->pages > 0) {
            ctx->event_writer.prefetch_pages(spec->pages);
        }
        if (spec != nullptr && spec->bytes > 0) {
            ctx->event_writer.prefetch_for_bytes(spec->bytes);
        }
        ctx->event_writer.close_old_pages();
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

DZ_API bool dz_preload_md(DzContext* ctx, const void* preload) {
    try {
        // 行情通道: 策略进程为纯 reader, 仅 reader 半边 (无 writer)
        // preload 为不透明透传: 布局为 DzShmPreload{bytes,pages,reserved},
        // 调用方无需解析, 从平台 DZ_FRAME_PRELOAD_MD_SHM 帧 payload 原样转发。
        const auto* spec = static_cast<const DzShmPreload*>(preload);
        if (spec != nullptr && spec->pages > 0) {
            ctx->md_reader.prefetch_pages(spec->pages);
        }
        if (spec != nullptr && spec->bytes > 0) {
            ctx->md_reader.prefetch_for_bytes(spec->bytes);
        }
        ctx->md_reader.release_old_pages();
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

DZ_API bool dz_on_md_started(DzContext* ctx, const void* frame) {
    try {
        if (frame == nullptr) {
            LastError::set(DZ_EC_INVALID_PARAM, "frame is null");
            return false;
        }
        const auto* bytes = static_cast<const std::byte*>(frame);
        shm::FrameView view(bytes);
        if (view.type() != DZ_FRAME_NOTIFY_MD_STARTED) {
            LastError::set(DZ_EC_INVALID_PARAM, "frame type is not NOTIFY_MD_STARTED");
            return false;
        }
        if (std::string_view(view.ext_inst_id()) != ctx->md_source_name) {
            return true;  // 非本策略源，静默忽略
        }
        if (ctx->md_desired_instruments.empty()) {
            return true;  // 无需补订阅
        }

        SubscribeReq req;
        req.instance_id = strategy_identity(ctx->strategy_id);
        req.action = SubscribeAction::Subscribe;
        req.replace = true;
        req.instruments.assign(ctx->md_desired_instruments.begin(),
                               ctx->md_desired_instruments.end());
        return write_subscribe_req(ctx, req);
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return false;
}

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
