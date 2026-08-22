#include <dztrader/api.h>

#include <algorithm>
#include <unordered_set>
#include <string_view>
#include <climits>
#include <cfloat>
#include <chrono>
#include <optional>

#include <dztrader/error.h>
#include <dztrader/data_type.h>
#include <dztrader/core/last_error.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/version.h>

#include "md_source.h"
#include "strategy_context.h"
#include "result_set.h"
#include "vector_result_set.h"
#include "cursor_result_set.h"

using namespace dztrader;

namespace {
std::optional<StrategyContext> g_ctx{};

/// 运行上下文惰性构造: 事件通道读写器/信号量/订单ID元数据在首次使用时创建。
/// 惰性而非静态 eager-init: 平台未启动时构造失败可在 dz_init 中捕获报错,
/// 而不是让策略进程在进入 main 前以未处理异常崩溃。
StrategyContext& ctx() {
    if (!g_ctx.has_value()) {
        g_ctx.emplace();
    }
    return *g_ctx;
}

}  // namespace

/* ── 生命周期 ── */

DZ_API bool dz_init(void) {
    try {
        (void)ctx();
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    }
    return false;
}

DZ_API void dz_release(void) { g_ctx.reset(); }

DZ_API DzMdSource* dz_create_md_source(const char* name) {
    try {
        if (name == nullptr || *name == '\0') {
            LastError::set(DZ_EC_INVALID_PARAM, "name is null or empty");
            return nullptr;
        }
        if (ctx().md_sources.contains(name)) {
            LastError::set(DZ_EC_INVALID_PARAM, "md source already created");
            return nullptr;
        }
        const std::string source_name(name);
        const std::string identity = strategy_identity(ctx().strategy_id);

        // 1. 主动向 master 注册为该行情通道读者 (帧 1013):
        //    契约 shm: 请求进程收到成功 RTN 前不得打开通道, 故先发注册帧, 待 RTN 确认后才打开。
        nlohmann::json payload = {{"subscriber", identity}};
        (void)shm::write_ext_inst_json(ctx().writer, DZ_FRAME_REQUEST_MD_READER_REGISTER,
                                       source_name, payload);
        ctx().writer.notify_subscribers();

        // 2. 阻塞等待匹配的 RTN (帧 1015): master 回 RTN 后 notify 订阅者信号量,
        //    故 sem.wait_for 可被唤醒; 唤醒后排空 event reader 逐帧检查。
        //    等待期间消费到的非目标帧直接丢弃——策略创建 md source 时尚未进入事件处理,
        //    初始化早期少量事件帧丢失是可接受的工程权衡; 读到匹配 RTN 立即处理返回。
        bool matched = false;
        bool ok = false;
        std::string fail_msg;
        constexpr uint32_t kRegisterTimeoutMs = 5000;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kRegisterTimeoutMs);
        while (!matched) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                break;  // 超时
            }
            const auto remaining_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (!ctx().sem.wait_for(static_cast<uint32_t>(remaining_ms))) {
                break;  // 超时
            }
            // 排空已到达的帧直到无新帧 (next_frame 非阻塞, 无新帧返回 nullptr)
            while (const auto* frame = ctx().reader.next_frame()) {
                shm::FrameView view(frame);
                if (view.type() != DZ_FRAME_RTN_MD_READER_REGISTER ||
                    std::string_view(view.ext_inst_id()) != identity) {
                    continue;  // 非本请求的 RTN / 其他事件帧, 直接丢弃
                }
                const auto rtn = shm::decode_ext_inst_json<nlohmann::json>(view);
                if (!rtn.is_object() || !rtn.contains("channel") || !rtn["channel"].is_string() ||
                    rtn["channel"].get<std::string>() != source_name) {
                    continue;  // 通道不匹配 (非本次注册的 RTN), 丢弃继续等
                }
                ok = rtn.value("ok", false);
                if (!ok) {
                    fail_msg = rtn.contains("message") && rtn["message"].is_string()
                                   ? rtn["message"].get<std::string>()
                                   : "md reader register failed";
                }
                matched = true;
                break;
            }
        }

        // 3. 结果处理: 成功 RTN 前不得打开通道; 失败不放行。
        if (!matched) {
            // 超时: master 未在期限内回 RTN (通道/进程异常或 master 不可达)
            LastError::set(DZ_EC_TIMEOUT, "md reader register timeout");
            return nullptr;
        }
        if (!ok) {
            // master 拒绝注册 (通道未配置/未就绪/行情进程未运行等), 携带原因不放行
            LastError::set(DZ_EC_INVALID_PARAM, fail_msg);
            return nullptr;
        }
        // 此刻才打开行情通道 reader (DzMdSource 构造函数立即 Reader::create)
        auto* source = new DzMdSource{source_name, identity};  // NOLINT
        ctx().md_sources.insert(source_name);
        return source;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    }
    return nullptr;
}

DZ_API void dz_destroy_md_source(DzMdSource* source) {
    if (source != nullptr) {
        // 主动向 master 注销读者 (帧 1014, best-effort):
        // master 在策略进程退出时也会兜底清理 (on_child_exit)。
        try {
            nlohmann::json payload = {{"subscriber", strategy_identity(ctx().strategy_id)}};
            (void)shm::write_ext_inst_json(ctx().writer, DZ_FRAME_REQUEST_MD_READER_UNREGISTER,
                                           source->name, payload);
            ctx().writer.notify_subscribers();
        } catch (const Exception& e) {
            LastError::set(e.code(), e.what());
        } catch (const std::exception& e) {
            LastError::set(DZ_EC_INTERNAL, e.what());
        }
        ctx().md_sources.erase(source->name);
        delete source;  // NOLINT
    }
}

DZ_API void dz_wait() {
    static_assert(noexcept(g_ctx->sem.wait()));
    ctx().sem.wait();
}

DZ_API bool dz_wait_for(uint32_t timeout_ms) {
    return ctx().sem.wait_for(timeout_ms);
}

DZ_API const void* dz_next_event(void) {
    static_assert(noexcept(g_ctx->reader.next_frame()));
    return ctx().reader.next_frame();
}

DZ_API const void* dz_next_md(DzMdSource* source) {
    static_assert(noexcept(source->reader.next_frame()));
    return source->reader.next_frame();
}

DZ_API void dz_notify_self(void) { ctx().sem.notify(); }

DZ_API const char* dz_strategy_home(void) { return ctx().strategy_home.c_str(); }

DZ_API const char* dz_strategy_id(void) { return ctx().strategy_id; }

DZ_API bool dz_preload() { return true; }

/* ── 交易接口 ── */

DZ_API DzOrderId dz_place_order(const char* account_id,
                                const char* instrument_id,
                                DzDirection direction,
                                DzPriceType price_type,
                                double price,
                                DzVolume volume,
                                DzPositionEffect position_effect) {
    auto* req = reinterpret_cast<DzOrderReq*>(
        ctx().writer.open_frame(DZ_FRAME_TD_ORDER_REQ, sizeof(DzOrderReq)));
    if (req == nullptr) {
        return -1;
    }
    const auto order_id = ctx().order_id.generate();
    copy_string(req->strategy_id, ctx().strategy_id, true);
    copy_string(req->account_id, account_id, true);
    copy_string(req->instrument_id, instrument_id, true);
    req->remark[0] = '\0';
    req->direction = direction;
    req->price_type = price_type;
    req->price = price;
    req->volume = volume;
    req->position_effect = position_effect;
    req->order_id = order_id;
    ctx().writer.close_frame();
    ctx().writer.notify_subscribers();
    return order_id;
}

DZ_API bool dz_cancel_order(const char* account_id, DzOrderId order_id) {
    auto* req = reinterpret_cast<DzOrderCancelReq*>(
        ctx().writer.open_frame(DZ_FRAME_TD_ORDER_CANCEL_REQ, sizeof(DzOrderCancelReq)));
    if (req == nullptr) {
        return false;
    }
    req->order_id = order_id;
    copy_string(req->account_id, account_id, true);
    ctx().writer.close_frame();
    ctx().writer.notify_subscribers();
    return true;
}

namespace {

bool write_subscribe_req(DzMdSource* source, SubscribeReq& req) {
    try {
        write_ext_inst_json(ctx().writer, DZ_FRAME_REQUEST_MD_SUBSCRIBE, source->name, req);
        ctx().writer.notify_subscribers();
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
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

DZ_API bool dz_subscribe(DzMdSource* source,
                         const char* const instruments[],
                         uint32_t count,
                         bool replace_previous) {
    if (source == nullptr) {
        LastError::set(DZ_EC_INVALID_PARAM, "source is nullptr");
        return false;
    }
    if (instruments == nullptr || count == 0) {
        LastError::set(DZ_EC_INVALID_PARAM, "instruments is null or count is 0");
        return false;
    }

    SubscribeReq req;
    req.instance_id = strategy_identity(ctx().strategy_id);
    req.action = SubscribeAction::Subscribe;
    req.replace = replace_previous;
    fill_instruments(req, instruments, count);

    if (req.instruments.empty()) {
        LastError::set(DZ_EC_INVALID_PARAM, "no valid instruments after dedup");
        return false;
    }

    return write_subscribe_req(source, req);
}

DZ_API bool dz_unsubscribe(DzMdSource* source, const char* const instruments[], uint32_t count) {
    if (source == nullptr) {
        LastError::set(DZ_EC_INVALID_PARAM, "source is nullptr");
        return false;
    }

    SubscribeReq req;
    req.instance_id = strategy_identity(ctx().strategy_id);

    if (instruments == nullptr || count == 0) {
        req.action = SubscribeAction::UnsubscribeAll;
        return write_subscribe_req(source, req);
    }

    req.action = SubscribeAction::Unsubscribe;
    fill_instruments(req, instruments, count);

    if (req.instruments.empty()) {
        LastError::set(DZ_EC_INVALID_PARAM, "no valid instruments after dedup");
        return false;
    }

    return write_subscribe_req(source, req);
}

/* ── 逻辑持仓 ── */

DZ_API bool dz_set_logical_position(const char* account_id,
                                    const char* instrument_id,
                                    int32_t net_volume) {
    auto* pos = reinterpret_cast<DzLogicalPosition*>(
        ctx().writer.open_frame(DZ_FRAME_SET_LOGICAL_POSITION, sizeof(DzLogicalPosition)));
    if (pos == nullptr) {
        return false;
    }
    copy_string(pos->account_id, account_id, true);
    copy_string(pos->instrument_id, instrument_id, true);
    copy_string(pos->strategy_id, ctx().strategy_id, true);
    pos->net_volume = net_volume;
    ctx().writer.close_frame();
    ctx().writer.notify_subscribers();
    return true;
}



/* ── UI 通知 ── */

DZ_API bool dz_notify_ui(DzNotifyLevel level, const char* message, bool popup) {
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
            {"source", ctx().strategy_id},
            {"level", level},
            {"message", message},
            {"timestamp", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())},
            {"popup", popup},
        };
        if (!shm::write_ext_json(ctx().writer, DZ_FRAME_NOTIFY_UI, payload)) {
            LastError::set(DZ_EC_INTERNAL, "frame write failed");
            return false;
        }
        ctx().writer.notify_subscribers();
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    }
    return false;
}

DZ_API bool dz_output_ui(const char* data) {
    if (!data) {
        LastError::set(DZ_EC_INVALID_PARAM, "data is null");
        return false;
    }

    const auto data_len = std::min(strlen(data), size_t{1024 * 1024});

    try {
        ctx().writer.write_ext_inst_frame(DZ_FRAME_STG_USER_OUTPUT, ctx().strategy_id,
                                     reinterpret_cast<const std::byte*>(data),
                                     static_cast<uint32_t>(data_len));
        ctx().writer.notify_subscribers();
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
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

DZ_API void dz_resultset_close(DzResultSet* rs) { delete rs; }

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
