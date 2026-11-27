#include "td/td_account_session.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/encoding.h>
#include <dztrader/core/string_util.h>
#include <dztrader/data_type.h>
#include <dztrader/date_time/date.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/risk_reject.h>
#include <dztrader/platform/td_account_ops.h>
#include <dztrader/struct.h>
#include <dztrader/tdstore/records.h>

#include "td/td_persist_records.h"
#include "td/td_position.h"

namespace dztrader::ctp {

namespace {

/// epoch 毫秒 (合约记录 updated_at 推进用).
int64_t epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 拒单路径 (on_rsp_order_insert / on_err_rtn_order_insert) 的输入枚举映射.
/// 未知的开平/价格类型/方向一律落成对应的 UNKNOWN 并登记到 `unknown`: 这两条路径构造的是
/// 本地拒单回报, 把不认识的值兜成 Open/Limit/Sell 会让下游看到一张「正常」的委托——
/// 方向猜错还会让净敞口符号反向 (issue #367 方案 §二). 由调用方随后统一上报.
void map_input_order_enums(const CThostFtdcInputOrderField& in,
                           OrderRecord& rec,
                           UnknownEnumSink& unknown) {
    switch (in.CombOffsetFlag[0]) {
        case THOST_FTDC_OF_Open:
            rec.base.position_effect = DZ_POSITION_EFFECT_OPEN;
            break;
        case THOST_FTDC_OF_Close:
            rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE;
            break;
        case THOST_FTDC_OF_CloseToday:
            rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE_TODAY;
            break;
        case THOST_FTDC_OF_CloseYesterday:
            rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE_YESTDAY;
            break;
        default:
            unknown.add(kUnknownCategoryPositionEffect, in.CombOffsetFlag[0]);
            rec.base.position_effect = DZ_POSITION_EFFECT_UNKNOWN;
            break;
    }
    switch (in.OrderPriceType) {
        case THOST_FTDC_OPT_AnyPrice:
            rec.base.price_type = DZ_PRICE_MARKET;
            break;
        case THOST_FTDC_OPT_LimitPrice:
            rec.base.price_type = DZ_PRICE_LIMIT;
            break;
        default:
            unknown.add(kUnknownCategoryPriceType, in.OrderPriceType);
            rec.base.price_type = DZ_PRICE_UNKNOWN;
            break;
    }
    switch (in.Direction) {
        case THOST_FTDC_D_Buy:
            rec.base.direction = DZ_DIRECTION_LONG;
            break;
        case THOST_FTDC_D_Sell:
            rec.base.direction = DZ_DIRECTION_SHORT;
            break;
        default:
            unknown.add(kUnknownCategoryDirection, in.Direction);
            rec.base.direction = DZ_DIRECTION_UNKNOWN;
            break;
    }
}

}  // namespace

AccountSession::AccountSession(std::string account_id,
                               shm::OrderIdMeta& order_id_meta,
                               shm::MultiWriter& event_writer,
                               PersistWriter& persist_writer,
                               const MpmcQueuePtr& event_queue,
                               dztrader::core::TimerQueue& timer_queue,
                               std::function<void(DzAccountState)> account_state_cb,
                               AccountSeqAllocator* seq_allocator,
                               AccountNotifyFn notify)
    : account_id_(std::move(account_id)),
      order_id_meta_(order_id_meta),
      event_writer_(event_writer),
      persist_writer_(persist_writer),
      event_queue_(event_queue),
      timer_queue_(timer_queue),
      account_state_cb_(std::move(account_state_cb)),
      notify_(std::move(notify)),
      risk_gate_(false),
      seq_allocator_(seq_allocator),
      query_lane_(query_pacer_) {
    if (!event_queue_) {
        throw std::runtime_error("account session init failed | reason=event_queue_null");
    }
    // 重放过滤器基准在登录日装载时建立 (ADR 0045 决策 5); 登录前保持空基准.
    report_filter_ = std::make_unique<ReportFilter>(account_id_);
}

AccountSession::~AccountSession() {
    try {
        disconnect();
    } catch (...) {
        // 析构不抛异常
    }
    // 兜底: disconnect 早退 (api_ 已空) 时也须让未跟踪定时器回调即刻失效.
    alive_token_.reset();
}

void AccountSession::set_query_min_interval(std::chrono::milliseconds interval) {
    query_pacer_.set_min_gap(interval);
}

bool AccountSession::defer_query_if_pacer_busy(const std::function<void()>& retry,
                                               std::string_view query_name) {
    // ADR 0055 决策 1: 所有 ReqQry* 共用「在途 1 笔 + 最小发送间隔」。pacer 拒绝立即发送时
    // 用绝对时间定时器延后到最早合法时刻（TimerQueue，无轮询 / 无 sleep）。
    const auto now = query_now();
    if (query_pacer_.can_issue(now)) {
        return false;
    }
    auto eligible = query_pacer_.next_eligible(now);
    if (eligible == QueryPacer::TimePoint::max()) {
        // 登录链自身串行、理论上不会在在途时再次进入发送点；防御性按最小间隔重排，
        // 重排次数受响应/超时收口约束（不会无限自旋）。
        SPDLOG_WARN("td query send deferred while in flight | account={} query={}", account_id_,
                    query_name);
        eligible = now + query_pacer_.min_gap();
    }
    auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(eligible - now);
    if (delay <= std::chrono::milliseconds(0)) {
        delay = std::chrono::milliseconds(1);
    }
    const uint64_t gen = generation_;
    std::weak_ptr<void> weak = alive_token_;
    // 带 tag 的可替换定时器：同一查询重复进入发送点时替换而不是叠加（范围 4）。
    timer_queue_.schedule_after_replace(
        "td_qry_defer_" + std::string(query_name), delay,
        [this, weak, gen, retry, name = std::string(query_name)]() {
            if (weak.expired()) return;
            if (gen != generation_) return;
            SPDLOG_DEBUG("td query send deferred | account={} query={}", account_id_, name);
            retry();
        });
    SPDLOG_DEBUG("td query waiting for flow control gap | account={} query={} delay_ms={}",
                 account_id_, query_name, delay.count());
    return true;
}

void AccountSession::note_query_sent() { query_pacer_.note_sent(query_now()); }

void AccountSession::note_query_done() { query_pacer_.note_done(); }

// === 运行期资金刷新（ADR 0055 / #458） ===
void AccountSession::set_query_send_hook(QuerySendFn hook) { query_send_hook_ = std::move(hook); }

void AccountSession::set_query_clock(QueryClockFn clock) { query_clock_ = std::move(clock); }

void AccountSession::set_settlement_send_hook(SettlementSendFn hook) {
    settlement_send_hook_ = std::move(hook);
}

void AccountSession::request_settlement_confirm_for_test() { req_settlement_confirm(); }

QueryPacer::TimePoint AccountSession::query_now() const {
    return query_clock_ ? query_clock_() : QueryPacer::Clock::now();
}

void AccountSession::advance_login_chain_for_test(TdState target) {
    // 测试夹具专用：从 Idle 逐条重放生产登录链的状态迁移，停在 target。
    // 纯状态机调用（不触网 / 不落库 / 不推帧）；迁移失败当场抛 —— 防止夹具与生产链漂移，
    // 也防止用例静默测到错误状态。
    const auto stop_at = [this, target](TdState reached) {
        if (state_machine_.state() != reached) {
            throw std::logic_error("advance_login_chain_for_test failed | reason=state_mismatch");
        }
        return reached == target;
    };
    state_machine_ = TdStateMachine{};
    state_machine_.on_connect();
    // #495: 停在 Connecting —— 前置连接超时用例的摆位点 (超时只在 Connecting 有效)。
    if (stop_at(TdState::Connecting)) return;
    state_machine_.on_front_connected();
    if (stop_at(TdState::Connected)) return;
    state_machine_.on_req_authenticate();
    if (stop_at(TdState::Authenticating)) return;
    state_machine_.on_authenticate_success();
    // #495: 认证通过但尚未发起登录 —— 登录失败用例的摆位点 (要验「不回退到 Authenticated」,
    // 必须先真的走过 Authenticated)。
    if (stop_at(TdState::Authenticated)) return;
    state_machine_.on_req_login();
    state_machine_.on_login_success(
        "test", trading_day_ > 0 ? format_day_text(trading_day_) : std::string(), "09:00:00");
    if (stop_at(TdState::LoggedIn)) return;
    state_machine_.on_req_settlement_confirm();
    if (stop_at(TdState::Confirming)) return;
    state_machine_.on_settlement_confirmed();
    if (stop_at(TdState::LoadingInstruments)) return;
    state_machine_.on_instruments_loaded();
    if (!stop_at(TdState::Ready)) {
        throw std::logic_error("advance_login_chain_for_test failed | reason=unsupported_target");
    }
}

void AccountSession::enter_ready_for_test(bool position_ok) {
    advance_login_chain_for_test(TdState::Ready);
    position_query_ok_ = position_ok;
    position_baseline_ready_ = position_ok;
}

void AccountSession::reset_query_channel() {
    cancel_runtime_query_timeout();
    if (query_lane_timer_id_ != 0) {
        timer_queue_.cancel(query_lane_timer_id_);
        query_lane_timer_id_ = 0;
        query_lane_timer_deadline_ = QueryPacer::TimePoint{};
    }
    query_pacer_.reset();
    query_lane_.reset();
    instrument_query_request_id_ = 0;
    account_query_request_id_ = 0;
    runtime_account_request_id_ = 0;
}

bool AccountSession::request_fund_refresh() { return mark_fund_refresh_dirty(); }

bool AccountSession::mark_fund_refresh_dirty() {
    // 只有账户 Ready 后的回报才触发刷新：登录缓冲 / 重放 / 未就绪阶段的事件不置脏
    // （重放本身处于 LoadingInstruments，本守卫已覆盖）。
    if (state_machine_.state() != TdState::Ready) {
        return false;
    }
    query_lane_.mark_dirty(QueryKind::TradingAccount);
    schedule_query_lane_pump();
    return true;
}

void AccountSession::schedule_query_lane_pump() {
    if (state_machine_.state() != TdState::Ready || query_lane_.in_flight()) {
        return;  // 在途时由响应收口驱动；离开 Ready 不排定时器
    }
    const auto now = query_now();
    const auto wake = query_lane_.next_wake(now);
    if (!wake.has_value()) {
        if (query_lane_timer_id_ != 0) {
            timer_queue_.cancel(query_lane_timer_id_);
            query_lane_timer_id_ = 0;
            query_lane_timer_deadline_ = QueryPacer::TimePoint{};
        }
        return;
    }
    // 绝对时间唤醒（tag 去重）；已有定时器且目标时刻不晚于所需时刻时保持不动：
    // 回报热路径上每笔事件都重排定时器是纯开销（同窗口 wake 不变）。
    if (query_lane_timer_id_ != 0 && *wake >= query_lane_timer_deadline_) {
        return;
    }
    const auto delay = *wake > now ? *wake - now : QueryPacer::Duration{0};
    query_lane_timer_deadline_ = *wake;
    const uint64_t gen = generation_;
    std::weak_ptr<void> weak = alive_token_;
    query_lane_timer_id_ = timer_queue_.schedule_after_replace(
        "td_qry_lane_" + account_id_, delay, [this, weak, gen]() {
            if (weak.expired() || gen != generation_) return;
            pump_query_lane();
        });
}

void AccountSession::pump_query_lane() {
    query_lane_timer_id_ = 0;
    query_lane_timer_deadline_ = QueryPacer::TimePoint{};
    if (state_machine_.state() != TdState::Ready || query_lane_.in_flight()) {
        return;
    }
    const auto now = query_now();
    const auto kind = query_lane_.take_ready(now);
    if (!kind.has_value()) {
        schedule_query_lane_pump();  // 退避/间隔未到：按下次合法时刻唤醒
        return;
    }
    send_runtime_trading_account(*kind, now);
}

void AccountSession::send_runtime_trading_account(QueryKind kind, QueryPacer::TimePoint now) {
    CThostFtdcQryTradingAccountField qry{};
    const int request_id = ++request_id_;
    int ret = 0;
    if (query_send_hook_) {
        ret = query_send_hook_(qry, request_id);
    } else if (api_ != nullptr) {
        ret = api_->ReqQryTradingAccount(&qry, request_id);
    } else {
        ret = -1;
    }
    if (ret != 0) {
        SPDLOG_WARN("td runtime fund query send failed | account={} request_id={} ret={}",
                    account_id_, request_id, ret);
        query_lane_.note_send_failure(now, kind);
        handle_runtime_query_failure(kind);
        return;
    }
    runtime_account_request_id_ = request_id;
    query_lane_.note_sent(now, kind);
    cancel_runtime_query_timeout();
    // 超时兜底（10s）：响应丢失时结束在途并进入退避，不悬挂到下一个事件。
    constexpr auto kTimeout = std::chrono::seconds(10);
    const uint64_t gen = generation_;
    std::weak_ptr<void> weak = alive_token_;
    runtime_account_timeout_timer_id_ =
        timer_queue_.schedule_after(kTimeout, [this, weak, gen, request_id, kind]() {
            if (weak.expired() || gen != generation_) return;
            on_runtime_query_timeout(request_id, kind);
        });
}

void AccountSession::expire_runtime_query_timeout_for_test() {
    // 生产路径由 TimerQueue 在 10s 后回调；测试直接注入同一收口逻辑（不 sleep）。
    if (runtime_account_request_id_ == 0) return;
    on_runtime_query_timeout(runtime_account_request_id_, QueryKind::TradingAccount);
}

void AccountSession::expire_settlement_confirm_timeout_for_test() {
    // 生产路径由 TimerQueue 在 settlement_confirm_timeout_ 后回调；测试注入同一收口逻辑。
    on_settlement_confirm_timeout();
}

void AccountSession::set_settlement_confirm_timeout_for_test(std::chrono::seconds timeout) {
    settlement_confirm_timeout_ = timeout;
}

// === 合约查询发送钩子 (#488) ===
void AccountSession::set_instrument_send_hook(InstrumentSendFn hook) {
    instrument_send_hook_ = std::move(hook);
}

void AccountSession::request_instrument_query_for_test() { req_qry_instrument(); }

void AccountSession::set_instruments_load_timeout_for_test(std::chrono::seconds timeout) {
    instruments_load_timeout_ = timeout;
}

void AccountSession::on_runtime_query_timeout(int request_id, QueryKind kind) {
    if (runtime_account_request_id_ != request_id) return;  // 迟到超时（已收口/已复位）
    SPDLOG_WARN("td runtime fund query timeout | account={} request_id={}", account_id_,
                request_id);
    cancel_runtime_query_timeout();
    runtime_account_request_id_ = 0;
    query_lane_.note_failure(query_now(), kind);
    handle_runtime_query_failure(kind);
}

void AccountSession::handle_runtime_query_failure(QueryKind kind) {
    if (query_lane_.exhausted(kind)) {
        query_lane_.clear_exhausted(kind);
        SPDLOG_WARN("td runtime fund refresh retries exhausted | account={}", account_id_);
        // 先排定时器再通知：通知回调异常不得让通道停在「有退避但无唤醒」的半状态。
        schedule_query_lane_pump();
        notify_runtime_fund_failure();
        return;
    }
    schedule_query_lane_pump();
}

void AccountSession::notify_runtime_fund_failure() {
    const int64_t now_s =
        std::chrono::duration_cast<std::chrono::seconds>(query_now().time_since_epoch()).count();
    constexpr int64_t kNotifyWindowS = 300;
    if (runtime_failure_notify_last_s_ >= 0 &&
        now_s - runtime_failure_notify_last_s_ < kNotifyWindowS) {
        SPDLOG_DEBUG("td runtime fund refresh notify rate limited | account={}", account_id_);
        return;
    }
    runtime_failure_notify_last_s_ = now_s;
    if (!notify_) return;
    try {
        notify_(DZ_NOTIFY_ERROR,
                "资金刷新失败：柜台查询连续失败，本轮刷新暂停；下一笔委托/成交会再次触发，"
                "持续失败请检查柜台连接。",
                /*popup=*/false);
    } catch (const std::exception& e) {
        // 通知是辅助通道：失败只记日志，不得让主循环 / 定时器回调抛出。
        SPDLOG_ERROR("td runtime fund failure notify failed | account={} error=\"{}\"", account_id_,
                     e.what());
    } catch (...) {
        SPDLOG_ERROR("td runtime fund failure notify failed | account={} error=\"unknown\"",
                     account_id_);
    }
}

void AccountSession::cancel_runtime_query_timeout() {
    if (runtime_account_timeout_timer_id_ != 0) {
        timer_queue_.cancel(runtime_account_timeout_timer_id_);
        runtime_account_timeout_timer_id_ = 0;
    }
}

bool AccountSession::load_trading_day_baseline() {
    // 契约 td-data-sync §3 / ADR 0045 决策 5: 每次登录成功 (含盘中重连) 后、
    // 处理首条重放回报前, 从交易库装载**当前交易日**的订单最新态与成交去重键.
    // 历史只在库中 (不做增量水位装载, 删除全史预扫).
    if (!read_db_provider_) {
        SPDLOG_ERROR("td login day load failed | account={} error=db_provider_missing",
                     account_id_);
        return false;
    }
    SQLite::Database* db = read_db_provider_();
    if (db == nullptr) {
        SPDLOG_ERROR("td login day load failed | account={} error=db_unavailable", account_id_);
        return false;
    }
    if (trading_day_ <= 0) {
        // #404 后不可达: 唯一调用点 (on_rsp_user_login) 已由 trading_day_usable 保证 > 0.
        // 保留作防御 (本函数是被独立测试覆盖的装载入口, 不与该守卫耦合).
        SPDLOG_WARN("td login day load degraded, trading day unknown | account={}", account_id_);
    }
    const std::string day_text = format_day_text(trading_day_);
    DayLoadData day;
    try {
        day = load_trading_day(*db, account_id_, day_text);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td login day load failed | account={} error=\"{}\"", account_id_, e.what());
        return false;
    }
    // seq 基准 = max(DB 四表 MAX(seq), 无会话拒单已分配值): 两者都只增不减,
    // 防新 seq 与 DB 旧行冲突, 也防同账户无会话拒单期间已发出的 seq 复用.
    const uint64_t allocator_seq =
        (seq_allocator_ != nullptr) ? seq_allocator_->current(account_id_) : 0;
    seq_counter_ = std::max(day.start_seq, allocator_seq);
    // 重放过滤器基准 = 当日订单最新态 + 当日成交去重键 (历史行不装载).
    auto filter = ReportFilter::load(day.orders, day.trades);
    *report_filter_ = std::move(filter);
    // 每单成交累计基准 = 当日成交按 order_id 聚合 (契约 §3 重启后重建均价).
    order_avgs_ = day.order_avgs;
    SPDLOG_INFO("td login day loaded | account={} day={} orders={} trades={} seq={}", account_id_,
                day_text, day.orders.size(), day.trades.size(), seq_counter_);
    return true;
}

void AccountSession::refresh_order_avg(int64_t order_id) {
    // 契约 td-data-sync §3: 每笔成交到达且均价变化时, 刷新该订单记录 (新 seq, 与订单行同
    // seq) 并推 ORDER_REPORT, 不依赖柜台回调顺序. 基准缺失 (外部单/委托回报尚未到达) →
    // 只保留累计, 等委托回报带走均价; 均价未变化 → 不推不落不分配 seq.
    if (!report_filter_) {
        return;
    }
    const OrderRecord* base = report_filter_->find_order(order_id);
    if (base == nullptr) {
        return;
    }
    const std::optional<double> avg = order_avgs_.avg_price(order_id);
    // 判定用容差比较 (同价多笔的全量重算有 1 ULP 漂移); 推帧/落库仍写 *avg 全精度值.
    if (!avg.has_value() || avg_price_unchanged(*avg, base->base.avg_price)) {
        return;
    }
    // 刷新行只覆盖均价派生列, 对比字段保持基准值 → 重放仍 kSkip (去重基准不被均价破坏).
    OrderRecord updated = order_record_with_avg(*base, *avg);
    updated.base.seq = ++seq_counter_;
    report_filter_->accept_order(updated);
    write_order_rpt(updated.base);
    persist_order(updated);
    SPDLOG_DEBUG("td order avg refreshed | account={} order_id={} avg_price={} seq={}", account_id_,
                 order_id, *avg, updated.base.seq);
}

// ============================================================================
// 生命周期
// ============================================================================

void AccountSession::open(const std::string& flow_dir,
                          const std::vector<std::string>& front_addrs,
                          const std::string& broker_id,
                          const std::string& password,
                          const std::string& auth_code,
                          const std::string& app_id) {
    if (api_ != nullptr) {
        SPDLOG_WARN("td session already opened | account={}", account_id_);
        return;
    }
    broker_id_ = broker_id;
    password_ = password;
    auth_code_ = auth_code;
    app_id_ = app_id;
    front_addrs_ = front_addrs;
    flow_dir_ = flow_dir;

    api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(flow_dir.c_str());
    if (api_ == nullptr) {
        throw std::runtime_error("create trader api failed | reason=null");
    }

    // 保存 CTP API 版本, 用于 on_rsp_user_login 填充 sys_version (I1: 替代 CZCETime)
    api_version_ = CThostFtdcTraderApi::GetApiVersion();
    state_machine_.set_api_version(api_version_);

    spi_ = std::make_unique<TdSpi>(account_id_, event_queue_);
    api_->RegisterSpi(spi_.get());
    // RESTART: 重连后从断点续传, 用于回报补登 (设计 §5.3)
    api_->SubscribePrivateTopic(THOST_TERT_RESTART);
    api_->SubscribePublicTopic(THOST_TERT_RESTART);
    // 注册所有前置地址（CTP 支持多前置自动故障切换）
    for (const auto& addr : front_addrs) {
        std::string front_addr = addr;
        if (!front_addr.starts_with("tcp://") && !front_addr.starts_with("ssl://") &&
            !front_addr.starts_with("socks")) {
            front_addr = "tcp://" + front_addr;
        }
        api_->RegisterFront(const_cast<char*>(front_addr.c_str()));  // NOLINT
    }

    state_machine_.on_connect();
    SPDLOG_INFO("td connecting | account={} fronts={}", account_id_, front_addrs.size());

    // 30s 连接超时, 防止卡在 Connecting 状态
    uint64_t gen = generation_;
    connect_timer_id_ = timer_queue_.schedule_after(std::chrono::seconds(30), [this, gen]() {
        if (gen != generation_) return;  // 陈旧回调
        on_connect_timeout();
    });

    api_->Init();
}

void AccountSession::disconnect() {
    if (api_ == nullptr) return;
    // 未跟踪的查询链定时器回调经弱引用检查即刻失效 (TdApi 随后 sessions_.erase,
    // 队列中按原始 this 排定的回调若不检查会 UAF).
    alive_token_.reset();
    cancel_connect_timer();
    cancel_login_timer();
    cancel_settlement_confirm_timer();
    cancel_instruments_load_timer();
    cancel_position_query_retry_timer();
    cancel_position_query_timeout_timer();
    ++generation_;  // 使已挂起定时器回调失效
    api_->RegisterSpi(nullptr);
    api_->Release();
    api_ = nullptr;
    spi_.reset();
    state_machine_.on_disconnect();
    // 断线持仓基准失效: 重连后重新查询重建; 在途查询响应变陈旧 (代际已失效).
    position_baseline_ready_ = false;
    position_query_in_flight_ = false;
    // ADR 0055: 查询通道复位（在途/上次发送清空，重登后立即允许查询）。
    reset_query_channel();
    // I1: 清空缓冲, 防止重连后重放陈旧回报
    buffered_orders_.clear();
    buffered_trades_.clear();
    SPDLOG_INFO("td disconnected | account={}", account_id_);
}

// ============================================================================
// SPI 事件处理
// ============================================================================

void AccountSession::on_front_connected() {
    cancel_connect_timer();
    // 登录日装载在 on_rsp_user_login 成功之后执行 (契约 td-data-sync §3:
    // 装载先于首条重放回报); 前置连接阶段不装载.
    auto notif = state_machine_.on_front_connected();
    SPDLOG_INFO("td front connected | account={} state={}", account_id_,
                magic_enum::enum_name(state_machine_.state()));
    if (!auth_code_.empty()) {
        req_authenticate();
    } else {
        req_login();
    }
}

void AccountSession::on_connect_timeout() {
    // 生产由 TimerQueue 在 30s 后回调; 测试经 expire_connect_timeout_for_test 注入同一收口.
    // 已离开 Connecting (前置已连上 / 已登出) 时忽略迟到超时。
    if (state_machine_.state() != TdState::Connecting) return;
    SPDLOG_ERROR("td connect timeout | account={}", account_id_);
    // #495 / ADR 0059 决策 7: 连接超时**不进失败态** (仍回 Idle), 但必须写
    // last_error.code = connect_timeout 并推通知 —— 此前这条路径既不写 last_error
    // 也丢弃了状态机返回的通知, 用户事后在卡片上看不到「上次失败：连接超时」。
    const auto notif = state_machine_.on_connect_timeout();
    push_notification(notif, connect_timeout_message(std::string(kConnectTimeoutReason)),
                      "td connect timeout notify failed");
    // 契约 account-status: 连接超时回 Offline (本路径是唯一不经 TdApi dispatch 的直调点,
    // 经状态通知回调走统一写出口)
    if (account_state_cb_) {
        account_state_cb_(DZ_ACCOUNT_OFFLINE);
    }
}

void AccountSession::on_front_disconnected(int reason) {
    state_machine_.on_front_disconnected(reason);
    cancel_connect_timer();
    cancel_login_timer();
    cancel_settlement_confirm_timer();
    cancel_instruments_load_timer();
    cancel_position_query_retry_timer();
    cancel_position_query_timeout_timer();
    ++generation_;  // 使已挂起定时器回调失效
    // 终检发现 1: 作废在途登录收尾查询链 (CTP 断连后在途查询响应作废)。
    // 否则首次登录已收尾 (finalizer 停在 kDone), 重连重登的持仓 is_last 被
    // phase()==kQueryPosition 门挡住不推进不发资金查询, 5min 兜底同门被挡
    // → 账户永久停在 LoadingInstruments。
    finalizer_ = LoginFinalizer{};  // 复位收尾状态机 (kQueryPosition)
    position_query_group_.clear();
    position_rebuild_consumed_ = false;
    position_query_ok_ = false;
    position_query_retries_ = 0;
    position_baseline_ready_ = false;
    position_query_in_flight_ = false;
    // ADR 0055: 查询通道复位（在途/上次发送清空，重登后立即允许查询）。
    reset_query_channel();
    // 清空持仓 map, 重连后主动查询重建 (设计 §6)
    holdings_.clear();
    // 清空合约 -> 交易所映射, 重连后 req_qry_instrument 重新填充 (C2)
    instrument_exchange_map_.clear();
    // I1: 清空缓冲回报, 防止重连后重放陈旧回报导致状态错乱
    buffered_orders_.clear();
    buffered_trades_.clear();
    // order_ref_ 故意保留: 重连后 sync_order_ref 重新同步 (设计 §9.4)
    // order_ref_map_ 故意保留: RESTART 重传去重 (设计 §9.4)
    SPDLOG_WARN("td front disconnected | account={} reason={}", account_id_, reason);
}

void AccountSession::on_rsp_authenticate(const OnRspAuthenticateField& f) {
    // 迟到响应窗口 (与 #488 的 on_rsp_settlement_confirm 同一形态、同一理由):
    // 10 秒认证超时已把状态转到 AuthFailed 之后, 真响应才到。此时若继续按成功分支走:
    //   on_authenticate_success() 会被状态机拒绝 (不在 Authenticating) → 状态仍是 AuthFailed,
    //   但 req_login() 照发 —— 一个没有归属的登录请求。柜台若回成功, on_login_success 的
    //   守卫同样拒绝 (不在 LoggingIn), 响应被丢弃; 账户就卡在 AuthFailed 且没人知道为什么。
    // 判据 = 状态 (此刻必须还在 Authenticating), 与 settlement 一致。
    // 守卫必须排在 cancel 之前: 排在前面会让一条被丢弃的外来响应把本链的超时定时器撤掉。
    if (state_machine_.state() != TdState::Authenticating) {
        SPDLOG_WARN("td stale authenticate response ignored | account={} state={}", account_id_,
                    magic_enum::enum_name(state_machine_.state()));
        return;
    }
    cancel_login_timer();
    bool ok = f.rsp_info && f.rsp_info->ErrorID == 0;
    if (!ok) {
        const int error_id = f.rsp_info ? f.rsp_info->ErrorID : -1;
        SPDLOG_ERROR("td authenticate failed | account={} error_id={}", account_id_, error_id);
        // #489 并入 #494: 此前丢弃状态机返回的通知, 认证失败对用户完全无痕。现在经统一
        // 出口 (#493) 上浮, 并把段内原因写进 last_error (契约 td-status §2.5)。
        // 文案不复用状态机的通用「认证失败」: 用户需要知道柜台给了什么。
        notify_auth_failed(TdFailureCode::AuthRejected,
                           f.rsp_info ? dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg)
                                      : std::string("柜台未返回错误信息"),
                           error_id);
        return;
    }
    state_machine_.on_authenticate_success();
    SPDLOG_INFO("td authenticated | account={}", account_id_);
    req_login();
}

void AccountSession::on_rsp_user_login(const OnRspTdUserLoginField& f) {
    // 迟到响应窗口 (与 #488 的 on_settlement_confirm / #494 的 on_rsp_authenticate 同一形态):
    // 10 秒登录超时已把状态转到 LoginFailed 之后, 真响应才到。此时若按成功分支继续走,
    // 会 set_trading_day → load_trading_day_baseline (重建重放过滤器 / 清空当日均价累计) →
    // on_login_success (被状态机拒绝) → req_settlement_confirm (也被拒绝) ——
    // 账户状态没变, 但**登录日基准已被一次无归属的响应重写**, 后续重放去重与均价累计
    // 建立在错误的基线上。下游守卫只是碰巧挡住最后一步, 不能靠它替上游擦屁股。
    // 判据 = 状态 (此刻必须还在 LoggingIn), 与 authenticate / settlement 一致。
    // 守卫必须排在 cancel 之前: 排在前面会让一条被丢弃的外来响应把本链的超时定时器撤掉。
    if (state_machine_.state() != TdState::LoggingIn) {
        SPDLOG_WARN("td stale login response ignored | account={} state={}", account_id_,
                    magic_enum::enum_name(state_machine_.state()));
        return;
    }
    cancel_login_timer();
    if (!f.rsp_info || f.rsp_info->ErrorID != 0) {
        const int error_id = f.rsp_info ? f.rsp_info->ErrorID : -1;
        SPDLOG_ERROR("td login failed | account={} error_id={}", account_id_, error_id);
        // 柜台拒绝: 原因码 login_rejected (带 CTP error_id), 原因文案取 ErrorMsg 原文
        // (错误码走 last_error.error_id, 不在正文里重复; 前端把它作为「上次失败」tooltip)。
        notify_login_failed(TdFailureCode::LoginRejected,
                            f.rsp_info ? dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg)
                                       : std::string("柜台未返回错误信息"),
                            error_id);
        return;
    }
    // 同步 order_ref (设计 §9.4): new = max(local+1, ctp_max+1)
    if (f.rsp_user_login) {
        int64_t ctp_max = parse_max_order_ref(f.rsp_user_login->MaxOrderRef);
        order_ref_ = sync_order_ref(order_ref_, ctp_max);
    }
    // 同步 trading_day. 解析失败按登录失败处理, 不进 LoggedIn (对齐 md 的
    // LoginParseError 分支): 会话交易日是本会话全部业务数据的时间锚, 它缺失时若继续登录,
    // 订单/成交/持仓/资金会盖上一个无效日期, 而 ACCOUNT_STATUS 帧的 date 会回落 0 ——
    // 日切感知 (status 帧 date 变化) 随之失真; 跨日会话本身已由回报 TradingDay 校验判死
    // (#423), 但那次判死同样依赖正确的会话交易日. 宁可这次登录失败,
    // 也不带着错误的会话交易日进入 Ready.
    // 注意: 失败后本进程不会自动重发登录 (状态停在 LoginFailed 直到人工登出重来或前置
    // 断线), 该缺口见 #406.
    // 发现 3 (评审 Important): 走 set_trading_day 而非直接赋值 — 日切/重连后新交易日的
    // 持仓为绝对态, 旧镜像 (key 不含日期) 会拦截新日首报, 必须在登录时清空 (spec §4.1 跨日清空).
    if (!trading_day_usable(f.days_since_epoch)) {
        const std::string raw = f.rsp_user_login ? f.rsp_user_login->TradingDay : "";
        SPDLOG_ERROR("td login trading day invalid | account={} trading_day=\"{}\"", account_id_,
                     raw);
        // 交易日无效 = 响应解析失败 (对齐 md 的 LoginParseError 分支), 不是柜台拒绝。
        notify_login_failed(TdFailureCode::LoginParseError,
                            std::format("交易日无效（原始值 \"{}\"）", raw));
        return;
    }
    set_trading_day(f.days_since_epoch);
    // 契约 td-data-sync §3 / ADR 0045 决策 5: 登录成功且交易日已更新后、处理首条
    // 重放回报前装载当日订单/成交基准. 装载失败按登录失败路径处理 (重试登录):
    // 空基准会让重放风暴整批误判转发, 宁可重试也不带错基准进入 Ready.
    if (!load_trading_day_baseline()) {
        notify_login_failed(TdFailureCode::DayLoadFailed, "当日订单/成交基准装载失败");
        return;
    }
    // I1: sys_version 改用 CTP API 版本 (CZCETime 是郑商所时间, 不是系统版本)
    const std::string& sys_version = api_version_;
    std::string trading_day_str = f.rsp_user_login ? f.rsp_user_login->TradingDay : "";
    std::string login_time = f.rsp_user_login ? f.rsp_user_login->LoginTime : "";
    state_machine_.on_login_success(sys_version, trading_day_str, login_time);
    SPDLOG_INFO("td login success | account={} order_ref={} trading_day={}", account_id_,
                order_ref_, trading_day_str);
    req_settlement_confirm();
}

void AccountSession::on_rsp_settlement_confirm(const OnRspSettlementInfoConfirmField& f) {
    // #488 迟到响应窗口: 看门狗超时已把状态转到 SettlementConfirmFailed (并通知了用户),
    // 此后真响应才到 —— 不能再驱动后续链条, 否则会发起一次没有收尾归属的合约查询, 把账户
    // 拖成一个既非「已通知失败」也非「正常初始化」的中间态。
    // 判据 = 状态 + request_id 双校验 (同 on_rsp_qry_instrument 的 query_gen_/request_id):
    // 只判状态不够 —— 断线重连后新链进入 Confirming 时, 旧链的迟到响应会以「状态正确」
    // 蒙混过关。
    //
    // 取消看门狗必须排在守卫**之后**: 排在前面会让一条被丢弃的外来响应把本链的定时器
    // 撤掉, 会话留在 Confirming 却再无回调 —— 正是本票要消灭的静默卡死形态。
    // 合法响应在这里之后才收口, 那时 cancel 才有意义 (cancel 对已失效 id 是 no-op)。
    if (state_machine_.state() != TdState::Confirming ||
        f.request_id != settlement_confirm_request_id_) {
        SPDLOG_WARN(
            "td stale settlement confirm response ignored | account={} state={} request_id={}",
            account_id_, magic_enum::enum_name(state_machine_.state()), f.request_id);
        return;
    }
    cancel_settlement_confirm_timer();
    // 本链收口: 在途 request id 作废 (成功与失败两条分支都走这里)。
    settlement_confirm_request_id_ = 0;
    // I8: 统一 rsp_info 处理: 缺失视为失败 (与 on_rsp_authenticate / on_rsp_user_login 一致)
    if (!f.rsp_info || f.rsp_info->ErrorID != 0) {
        std::string err =
            f.rsp_info ? dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg) : "rsp_info is null";
        SPDLOG_ERROR("td settlement confirm failed | account={} error_id={} error=\"{}\"",
                     account_id_, f.rsp_info ? f.rsp_info->ErrorID : -1, err);
        // #496: 停在 SettlementConfirmFailed (7/10「结算单确认失败」), 不再回退 LoggedIn。
        // 缺 rsp_info 也归 settlement_rejected —— 与 login_rejected 的缺信息口径一致。
        notify_settlement_confirm_failed(
            TdFailureCode::SettlementRejected,
            f.rsp_info ? std::format("柜台拒绝，错误码 {}：{}", f.rsp_info->ErrorID, err)
                       : std::string("柜台未返回错误信息"),
            f.rsp_info ? f.rsp_info->ErrorID : -1);
        return;
    }
    state_machine_.on_settlement_confirmed();
    SPDLOG_INFO("td settlement confirmed | account={}", account_id_);
    // C2: 启动合约查询 (设计 §7.2), 进入 LoadingInstruments 状态.
    // 查询完成 (on_rsp_qry_instrument is_last) 或失败时调 on_instruments_loaded 转 Ready.
    req_qry_instrument();
}

void AccountSession::on_rsp_qry_instrument(const OnRspQryInstrumentField& f) {
    // 终检发现 1: 陈旧响应防护 — 断连重连后, 旧会话事件队列中的迟到合约查询响应
    // (响应发起时快照的 query_gen_ ≠ 当前 generation_) 一律丢弃, 防旧 is_last
    // 二次发起查询链 / 旧行污染映射表。
    if (query_gen_ != generation_) {
        return;
    }
    if (f.request_id != instrument_query_request_id_) {
        return;  // 迟到 / 重复 / 非本链响应
    }
    // 先处理数据, 再判 is_last (避免 null instrument + is_last 时状态机卡死)
    if (f.instrument) {
        // C2: 存储 instrument_id -> exchange_id/price_tick, 供 place_order 查表与 Task 7 用
        instrument_exchange_map_[f.instrument->InstrumentID] = InstrumentBrief{
            f.instrument->ExchangeID, f.instrument->PriceTick,
            static_cast<double>(f.instrument->VolumeMultiple),
            static_cast<DzProductClass>(product_class_from_ctp(f.instrument->ProductClass))};
        try {
            // 持久化合约信息 (供审计/复盘/策略查询)
            // update_day[9]: "YYYYMMDD" 文本 (从 DzDate 距纪元天数转换)
            std::string update_day_str = "00000000";
            if (trading_day_ > 0) {
                dztrader::Date d{trading_day_};
                char update_day[9];
                auto* end = std::format_to_n(update_day, sizeof(update_day) - 1,
                                             "{:04d}{:02d}{:02d}", d.year(), d.month(), d.day())
                                .out;
                *end = '\0';
                update_day_str = update_day;
            }
            auto rec = to_instrument_record(*f.instrument, update_day_str);
            rec.updated_at = epoch_ms();
            persist_writer_.enqueue(PersistTask{PersistTask::Kind::Instrument, std::move(rec)});
        } catch (const std::exception& e) {
            SPDLOG_ERROR("td instrument persist failed | account={} instrument={} error=\"{}\"",
                         account_id_, f.instrument->InstrumentID, e.what());
        }
        SPDLOG_DEBUG("td instrument | account={} instrument={}", account_id_,
                     f.instrument->InstrumentID);
    } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
        // CTP 查询失败 (流控超时/权限错误等): pInstrument=null, pRspInfo={ErrorID!=0}
        SPDLOG_ERROR("td qry instrument error | account={} error_id={} error_msg=\"{}\"",
                     account_id_, f.rsp_info->ErrorID,
                     dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
    }
    if (f.is_last) {
        note_query_done();  // 本查询收口（无论成功/失败）
        instrument_query_request_id_ = 0;
        // Ready 后无合约查询发起方 (定向刷新已退役, ADR 0014); 重复/迟到 is_last 一律忽略.
        if (state_machine_.state() != TdState::LoadingInstruments) {
            SPDLOG_INFO(
                "td qry instrument is_last ignored (not loading) | account={} instrument={}",
                account_id_, f.instrument ? f.instrument->InstrumentID : "");
            return;
        }
        // Task 6 (spec §4.2 登录完成协议): 合约加载完成**不立即**转 Ready.
        // 留在 LoadingInstruments (缓冲分支继续生效, CTP 私有流重放期间回报继续进缓冲),
        // 进入登录收尾查询阶段: 发起持仓查询 (CTP 流控 1 次/秒, 串行 -> 资金).
        bool failed = (f.rsp_info && f.rsp_info->ErrorID != 0) || !f.instrument;
        if (failed) {
            // 合约加载失败: 停在 InstrumentsLoadFailed (8/10「合约加载失败」), 不进 Ready (#496).
            // #488: 原因改中文 —— 它会进用户弹窗 (notify_instruments_load_failed)。
            const std::string err =
                f.rsp_info ? std::format("柜台拒绝，错误码 {}：{}", f.rsp_info->ErrorID,
                                         dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg))
                           : std::string("最后一批响应未携带合约数据");
            // is_last 带错误是 instruments_rejected 的三种来源之一 (契约 td-status §2.5);
            // 缺 rsp_info 而只有「无合约数据」也算柜台侧拒绝 (与缺 rsp_info 的通用口径一致)。
            notify_instruments_load_failed(TdFailureCode::InstrumentsRejected, err,
                                           f.rsp_info ? f.rsp_info->ErrorID : -1);
            return;
        }
        // 合约加载成功: 停留在 LoadingInstruments, 发起持仓查询.
        cancel_instruments_load_timer();
        SPDLOG_INFO("td instruments loaded, start login finalize | account={} count={}",
                    account_id_, instrument_exchange_map_.size());
        // 日切/重连后的持仓为绝对态: 解除基准就绪标志, 新登录链查询重建基准.
        position_baseline_ready_ = false;
        // 终检发现 1 (双保险 b): 二次进入收尾前防御性复位收尾状态机 + ok 标志 —
        // 首次登录已 kDone 时 (断连未清残留的任何路径), 重登查询链从 kQueryPosition
        // 重走, 持仓/资金 is_last 的 phase 门才成立。
        finalizer_ = LoginFinalizer{};
        position_query_ok_ = false;
        position_rebuild_consumed_ = false;
        position_query_group_.clear();
        req_qry_investor_position();
    }
}

void AccountSession::on_rtn_order(const OnRtnOrderField& f) {
    // ADR 0049 决策 5 / 契约 td-data-sync §3 §7: 回报必须自证属于当前会话交易日.
    // 判定必须在 LoadingInstruments 缓冲之前 —— 否则登录重放阶段的跨日回报会被缓冲而漏检.
    // 判死后该条回报直接丢弃 (不分配 seq / 不推帧 / 不落库): 其 date 取自会话交易日, 落库即
    // 污染次日登录也修不回的委托/成交追加流 (整组替换只覆盖持仓/资金).
    const auto day_verdict = judge_report_trading_day(trading_day_, f.order.TradingDay);
    if (day_verdict != ReportTradingDayVerdict::Match) {
        if (request_cross_day_logout(day_verdict, f.order.TradingDay)) {
            SPDLOG_ERROR(
                "td rtn order trading day rejected | account={} reason={} session_day={} "
                "report_day=\"{}\" order_ref={}",
                account_id_, report_trading_day_reason(day_verdict), trading_day_,
                f.order.TradingDay, f.order.OrderRef);
        }
        return;
    }

    // LoadingInstruments 期间缓冲, Ready 后重放 (设计 §5.3).
    // Task 6: 缓冲重放阶段 (replaying_) 状态仍是 LoadingInstruments, 须放行,
    // 否则重放回报会被重新入缓冲 (无限循环).
    if (state_machine_.state() == TdState::LoadingInstruments && !replaying_) {
        buffer_order_rpt(f);
        return;
    }

    try {
        // CTP OrderField -> OrderRecord (含 DzOrderReport base + CTP 扩展字段)
        // 未知枚举落成对应 UNKNOWN 并把原始码登记, 随后统一上报 (不改写成本地拒单).
        UnknownEnumSink unknown;
        OrderRecord rpt = to_order_record(f.order, account_id_, trading_day_, &unknown);
        report_unknown_enums(unknown);

        // ① 现有 order_ref_map_ 识别: 本地单/外部单, order_id 赋值, is_external 判定 (设计 §9.3).
        //    必须先于过滤器 check (过滤器按 order_id 定位基准; kSkip 命中时 replay_hit_order
        //    用 DB 行回填 order_ref->order_id/strategy_id, 此时 rec 已带正确 order_id 才有效).
        // OrderRef 在映射表中 = 本地发出, 否则外部订单
        const DzOrderId* local = order_ref_map_.find_by_order_ref(f.order.OrderRef);
        if (local != nullptr) {
            rpt.base.order_id = *local;
            rpt.is_external = 0;
            // 本地单: 回填 strategy_id (策略 SDK 按 strategy_id 定向过滤回报)
            if (const std::string* sid = order_ref_map_.find_strategy(*local)) {
                copy_string(rpt.base.strategy_id, sid->c_str(), true);
            }
            // C4: 收到 CTP 回报后更新 CancelContext 的 front_id/session_id
            // (place_order 时初始为 0, 这里填充实际值供后续 cancel_order 使用)
            order_ref_map_.update_cancel_context(*local, f.order.FrontID, f.order.SessionID);
        } else {
            rpt.base.order_id = order_id_meta_.generate();
            rpt.is_external = 1;
            order_ref_map_.insert_by_order_ref(f.order.OrderRef, rpt.base.order_id);
        }
        // OrderSysID 非空时建立反向映射 (供外部订单识别)
        if (f.order.OrderSysID[0] != '\0') {
            order_ref_map_.insert_by_sys_id(f.order.OrderSysID, rpt.base.order_id);
        }
        // 撤单数量推导: CTP OrderField 无 VolumeCanceled 字段, 按状态推导
        // M1: 用 std::max 防止 VolumeTraded > Volume 异常数据导致负值
        if (rpt.base.status == DZ_ORDER_CANCELLED) {
            rpt.volume_canceled = std::max(0, rpt.base.volume - rpt.base.volume_traded);
        }

        // ② 重放过滤器: 按 order_id 定位基准, 比对字段集 (spec §4.1).
        //    基准未装载时 (兜底默认构造) 直接放行, 避免误吞.
        bool outdated = false;
        if (report_filter_ &&
            report_filter_->check_order(rpt, &outdated) == ReportFilter::Verdict::kSkip) {
            // 对比命中: 回填 order_ref_map_ (重启后本地单误判修复, spec §4.1).
            // rec 已带正确 order_id, 用 DB 行含的 order_ref->order_id/strategy_id 信息回填.
            report_filter_->replay_hit_order(rpt);
            // 修复: 该 order_ref 已由识别块判为外部单时, 用基准行 order_ref 覆盖映射,
            // 使后续回报 (含成交) 正确关联本地 order_id.
            if (rpt.is_external && rpt.order_ref[0] != '\0') {
                order_ref_map_.insert_by_order_ref(rpt.order_ref, rpt.base.order_id);
                if (rpt.base.strategy_id[0] != '\0') {
                    order_ref_map_.insert_strategy(rpt.base.order_id, rpt.base.strategy_id);
                }
            }
            return;  // spec: 吞同不推不落不分配 seq
        }
        if (outdated) {
            SPDLOG_WARN("td order outdated dropped | account={} order_id={}", account_id_,
                        rpt.base.order_id);
            return;
        }

        // ADR 0055 决策 2：过滤器之后才置脏——被吞的重复/重放回报不代表新的柜台状态。
        mark_fund_refresh_dirty();

        // 实时流观测 (ADR 0049 外部单假设): 记录最后一条**实时**回报的柜台时间, 供后续登录
        // 重放判定「某笔外部单是否只在重放里出现」. 取 UpdateTime (本次状态变化时刻),
        // 缺失时回落 InsertTime; 重放回报不更新 (否则判据基准会被重放数据污染).
        if (!replaying_) {
            note_live_report_time(rpt.update_time > 0 ? rpt.update_time : rpt.insert_time);
        }
        // 外部单「只在登录重放出现」告警: 放在过滤器之后 —— 已命中基准的记录是此前见过并
        // 落库的, 不构成「私有流非投资者级」的证据.
        if (rpt.is_external) {
            check_replay_external_order(rpt);
        }

        // ③ 转发: 分配 seq -> 更新基准 -> 推帧 -> 落库 (同一事件 shm 帧与 DB 行同 seq).
        // 均价不来自柜台 (CTP 无该字段): 有当日成交累计 → 用累计值; 否则保留基准行均价
        // (跨日 GFD 单不得被清零); 两者都没有 → 0 (无成交语义). 顺序无关: 成交先到时
        // 累计已建, 委托回报在此带走均价.
        if (const auto avg = order_avgs_.avg_price(rpt.base.order_id); avg.has_value()) {
            rpt.base.avg_price = *avg;
        } else if (const OrderRecord* base = report_filter_->find_order(rpt.base.order_id);
                   base != nullptr) {
            rpt.base.avg_price = base->base.avg_price;
        }
        rpt.base.seq = ++seq_counter_;
        report_filter_->accept_order(rpt);
        write_order_rpt(rpt.base);
        persist_order(rpt);

        // 活动平仓挂单 → 冻结重算 (kReplay 阶段同样应用; 基准未就绪时跳过)
        if (position_baseline_ready_) {
            if (is_spread_instrument(rpt.base.instrument_id)) {
                // 套利 / 组合合约不参与持仓统计 (ADR 0049 决策 6): 组合合约的持仓行本身不入账,
                // 冻结推导也只对单腿合约有意义. 委托回报照常推帧/落库 (上面已完成).
                SPDLOG_DEBUG(
                    "td spread order not applied to position | account={} instrument={} "
                    "order_id={}",
                    account_id_, rpt.base.instrument_id, rpt.base.order_id);
            } else if (auto* h = ensure_holding(rpt.base.instrument_id,
                                                std::string(rpt.base.exchange_id));
                       h != nullptr) {
                ActiveOrderUpdate upd{std::string(rpt.base.instrument_id),
                                      std::string(rpt.order_ref),
                                      rpt.base.direction,
                                      rpt.base.position_effect,
                                      rpt.base.status,
                                      rpt.base.volume,
                                      rpt.base.volume_traded};
                auto ch = h->apply_order(upd);
                if (ch.long_changed) push_position(*h, DZ_DIRECTION_LONG);
                if (ch.short_changed) push_position(*h, DZ_DIRECTION_SHORT);
            }
        }

        SPDLOG_DEBUG(
            "td rtn order | account={} order_id={} order_ref={} status={} traded={} seq={}",
            account_id_, rpt.base.order_id, f.order.OrderRef,
            magic_enum::enum_name(rpt.base.status), rpt.base.volume_traded, rpt.base.seq);
    } catch (const std::exception& e) {
        // "宁肯乱码也不能崩溃": 不传播异常到 TdApi 主循环
        SPDLOG_ERROR(
            "td rtn order process failed | account={} error=\"{}\" instrument={} order_ref={}",
            account_id_, e.what(), f.order.InstrumentID, f.order.OrderRef);
    }
}

void AccountSession::on_rtn_trade(const OnRtnTradeField& f) {
    // ADR 0049 决策 5 / 契约 td-data-sync §3 §7: 同 on_rtn_order —— 判定在缓冲之前,
    // 判死后该条成交直接丢弃 (不分配 seq / 不推帧 / 不落库).
    const auto day_verdict = judge_report_trading_day(trading_day_, f.trade.TradingDay);
    if (day_verdict != ReportTradingDayVerdict::Match) {
        if (request_cross_day_logout(day_verdict, f.trade.TradingDay)) {
            SPDLOG_ERROR(
                "td rtn trade trading day rejected | account={} reason={} session_day={} "
                "report_day=\"{}\" instrument={} trade_id={}",
                account_id_, report_trading_day_reason(day_verdict), trading_day_,
                f.trade.TradingDay, f.trade.InstrumentID, f.trade.TradeID);
        }
        return;
    }

    // Task 6: 重放阶段放行 (同 on_rtn_order, 防止重放回报被重新入缓冲).
    if (state_machine_.state() == TdState::LoadingInstruments && !replaying_) {
        buffer_trade_rpt(f);
        return;
    }

    try {
        // CTP TradeField -> TradeRecord (含 DzTradeReport base + 扩展字段)
        UnknownEnumSink unknown;
        TradeRecord rpt = to_trade_record(f.trade, account_id_, trading_day_, &unknown);
        report_unknown_enums(unknown);

        // 通过 OrderRef 反查 DzOrderId, 填到 base.order_id
        const DzOrderId* local = order_ref_map_.find_by_order_ref(f.trade.OrderRef);
        if (local != nullptr) {
            rpt.base.order_id = *local;
            // 本地单: 回填 strategy_id (策略 SDK 按 strategy_id 定向过滤回报)
            if (const std::string* sid = order_ref_map_.find_strategy(*local)) {
                copy_string(rpt.base.strategy_id, sid->c_str(), true);
            }
        }

        // 重放过滤器: 成交键 (trading_day, trade_id) 存在性去重 (spec §4.1).
        // 命中 = CTP 重放重复, 吞 (不推不落不分配 seq). 基准未装载时直接放行.
        if (report_filter_ && report_filter_->check_trade(rpt) == ReportFilter::Verdict::kSkip) {
            return;
        }

        // ADR 0055 决策 2：过滤器之后才置脏（重放重复成交不代表新的柜台余额）。
        mark_fund_refresh_dirty();

        // 实时流观测 (同 on_rtn_order): 成交的柜台时间 = 交易日 × 86400 + TradeTime 秒.
        // 只记实时回报, 登录重放不更新 —— 它是外部单「只在重放出现」告警的判据基准.
        if (!replaying_ && rpt.base.date > 0 && rpt.base.time >= 0) {
            note_live_report_time(static_cast<int64_t>(rpt.base.date) * 86400 + rpt.base.time);
        }

        // 转发: 分配 seq -> 更新基准 -> 推帧 -> 落库 (与委托同取号器, 全类型共享).
        rpt.base.seq = ++seq_counter_;
        report_filter_->accept_trade(rpt);
        write_trade_rpt(rpt.base);
        persist_trade(rpt);

        // 每单成交累计 + 均价变化则刷新委托行 (契约 §3: 不依赖柜台回调顺序).
        // 放在成交自身推帧/落库之后: 成交帧先于其触发的委托刷新帧, seq 亦如此.
        order_avgs_.add_trade(rpt.base.order_id, rpt.base.price, rpt.base.volume);
        refresh_order_avg(rpt.base.order_id);

        // 盘中持仓增量 (基准就绪且非缓冲重放; 重放成交已含在查询快照中)
        // 套利 / 组合合约成交照常落库/推帧 (上面已完成), 但不参与持仓统计 (ADR 0049 决策 6):
        // 组合持仓行本身不入账, 单腿合约是普通期货合约, 由各自的成交正常增量维护.
        // 未知开平标志不参与持仓增量: 当成平仓会平掉对方真实持仓, 当开仓会凭空放大敞口
        // (apply_trade 内部也有同一道防御, 这里判定是为了留下可定位的 ERROR 证据).
        if (is_spread_instrument(rpt.base.instrument_id)) {
            SPDLOG_DEBUG(
                "td spread trade excluded from position | account={} instrument={} trade_id={} "
                "volume={}",
                account_id_, f.trade.InstrumentID, f.trade.TradeID, f.trade.Volume);
        } else if (rpt.base.position_effect == DZ_POSITION_EFFECT_UNKNOWN) {
            SPDLOG_ERROR(
                "td rtn trade skipped for unknown position effect | account={} instrument={} "
                "trade_id={} volume={}",
                account_id_, f.trade.InstrumentID, f.trade.TradeID, f.trade.Volume);
        } else if (position_baseline_ready_ && !replaying_) {
            if (auto* h = ensure_holding(rpt.base.instrument_id, std::string(rpt.base.exchange_id));
                h != nullptr) {
                auto ch = h->apply_trade(rpt.base);
                if (ch.long_changed) push_position(*h, DZ_DIRECTION_LONG);
                if (ch.short_changed) push_position(*h, DZ_DIRECTION_SHORT);
            }
        }

        SPDLOG_INFO("td rtn trade | account={} instrument={} trade_id={} volume={} price={} seq={}",
                    account_id_, f.trade.InstrumentID, f.trade.TradeID, f.trade.Volume,
                    f.trade.Price, rpt.base.seq);
    } catch (const std::exception& e) {
        SPDLOG_ERROR(
            "td rtn trade process failed | account={} error=\"{}\" instrument={} trade_id={}",
            account_id_, e.what(), f.trade.InstrumentID, f.trade.TradeID);
    }
}

// ============================================================================
// 业务接口
// ============================================================================

void AccountSession::place_order(const DzOrderReq& req) {
    if (req.position_effect == DZ_POSITION_EFFECT_AUTO) {
        SPDLOG_WARN(
            "td place_order rejected: auto offset unimplemented | account={} order_id={} "
            "instrument={}",
            account_id_, req.order_id, req.instrument_id);
        reject_order(req, "AUTO 开平未实现");
        return;
    }
    // 出账前枚举校验 (issue #367 §三): 不认识的枚举绝不能发往交易所. 兜底映射的
    // 后果是「止损单变限价单 / 平仓单变开仓单」, 比拒单严重得多.
    // 顺序刻意在就绪检查之前: 这些是「永远发不出去」的永久缺陷, 比「当前没连上」
    // 这种暂态更有诊断价值, 也避免账号离线时把坏单只记成「未就绪」放过去。
    if (req.price_type != DZ_PRICE_LIMIT && req.price_type != DZ_PRICE_MARKET &&
        req.price_type != DZ_PRICE_FAK && req.price_type != DZ_PRICE_FOK) {
        SPDLOG_WARN(
            "td place_order rejected: unsupported price type | account={} order_id={} "
            "price_type={}",
            account_id_, req.order_id, static_cast<int>(req.price_type));
        reject_order(req, std::format("不支持的价格类型: {}", static_cast<int>(req.price_type)));
        return;
    }
    if (req.position_effect != DZ_POSITION_EFFECT_OPEN &&
        req.position_effect != DZ_POSITION_EFFECT_CLOSE &&
        req.position_effect != DZ_POSITION_EFFECT_CLOSE_TODAY &&
        req.position_effect != DZ_POSITION_EFFECT_CLOSE_YESTDAY) {
        SPDLOG_WARN(
            "td place_order rejected: unsupported position effect | account={} "
            "order_id={} position_effect={}",
            account_id_, req.order_id, static_cast<int>(req.position_effect));
        reject_order(req,
                     std::format("不支持的开平标志: {}", static_cast<int>(req.position_effect)));
        return;
    }
    // `DzOrderReq.direction` 只允许 LONG/SHORT: 报单方向必须明确买或卖, 没有第三种可能.
    // 注意 NET(0) 不是合法的报单方向 —— 它是**内部持仓侧标签**(见 td_position.h 的
    // ActiveOrderUpdate.direction, 默认值 DZ_DIRECTION_NET, 表示「未标注/净」), 由
    // td_offset_converter 以内部入参形式消费, 不走本接口. UNKNOWN(-128) 表示柜台数据坏了.
    // 两者都必须在本地拒单, 绝不发往 CTP.
    if (req.direction != DZ_DIRECTION_LONG && req.direction != DZ_DIRECTION_SHORT) {
        SPDLOG_WARN(
            "td place_order rejected: unsupported direction | account={} order_id={} "
            "direction={}",
            account_id_, req.order_id, static_cast<int>(req.direction));
        reject_order(req, std::format("不支持的方向: {}", static_cast<int>(req.direction)));
        return;
    }
    if (!is_ready()) {
        SPDLOG_WARN("td place_order rejected, not ready | account={} state={}", account_id_,
                    magic_enum::enum_name(state_machine_.state()));
        reject_order(req,
                     std::format("交易未就绪: {}", magic_enum::enum_name(state_machine_.state())));
        return;
    }
    auto it = instrument_exchange_map_.find(req.instrument_id);
    if (it == instrument_exchange_map_.end()) {
        SPDLOG_WARN("td place_order rejected: instrument not found | account={} instrument={}",
                    account_id_, req.instrument_id);
        reject_order(req, std::format("未知合约: {}", req.instrument_id));
        return;
    }
    AccountContext ctx;
    ctx.account_id = account_id_;
    ctx.price_tick = it->second.price_tick;
    if (auto hit = holdings_.find(req.instrument_id); hit != holdings_.end()) {
        ctx.long_pos = hit->second.side(DZ_DIRECTION_LONG).volume();
        ctx.short_pos = hit->second.side(DZ_DIRECTION_SHORT).volume();
    }
    if (auto rej = risk_gate_.check_order(req, ctx)) {
        SPDLOG_WARN("td place_order rejected by risk gate | account={} rule={} reason={}",
                    account_id_, rej->rule_name, rej->reason);
        reject_order(req, std::format("风控拒绝: {}", rej->reason));
        write_risk_reject(account_id_, rej->rule_name, rej->reason);
        return;
    }

    // order_ref 递增 (设计 §9.4)
    ++order_ref_;
    OrderBuildContext build_ctx{};
    build_ctx.account_id = account_id_;  // CTP InvestorID 与 UserID 同值 (ADR 0056)
    build_ctx.order_ref = order_ref_;
    build_ctx.request_id = ++request_id_;
    build_ctx.exchange_id = it->second.exchange_id;

    CThostFtdcInputOrderField input{};
    if (auto mapped = to_input_order_field(req, build_ctx); mapped.has_value()) {
        input = *mapped;
    } else {
        // 前置校验已挡住不支持的枚举; 走到这里说明映射层与校验集合漂移了 —— 仍然拒单,
        // 绝不退回「兜底成限价/开仓」把错误指令发给交易所.
        SPDLOG_ERROR(
            "td place_order rejected: unmappable order | account={} order_id={} "
            "price_type={} position_effect={}",
            account_id_, req.order_id, static_cast<int>(req.price_type),
            static_cast<int>(req.position_effect));
        reject_order(req, "无法映射为柜台委托");
        return;
    }
    // BrokerID 单独填 (CTP 要求, to_input_order_field 不填)
    copy_string(input.BrokerID, broker_id_.c_str(), true);

    // 映射表登记: order_ref -> DzOrderId (发单前登记, 防止 OnRtnOrder 先到)
    // C1: OrderRefMap 内部归一化为 12 位补零格式, 与 CTP 回传格式一致
    std::string order_ref_str = std::to_string(order_ref_);
    order_ref_map_.insert_by_order_ref(order_ref_str, req.order_id);
    // 回报回填: DzOrderId -> strategy_id (on_rtn_order/on_rtn_trade 回填用)
    order_ref_map_.insert_strategy(req.order_id, req.strategy_id);

    int ret = api_->ReqOrderInsert(&input, build_ctx.request_id);
    if (ret != 0) {
        SPDLOG_ERROR("td req order insert failed | account={} ret={} order_id={} order_ref={}",
                     account_id_, ret, req.order_id, order_ref_);
        // 失败回滚映射
        order_ref_map_.erase_by_order_ref(order_ref_str);
        order_ref_map_.erase_strategy(req.order_id);
        reject_order(req, std::format("CTP 下单失败: ret={}", ret));
        return;
    }

    // C4: 登记 DzOrderId -> CancelContext, 供 cancel_order 反向查找.
    // front_id/session_id 初始为 0, on_rtn_order 收到 CTP 回报后 update.
    // order_ref 用 12 位补零格式, 与 to_input_order_field 下单格式一致 (CTP 按字符串比较 OrderRef)
    std::string cancel_order_ref = std::format("{:012}", order_ref_);
    CancelContext cancel_ctx{.order_ref = cancel_order_ref, .front_id = 0, .session_id = 0};
    order_ref_map_.insert_cancel_context(req.order_id, cancel_ctx);

    SPDLOG_INFO("td order submitted | account={} order_id={} order_ref={} instrument={} volume={}",
                account_id_, req.order_id, order_ref_, req.instrument_id, req.volume);
}

bool AccountSession::cancel_order(DzOrderId order_id) {
    if (!is_ready()) {
        SPDLOG_WARN("td cancel_order rejected, not ready | account={} state={} order_id={}",
                    account_id_, magic_enum::enum_name(state_machine_.state()), order_id);
        return false;
    }
    // C4: 反向查找 order_ref + front_id + session_id
    const CancelContext* ctx = order_ref_map_.find_cancel_context(order_id);
    if (ctx == nullptr) {
        SPDLOG_WARN("td cancel_order rejected: order_id not found | account={} order_id={}",
                    account_id_, order_id);
        return false;
    }
    if (api_ == nullptr) {
        SPDLOG_ERROR("td cancel_order failed: api is null | account={} order_id={}", account_id_,
                     order_id);
        return false;
    }

    CThostFtdcInputOrderActionField action{};
    copy_string(action.BrokerID, broker_id_.c_str(), true);
    fill_account_id(action.InvestorID, sizeof(action.InvestorID), account_id_);
    // OrderRef + FrontID + SessionID 三元组定位订单 (CTP 撤单要求)
    copy_string(action.OrderRef, ctx->order_ref.c_str(), true);
    action.FrontID = ctx->front_id;
    action.SessionID = ctx->session_id;
    action.ActionFlag = THOST_FTDC_AF_Delete;

    int ret = api_->ReqOrderAction(&action, ++request_id_);
    if (ret != 0) {
        SPDLOG_ERROR("td req order action failed | account={} ret={} order_id={} order_ref={}",
                     account_id_, ret, order_id, ctx->order_ref);
        return false;
    }
    SPDLOG_INFO("td cancel submitted | account={} order_id={} order_ref={} front={} session={}",
                account_id_, order_id, ctx->order_ref, ctx->front_id, ctx->session_id);
    return true;
}

void AccountSession::set_trading_day(int32_t trading_day) {
    if (trading_day != trading_day_) {
        // 跨交易日: 上一交易日的实时回报柜台时间不再可比 (时间戳含交易日), 清掉基准.
        last_live_report_epoch_s_ = 0;
    }
    trading_day_ = trading_day;
    // 每单成交累计只含当日成交 (契约 §3): 跨日后上一交易日的累计必须清空, 否则会话
    // 存活跨日时均价会混入昨日成交 (重启场景由 load_trading_day_baseline 重新 seed).
    // 登录链次序 = set_trading_day → load_trading_day_baseline, 清空后由装载重建, 安全.
    order_avgs_ = OrderTradeAvgTracker{};
    for (auto& [inst, h] : holdings_) {
        auto ch = h.on_day_switch();
        if (ch.long_changed) push_position(h, DZ_DIRECTION_LONG);
        if (ch.short_changed) push_position(h, DZ_DIRECTION_SHORT);
    }
    SPDLOG_INFO("td trading day updated | account={} trading_day={}", account_id_, trading_day);
}

bool AccountSession::request_cross_day_logout(ReportTradingDayVerdict verdict,
                                              const char* report_day) {
    // 单次语义: 判死后本条回报已丢弃, TdApi::dispatch 会在本次事件处理完后拆会话; 之后同账户
    // 的残留事件走 dispatch 的 unknown-account WARN 兜底, 不再记 ERROR / 推通知 —— 逐条刷
    // ERROR / 通知会把前端通知环打爆 (与持续性异常限频同一口径). 返回值 = 是否本次首次判死
    // (调用方据此决定要不要记 ERROR 日志).
    if (verdict == ReportTradingDayVerdict::Match) {
        return false;  // 调用方只对非 Match 调用; 防御性返回 false (不置请求、不记日志)
    }
    if (cross_day_logout_requested_) {
        return false;
    }
    cross_day_logout_requested_ = true;
    if (notify_) {
        // 主动登出是破坏性事件: ERROR + 弹窗 (登出后由自动调度在下一个 login_time 重登).
        notify_(DZ_NOTIFY_ERROR, cross_day_logout_message(verdict, trading_day_, report_day),
                /*popup=*/true);
    }
    return true;
}

void AccountSession::repush_last_records() {
    // Task 6 (spec §4.2 "最后一条重推"): 在 Ready 广播之后 (另一帧时机) 调用.
    // 从过滤器基准镜像取当日最后一条委托/成交, 带原 seq **直接写帧**:
    // 不经过滤器 (已是基准最新态, 再 check 会因同字段集被吞)、不落库 (DB 已含该记录).
    // 角色 = 保证到达的触发帧: 重放全被吞 / 行情安静时, 登录后必有一帧带 seq 到来,
    // 把消费端"断档挂到开盘"收成"登录完成即自愈". 无记录则不推 (无断档可能).
    // 终检发现 6: 委托/成交各自取最新会推两条帧 — 成交 seq < 委托 seq (常见) 时,
    // 低 seq 帧后到会被消费端 detect_reset 误判倒退, 触发全账户 rebuild (结果正确
    // 但开销 + 噪声, 且 reset 清 applied_trades)。只推两帧中 seq 较大的一条:
    // 消费端 last_applied 低于它才需要触发帧, 推大 seq 一条已覆盖该目的且无伪倒退.
    try {
        const DzOrderReport* latest_order = report_filter_->find_latest_order();
        const DzTradeReport* latest_trade = report_filter_->find_latest_trade();
        if (latest_order != nullptr &&
            (latest_trade == nullptr || latest_order->seq >= latest_trade->seq)) {
            platform::write_struct(event_writer_, DZ_FRAME_ORDER_REPORT, *latest_order);
            SPDLOG_DEBUG("td repush last order | account={} order_id={} seq={}", account_id_,
                         latest_order->order_id, latest_order->seq);
        } else if (latest_trade != nullptr) {
            platform::write_struct(event_writer_, DZ_FRAME_TRADE_REPORT, *latest_trade);
            SPDLOG_DEBUG("td repush last trade | account={} trade_id={} seq={}", account_id_,
                         latest_trade->trade_id, latest_trade->seq);
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td repush last records failed | account={} error=\"{}\"", account_id_,
                     e.what());
    }
}

void AccountSession::delete_event(Event& event) noexcept {
    if (event.data == nullptr) return;
    // td 关心的事件走 td_delete_event_data:
    //   - td 类型 (>=100)
    //   - OnFrontConnected / OnFrontDisconnected (td 进程用 td 版本 Field, 不可走 md 路径)
    // 其他 md 类型 (3, 4-13) 走 Event::delete_data (md 内联)
    if (static_cast<int16_t>(event.type) >= 100 || event.type == EventType::OnFrontConnected ||
        event.type == EventType::OnFrontDisconnected) {
        td_delete_event_data(event);
    } else {
        event.delete_data();
    }
}

// ============================================================================
// 内部辅助: CTP 请求
// ============================================================================

void AccountSession::req_authenticate() {
    CThostFtdcReqAuthenticateField f{};
    copy_string(f.BrokerID, broker_id_.c_str(), true);
    fill_account_id(f.UserID, sizeof(f.UserID), account_id_);
    copy_string(f.AppID, app_id_.c_str(), true);
    copy_string(f.AuthCode, auth_code_.c_str(), true);
    // 测试注入的发送钩子优先 (空 = 真实 api_); 与 set_settlement_send_hook 同形态 ——
    // 没有它, 两条认证失败出口只能靠读代码确认 (docs/unit-test-standard.md §1.1).
    int ret = auth_send_hook_ ? auth_send_hook_(f, ++request_id_)
                              : api_->ReqAuthenticate(&f, ++request_id_);
    if (ret != 0) {
        SPDLOG_ERROR("td req authenticate failed | account={} ret={}", account_id_, ret);
        return;
    }
    state_machine_.on_req_authenticate();
    // 认证超时（生产 10s；测试可缩短到 0 走真 TimerQueue 回调）
    uint64_t gen = generation_;
    login_timer_id_ = timer_queue_.schedule_after(auth_timeout_, [this, gen]() {
        if (gen != generation_) return;
        on_auth_timeout();
    });
}

void AccountSession::on_auth_timeout() {
    // 生产由 TimerQueue 在 10s 后回调; 测试经 expire_auth_timeout_for_test 注入同一收口.
    // 已离开 Authenticating (响应已到 / 已断线 / 已登出) 时忽略迟到超时: 此时弹窗对不上
    // 界面状态, 也不该把已前进的链硬拽回失败态。
    if (state_machine_.state() != TdState::Authenticating) return;
    SPDLOG_ERROR("td authenticate timeout | account={}", account_id_);
    // #489 并入 #494: 超时与柜台拒绝同等对待 (ADR 0059 决策 5) —— 同一失败态、
    // 同一 last_error 写入、同一条通知出口, 只靠 code 区分。
    notify_auth_failed(TdFailureCode::AuthTimeout, "10 秒内未收到柜台的认证响应");
}

void AccountSession::set_auth_send_hook(AuthSendFn hook) { auth_send_hook_ = std::move(hook); }

void AccountSession::set_login_send_hook(LoginSendFn hook) { login_send_hook_ = std::move(hook); }

void AccountSession::request_login_for_test() { req_login(); }

void AccountSession::expire_login_timeout_for_test() { on_login_timeout(); }

void AccountSession::expire_connect_timeout_for_test() { on_connect_timeout(); }

void AccountSession::set_login_timeout_for_test(std::chrono::seconds timeout) {
    login_timeout_ = timeout;
}

void AccountSession::request_authenticate_for_test() { req_authenticate(); }

void AccountSession::expire_auth_timeout_for_test() { on_auth_timeout(); }

void AccountSession::set_auth_timeout_for_test(std::chrono::seconds timeout) {
    auth_timeout_ = timeout;
}

void AccountSession::push_notification(const std::optional<TdNotification>& notif,
                                       const std::string& message,
                                       std::string_view prose) {
    // 状态机拒绝了这次转移 (此刻不在该出口对应的那一段状态, 已由状态机记 WARN): 此刻弹窗
    // 对不上界面状态, 不推送 (具体哪一段见各调用点)。
    if (!notif) {
        return;
    }
    if (!notify_) {
        return;  // 未注入 (测试 / 无 UI): 状态已转移, 仅少一条通知, 不产生日志噪音
    }
    try {
        notify_(notif->level, message, notif->popup);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("{} | account={} error=\"{}\"", prose, account_id_, e.what());
    } catch (...) {
        SPDLOG_ERROR("{} | account={} error=unknown_exception", prose, account_id_);
    }
}

void AccountSession::notify_auth_failed(TdFailureCode code,
                                        const std::string& reason,
                                        int error_id) {
    // #489 并入 #494: 认证失败的两处调用点 (响应被拒 / 10 秒超时) 此前都丢弃状态机返回值,
    // 账户静默停在 2/10「已连接」。现在统一走本出口: 转状态 (停在 AuthFailed 3/10) +
    // 写 last_error + 弹窗, 与 #407 / #488 同类缺陷的处置一致。
    // 先转状态再通知 (与 md 一致): last_error 与状态在同一次转移写出 (ADR 0059 决策 2)。
    const auto notif = state_machine_.on_authenticate_failed(code, reason, error_id);
    push_notification(notif, auth_failed_message(code, reason), "td auth failed notify failed");
}

void AccountSession::notify_login_failed(TdFailureCode code,
                                         const std::string& reason,
                                         int error_id) {
    // 先转状态 (停在 LoginFailed 5/10) + 写 last_error, 再通知 —— 与 md 的
    // 「先 set_last_error + set_state, 再返回通知」时序一致.
    const auto notif = state_machine_.on_login_failed(code, reason, error_id);
    push_notification(notif, login_failed_message(code, reason), "td login failed notify failed");
}

void AccountSession::notify_settlement_confirm_failed(TdFailureCode code,
                                                      const std::string& reason,
                                                      int error_id) {
    // #488: 与 notify_login_failed 同结构 —— 先收定时器, 再转状态 (停在失败态), 再通知.
    // 看门狗收口在失败出口统一取消: 每个调用点各写一遍只会漏一处 (此前 5 个调用点
    // 全部丢弃状态机返回值, 失败无痕)。
    cancel_settlement_confirm_timer();
    settlement_confirm_request_id_ = 0;  // 出链即作废, 迟到响应的 request_id 校验依赖它
    const auto notif = state_machine_.on_settlement_confirm_failed(code, reason, error_id);
    push_notification(notif, settlement_confirm_failed_message(code, reason),
                      "td settlement confirm failed notify failed");
}

void AccountSession::notify_instruments_load_failed(TdFailureCode code,
                                                    const std::string& reason,
                                                    int error_id) {
    // #488: 同上 —— 收定时器 + 转状态 + 通知. #496 起 on_instruments_load_failed 停在
    // InstrumentsLoadFailed (8/10「合约加载失败」), 不再回退 LoggedIn 这个正常中间态,
    // 且不会有下一次尝试 —— 不上浮就是静默卡住.
    cancel_instruments_load_timer();
    const auto notif = state_machine_.on_instruments_load_failed(code, reason, error_id);
    push_notification(notif, instruments_load_failed_message(code, reason),
                      "td instruments load failed notify failed");
}

void AccountSession::req_login() {
    CThostFtdcReqUserLoginField f{};
    copy_string(f.BrokerID, broker_id_.c_str(), true);
    fill_account_id(f.UserID, sizeof(f.UserID), account_id_);
    copy_string(f.Password, password_.c_str(), true);
    // 测试注入的发送钩子优先 (空 = 真实 api_); 与 set_auth_send_hook 同形态 ——
    // 没有它, 登录段的五条失败出口只能靠读代码确认 (docs/unit-test-standard.md §1.1).
    int ret = login_send_hook_ ? login_send_hook_(f, ++request_id_)
                               : api_->ReqUserLogin(&f, ++request_id_);
    if (ret != 0) {
        // #495: 发送被拒也是登录段的一种失败 —— 此前只记日志后 return, 状态停在
        // Authenticated(4)/Connected(2)、定时器未武装, 账户静默卡在链上, 而前端清 pending
        // 的唯一主信号是 last_error 新身份, 这条路径永不产生 → 只能等 45s 兜底。
        // 与结算确认发送被拒同一处置 (notify_settlement_confirm_failed)。
        // 先转 LoggingIn 再走失败出口: 失败出口只在 LoggingIn 有效 (与其它四条路径同一条缝)。
        // ret 是 CTP API 返回码 (-1 网络 / -2 未登录 / -3 流控), 不是柜台裁决 —— 文案不写
        // 「柜台拒绝」, 码也用独立的 login_send_failed (不与 login_rejected 混同)。
        SPDLOG_ERROR("td req login failed | account={} ret={}", account_id_, ret);
        state_machine_.on_req_login();
        notify_login_failed(TdFailureCode::LoginSendFailed,
                            std::format("发送登录请求失败（ret={}）", ret));
        return;
    }
    state_machine_.on_req_login();
    // 登录超时（生产 10s；测试可缩短到 0 走真 TimerQueue 回调）
    uint64_t gen = generation_;
    login_timer_id_ = timer_queue_.schedule_after(login_timeout_, [this, gen]() {
        if (gen != generation_) return;
        on_login_timeout();
    });
}

void AccountSession::on_login_timeout() {
    // 生产由 TimerQueue 在 10s 后回调; 测试经 expire_login_timeout_for_test 注入同一收口.
    // 已离开 LoggingIn (响应已到 / 已断线 / 已登出) 时忽略迟到超时: 此时弹窗对不上
    // 界面状态, 也不该把已前进的链硬拽回失败态。
    if (state_machine_.state() != TdState::LoggingIn) return;
    SPDLOG_ERROR("td login timeout | account={}", account_id_);
    // 超时与柜台拒绝同等对待 (ADR 0059 决策 5): 同一失败态、同一 last_error 写入,
    // 只靠 code 区分。
    notify_login_failed(TdFailureCode::LoginTimeout, "10 秒内未收到柜台的登录响应");
}

void AccountSession::req_settlement_confirm() {
    CThostFtdcSettlementInfoConfirmField f{};
    copy_string(f.BrokerID, broker_id_.c_str(), true);
    fill_account_id(f.InvestorID, sizeof(f.InvestorID), account_id_);
    // #488: 先转状态再发送 —— 发送被拒与响应失败/超时从同一状态 (Confirming) 出链,
    // 失败出口因此唯一 (notify_settlement_confirm_failed)。此前 ret != 0 只记日志:
    // 状态停在 LoggedIn、回调永不到来, 账户静默停在 6/10。
    (void)state_machine_.on_req_settlement_confirm();
    if (state_machine_.state() != TdState::Confirming) {
        // on_req_settlement_confirm() 只在状态非 LoggedIn 时拒绝转移 (已由状态机记 WARN)。
        // 此时不发送, 免得响应回来没有归属。
        SPDLOG_WARN("td settlement confirm skipped | account={} state={}", account_id_,
                    magic_enum::enum_name(state_machine_.state()));
        return;
    }
    // #488 看门狗: 柜台不回 OnRspSettlementInfoConfirm 时状态机会永久停在 Confirming (7/10),
    // 而本进程不自动重试登录链 —— 没有看门狗就是静默卡死。时长依据见
    // kSettlementConfirmTimeout (实测该响应 13.5s / 4.3s, 留 4.4 倍余量)。
    // **排在发送之前**: 发送本身也可能抛 (注入钩子 / SDK), 那一刻状态已是 Confirming,
    // 若不先武装看门狗就会留下「无定时器、无回调」的永久 Confirming —— 正是本票要消灭的形态。
    uint64_t gen = generation_;
    std::weak_ptr<void> weak = alive_token_;
    settlement_confirm_timer_id_ =
        timer_queue_.schedule_after(settlement_confirm_timeout_, [this, weak, gen]() {
            // 未跟踪的回调先查存活令牌: logout 会 erase 会话, 不查就 UAF。
            if (weak.expired()) return;
            if (gen != generation_) return;  // 断线重连后旧代际的回调作废
            on_settlement_confirm_timeout();
        });
    // 测试注入的发送钩子优先 (空 = 真实 api_); 与 set_query_send_hook 同形态 ——
    // 没有它, 本函数的看门狗排定与 ret != 0 失败出口只能靠读代码确认 (失效测试形态)。
    settlement_confirm_request_id_ = ++request_id_;
    int ret = settlement_send_hook_
                  ? settlement_send_hook_(f, settlement_confirm_request_id_)
                  : api_->ReqSettlementInfoConfirm(&f, settlement_confirm_request_id_);
    if (ret != 0) {
        SPDLOG_ERROR("td req settlement confirm failed | account={} ret={}", account_id_, ret);
        // ret 是 CTP API 返回码 (-1 网络 / -2 未登录 / -3 流控), 不是柜台裁决 —— 文案不写成
        // 「柜台拒绝」。码仍归 settlement_rejected: 契约 td-status §2.5 把「发送被拒」与
        // 「柜台拒绝」并列为同一码 (用户看到的是同一件事: 这次确认被拒了)。
        // 看门狗由统一失败出口取消。
        notify_settlement_confirm_failed(TdFailureCode::SettlementRejected,
                                         std::format("发送结算单确认请求失败（ret={}）", ret));
        return;
    }
}

void AccountSession::on_settlement_confirm_timeout() {
    // 生产路径由 TimerQueue 在 settlement_confirm_timeout_ 后回调; 测试经
    // expire_settlement_confirm_timeout_for_test 直接注入同一收口, 或用 0 延迟时长 +
    // TimerQueue::tick() 走真回调 (都不 sleep)。
    // 迟到超时 (响应已到 / 已断线 / 已登出) 一律忽略: 状态已离开 Confirming,
    // 此时转失败态与弹窗都会对不上界面状态。
    if (state_machine_.state() != TdState::Confirming) return;
    SPDLOG_ERROR("td settlement confirm timeout | account={} timeout_s={}", account_id_,
                 settlement_confirm_timeout_.count());
    // 超时与柜台拒绝同等对待 (ADR 0059 决策 5): 同一失败态、同一 last_error 写入, 只靠
    // code 区分。
    notify_settlement_confirm_failed(
        TdFailureCode::SettlementTimeout,
        std::format("{} 秒内未收到柜台的结算单确认响应", settlement_confirm_timeout_.count()));
}

void AccountSession::req_qry_instrument() {
    // ADR 0055: 查询通道闸门（登录链三查询与运行期资金刷新共用）。
    if (defer_query_if_pacer_busy(
            [this]() {
                if (state_machine_.state() == TdState::LoadingInstruments) {
                    req_qry_instrument();
                }
            },
            "instrument")) {
        return;
    }
    // C2: 查询全部合约, 填充 instrument_exchange_map_ (设计 §7.2)
    // 终检发现 1: 发起时快照代际, 响应侧校验 (陈旧响应丢弃)
    query_gen_ = generation_;
    CThostFtdcQryInstrumentField qry{};
    // InstrumentID 留空: 查询所有合约
    instrument_query_request_id_ = ++request_id_;
    // 测试注入的发送钩子优先 (空 = 真实 api_); 与 set_settlement_send_hook 同形态。
    int ret = instrument_send_hook_ ? instrument_send_hook_(qry, instrument_query_request_id_)
                                    : api_->ReqQryInstrument(&qry, instrument_query_request_id_);
    if (ret != 0) {
        note_query_done();
        instrument_query_request_id_ = 0;
        if (ret == -3) {
            // I3: 流控 (-3), 1.5s 后重试 (参考 mdctp 流控队列模式)
            SPDLOG_WARN("td qry instrument flow control, retry in 1.5s | account={}", account_id_);
            // #508: 流控重试也属于「加载合约」这一段过程, 必须保证看门狗在场——
            // 漏武装时持续流控会让账户永久停在 8/10（既不失败也不就绪）。
            // 幂等: 已经在计时就不重置 deadline, 防止无限流控靠重试续命。
            arm_instruments_load_watchdog();
            uint64_t gen = generation_;
            std::weak_ptr<void> weak = alive_token_;
            timer_queue_.schedule_after(std::chrono::milliseconds(1500), [this, weak, gen]() {
                if (weak.expired()) return;
                if (gen != generation_) return;
                if (state_machine_.state() == TdState::LoadingInstruments) {
                    req_qry_instrument();
                }
            });
            return;
        }
        // C2: 非 -3 错误, 停在 InstrumentsLoadFailed (8/10「合约加载失败」), 不进 Ready (#496);
        // #488 失败原因上浮 UI。
        // ret 是 CTP API 返回码 (-1 网络 / -2 未登录 / -3 流控), 不是柜台裁决。
        // instruments_rejected 的第一种来源 = 查询发送被拒 (契约 td-status §2.5)。
        SPDLOG_ERROR("td req qry instrument failed | account={} ret={}", account_id_, ret);
        notify_instruments_load_failed(TdFailureCode::InstrumentsRejected,
                                       std::format("发送合约查询请求失败（ret={}）", ret));
        return;
    }
    note_query_sent();
    // I2: 排定 5 分钟超时, 防止 CTP 长期不回 is_last 导致卡死 (设计 §2.4.1)
    arm_instruments_load_watchdog();
}

void AccountSession::cancel_connect_timer() {
    if (connect_timer_id_ != 0) {
        timer_queue_.cancel(connect_timer_id_);
        connect_timer_id_ = 0;
    }
}

void AccountSession::req_qry_investor_position() {
    // 登录 / 重连后的基准查询起点: 重置重试预算, 开始首次尝试 (ADR 0049 决策 3).
    cancel_position_query_retry_timer();
    cancel_position_query_timeout_timer();
    position_query_retries_ = 0;
    req_qry_investor_position_once();
}

void AccountSession::req_qry_investor_position_once() {
    // ADR 0055: 查询通道闸门（不足最小间隔时延后到合法时刻再发）。
    if (defer_query_if_pacer_busy(
            [this]() {
                if (state_machine_.state() == TdState::LoadingInstruments &&
                    finalizer_.phase() == Phase::kQueryPosition) {
                    req_qry_investor_position_once();
                }
            },
            "position")) {
        return;
    }
    ++position_query_token_;
    query_gen_ = generation_;
    position_query_agg_.clear();
    position_query_group_.clear();
    position_rebuild_consumed_ = false;
    position_query_unknown_direction_ = false;
    position_query_in_flight_ = true;
    if (api_ == nullptr) {
        on_position_query_attempt_failed("trader api is null");
        return;
    }
    CThostFtdcQryInvestorPositionField qry{};
    position_query_request_id_ = ++request_id_;
    int ret = api_->ReqQryInvestorPosition(&qry, position_query_request_id_);
    if (ret != 0) {
        // -1 / -2 / -3 都算一次失败; -3 流控同样计入重试预算 (ADR 0049 决策 3).
        on_position_query_attempt_failed(std::format("ReqQryInvestorPosition ret={}", ret));
        return;
    }
    note_query_sent();
    // 超时兜底: 登录阶段 5 分钟 (查询只在登录收尾链发起, 不存在 Ready 阶段的查询).
    cancel_position_query_timeout_timer();
    uint64_t gen = generation_;
    uint64_t token = position_query_token_;
    std::weak_ptr<void> weak = alive_token_;
    position_query_timeout_timer_id_ =
        timer_queue_.schedule_after(std::chrono::minutes(5), [this, weak, gen, token]() {
            if (weak.expired()) return;
            if (gen != generation_) return;
            position_query_timeout_timer_id_ = 0;
            if (token != position_query_token_) return;  // 已有更新的查询, 本次超时作废
            if (!position_query_in_flight_) return;      // 响应已到, 超时作废
            if (state_machine_.state() != TdState::LoadingInstruments ||
                finalizer_.phase() != Phase::kQueryPosition) {
                return;
            }
            SPDLOG_ERROR("td qry position timeout | account={}", account_id_);
            on_position_query_attempt_failed("query timeout 5min");
        });
}

void AccountSession::on_position_query_attempt_failed(std::string_view reason) {
    if (finalizer_.position_failed()) {
        // 本次登录已判失败: 重复/迟到失败不再计数或重复弹窗 (幂等防御).
        return;
    }
    position_query_in_flight_ = false;
    position_query_ok_ = false;
    note_query_done();
    cancel_position_query_timeout_timer();
    if (position_query_retry_allowed(position_query_retries_)) {
        ++position_query_retries_;
        SPDLOG_WARN(
            "td position baseline query failed, retry scheduled | account={} retry={}"
            " max_retries={} reason=\"{}\"",
            account_id_, position_query_retries_, kPositionQueryMaxRetries, reason);
        cancel_position_query_retry_timer();
        uint64_t gen = generation_;
        std::weak_ptr<void> weak = alive_token_;
        position_query_retry_timer_id_ =
            timer_queue_.schedule_after(kPositionQueryRetryInterval, [this, weak, gen]() {
                if (weak.expired()) return;
                if (gen != generation_) return;
                position_query_retry_timer_id_ = 0;
                // 断连 / 已收尾时不再重试 (与超时回调同一道防御).
                if (state_machine_.state() != TdState::LoadingInstruments ||
                    finalizer_.phase() != Phase::kQueryPosition) {
                    return;
                }
                req_qry_investor_position_once();
            });
        return;
    }
    // 重试预算用尽 → 本次登录失败: 停在当前进度 (LoadingInstruments)、不转 Ready、
    // 推 ERROR 通知, 且不再排任何定时器 —— 不自动恢复, 等用户处理 (ADR 0049 决策 3).
    cancel_position_query_retry_timer();
    finalizer_.on_position_failed();
    SPDLOG_ERROR(
        "td position baseline query failed, account not ready | account={} retries={}"
        " reason=\"{}\"",
        account_id_, position_query_retries_, reason);
    notify_position_query_failed();
}

void AccountSession::notify_position_query_failed() {
    if (!notify_) {
        return;  // 未注入 (测试/无 UI): 仅日志, 上文已记 ERROR
    }
    try {
        notify_(DZ_NOTIFY_ERROR, position_query_failed_message(), /*popup=*/true);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td position query failed notify failed | account={} error=\"{}\"",
                     account_id_, e.what());
    } catch (...) {
        SPDLOG_ERROR("td position query failed notify failed | account={} error=unknown_exception",
                     account_id_);
    }
}

void AccountSession::cancel_position_query_retry_timer() {
    if (position_query_retry_timer_id_ != 0) {
        timer_queue_.cancel(position_query_retry_timer_id_);
        position_query_retry_timer_id_ = 0;
    }
}

void AccountSession::cancel_position_query_timeout_timer() {
    if (position_query_timeout_timer_id_ != 0) {
        timer_queue_.cancel(position_query_timeout_timer_id_);
        position_query_timeout_timer_id_ = 0;
    }
}

void AccountSession::req_qry_trading_account() {
    // Task 6 (spec §4.2 登录收尾): 持仓完成后发起资金查询 (CTP 流控串行).
    if (api_ == nullptr) {
        finalizer_.on_account_failed();
        finalize_login();
        return;
    }
    // ADR 0055: 查询通道闸门（不足最小间隔时延后到合法时刻再发）。
    if (defer_query_if_pacer_busy(
            [this]() {
                if (state_machine_.state() == TdState::LoadingInstruments &&
                    finalizer_.phase() == Phase::kQueryAccount) {
                    req_qry_trading_account();
                }
            },
            "trading_account")) {
        return;
    }
    // 终检发现 1: 发起时快照代际, 响应侧校验 (陈旧响应丢弃)
    query_gen_ = generation_;
    CThostFtdcQryTradingAccountField qry{};
    account_query_request_id_ = ++request_id_;
    int ret = api_->ReqQryTradingAccount(&qry, account_query_request_id_);
    if (ret != 0) {
        note_query_done();
        account_query_request_id_ = 0;
        if (ret == -3) {
            SPDLOG_WARN("td qry account flow control, retry in 1.5s | account={}", account_id_);
            // #508: 与合约查询同型——流控重试路径也必须保证收尾看门狗在场,
            // 否则持续流控会让账户永久停在 8/10（既不失败也不就绪）。
            arm_account_query_watchdog();
            uint64_t gen = generation_;
            std::weak_ptr<void> weak = alive_token_;
            timer_queue_.schedule_after(std::chrono::milliseconds(1500), [this, weak, gen]() {
                if (weak.expired()) return;
                if (gen != generation_) return;
                if (finalizer_.phase() == Phase::kQueryAccount) {
                    req_qry_trading_account();
                }
            });
            return;
        }
        SPDLOG_ERROR("td req qry account failed | account={} ret={}", account_id_, ret);
        finalizer_.on_account_failed();
        finalize_login();
        return;
    }
    note_query_sent();
    // 超时兜底: 登录阶段 5 分钟 (查询只在登录收尾链发起).
    arm_account_query_watchdog();
}

void AccountSession::arm_account_query_watchdog() {
    // 与合约查询看门狗同型: 计的是整段查询过程, 故带 tag 去重（已在计时则忽略）。
    uint64_t gen = generation_;
    std::weak_ptr<void> weak = alive_token_;
    timer_queue_.schedule_after_ignored(
        "td_qry_account_timeout", std::chrono::minutes(5), [this, weak, gen]() {
            if (weak.expired()) return;
            if (gen != generation_) return;
            if (state_machine_.state() == TdState::LoadingInstruments &&
                finalizer_.phase() == Phase::kQueryAccount) {
                SPDLOG_ERROR("td qry account timeout, degrade | account={}", account_id_);
                note_query_done();
                account_query_request_id_ = 0;
                finalizer_.on_account_failed();
                finalize_login();
            }
        });
}

void AccountSession::drive_finalizer() {
    // 登录收尾状态机线性推进 (spec §4.2).
    // 每个阶段动作完成后调 next() 取下一阶段; 未满足前置时 next() 停留.
    // 查询阶段由 SPI 回调驱动: 资金 is_last 处 on_account_done 已使 phase 进入
    // kReplay, 随后 finalize_login() 由此推进 replay -> flush -> ready.
    while (finalizer_.phase() != Phase::kDone) {
        switch (finalizer_.phase()) {
            case Phase::kQueryPosition:
            case Phase::kQueryAccount:
                // 查询阶段由 SPI 回调 (on_rsp_qry_* is_last/失败) 驱动, 这里不推进.
                return;
            case Phase::kReplay:
                replay_buffered_reports();
                break;
            case Phase::kFlush:
                // flush 屏障: 排空 persist 队列且末批已提交后才转 Ready (spec 屏障语义).
                // 登录不在 30μs 热路径, 几 ms 可接受; 5s 超时兜底 (超时仍继续, 不阻塞
                // Ready; ADR 0042 §5) —— 超时的 ERROR 通知由 run_flush_barrier 统一发出.
                (void)run_flush_barrier(
                    [this](std::chrono::milliseconds timeout) {
                        const uint64_t token = persist_writer_.enqueue_flush_signal();
                        return persist_writer_.wait_flush(token, timeout);
                    },
                    std::chrono::seconds(5), notify_, account_id_);
                break;
            case Phase::kReady:
                // flush 屏障已放行 (成功提交或超时降级). Ready 语义 = 登录完成、可交易,
                // 不承诺 DB 稳定 (ADR 0042 §5). 状态机翻转 Ready (设计 §5.8);
                // handler 返回后 TdApi 检测 Ready 翻转 -> 广播 2018.
                if (state_machine_.state() == TdState::LoadingInstruments) {
                    state_machine_.on_instruments_loaded();
                }
                break;
            case Phase::kDone:
                return;
        }
        (void)finalizer_.next();
    }
}

void AccountSession::finalize_login() {
    // 持仓查询成功 + 资金查询完结后才进入收尾序列. 防御: 未达前置时 no-op
    // (持仓查询重试耗尽时 can_reach_ready() 恒 false → 不转 Ready).
    if (!finalizer_.can_reach_ready()) {
        return;
    }
    drive_finalizer();
}

void AccountSession::cancel_login_timer() {
    if (login_timer_id_ != 0) {
        timer_queue_.cancel(login_timer_id_);
        login_timer_id_ = 0;
    }
}

void AccountSession::cancel_settlement_confirm_timer() {
    if (settlement_confirm_timer_id_ != 0) {
        timer_queue_.cancel(settlement_confirm_timer_id_);
        settlement_confirm_timer_id_ = 0;
    }
}

void AccountSession::cancel_instruments_load_timer() {
    if (instruments_load_timer_id_ != 0) {
        timer_queue_.cancel(instruments_load_timer_id_);
        instruments_load_timer_id_ = 0;
    }
}

void AccountSession::arm_instruments_load_watchdog() {
    // 幂等: 已在计时就不动 deadline —— 看门狗量的是整段加载过程, 不是单次请求。
    // 每条重试路径都调它（首次发送 / 流控 -3 重发）不会把 5 分钟往后推。
    if (instruments_load_timer_id_ != 0) {
        return;
    }
    uint64_t gen = generation_;
    instruments_load_timer_id_ =
        timer_queue_.schedule_after(instruments_load_timeout_, [this, gen]() {
            if (gen != generation_) return;
            if (state_machine_.state() == TdState::LoadingInstruments) {
                SPDLOG_ERROR("td instruments load timeout | account={} timeout_s={}", account_id_,
                             instruments_load_timeout_.count());
                note_query_done();
                instrument_query_request_id_ = 0;
                // #488: 原因改中文 —— 它会进用户弹窗。时长随实际配置, 与看门狗同源。
                notify_instruments_load_failed(
                    TdFailureCode::InstrumentsTimeout,
                    std::format("查询 {} 秒未收到最后一批响应", instruments_load_timeout_.count()));
            }
        });
}

// ============================================================================
// 内部辅助: SHM 推送 + 持久化
// ============================================================================

void AccountSession::write_order_rpt(const DzOrderReport& rpt) {
    // DZ_FRAME_ORDER_REPORT (2000), payload=DzOrderReport 通用字段
    platform::write_struct(event_writer_, DZ_FRAME_ORDER_REPORT, rpt);
}

void AccountSession::write_trade_rpt(const DzTradeReport& rpt) {
    // DZ_FRAME_TRADE_REPORT (2001), payload=DzTradeReport 通用字段
    platform::write_struct(event_writer_, DZ_FRAME_TRADE_REPORT, rpt);
}

void AccountSession::report_unknown_enums(const UnknownEnumSink& unknown) {
    if (unknown.empty()) {
        return;
    }
    // 限频: 未知枚举可能每条回报都带 (某合约状态一直是新值). 逐条 notify 会刷爆前端通知环.
    // 间隔内只计数 + 记日志, 不通知.
    const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    const bool report_due =
        alert_report_due(now_s, unknown_enum_last_report_s_, kUnknownEnumReportIntervalS);
    bool first_of_batch = true;
    for (size_t i = 0; i < unknown.count; ++i) {
        const UnknownEnumInfo& info = unknown.items[i];
        // 按种类分开计数: 「我们没跟上柜台」与「柜台给了坏数据」处置不同 (issue #367 故事 8).
        uint64_t count = 0;
        std::string_view label_zh;
        if (std::string_view(info.category) == kUnknownCategoryOrderStatus) {
            count = ++unknown_enum_counts_.order_status;
            label_zh = "委托状态";
        } else if (std::string_view(info.category) == kUnknownCategoryDirection) {
            count = ++unknown_enum_counts_.direction;
            label_zh = "持仓方向";
        } else if (std::string_view(info.category) == kUnknownCategoryPriceType) {
            count = ++unknown_enum_counts_.price_type;
            label_zh = "价格类型";
        } else if (std::string_view(info.category) == kUnknownCategoryPositionEffect) {
            count = ++unknown_enum_counts_.position_effect;
            label_zh = "开平标志";
        } else {
            // 分类标签只由 td_ctp_mapping.h 的常量产出; 走到这里说明有新分类没接入.
            continue;
        }
        // 原始值用数值码记录 (CTP 侧枚举是 char 字面量, 直接格式化不可打印字符会丢证据).
        SPDLOG_ERROR("td unknown enum | account={} category={} raw_code={} count={}", account_id_,
                     info.category, static_cast<int>(info.raw_char), count);
        if (!report_due) {
            continue;  // 计数与日志已记, 通知在间隔内跳过
        }
        // 同一批里只通知一次: 一条回报可能同时带多个未知枚举, 逐条通知是重复刷屏.
        if (!first_of_batch) {
            continue;
        }
        first_of_batch = false;
        if (notify_) {
            try {
                notify_(DZ_NOTIFY_ERROR,
                        std::format("收到未知{}（原始码 {}），已标记为未知并计数", label_zh,
                                    static_cast<int>(info.raw_char)),
                        /*popup=*/false);
            } catch (const std::exception& e) {
                SPDLOG_ERROR("td unknown enum notify failed | account={} error=\"{}\"", account_id_,
                             e.what());
            }
        }
    }
    if (!report_due || first_of_batch) {
        return;  // 间隔内仅计数与日志
    }
    // 通知已发出一条: 推进限频窗口. 未知枚举只计数 + 通知, 不触发任何查询
    // (ADR 0049: 取消周期查询后不存在"重查纠正"路径; 可观测性由计数与通知承接).
    unknown_enum_last_report_s_ = now_s;
}

void AccountSession::reject_order(const DzOrderReq& req, const std::string& reason) {
    // C3: 推 REJECTED 回报让策略进程感知, 避免静默丢单 (设计 §11.1)
    // 异常不传播, 仅记日志 (符合 "宁肯乱码也不能崩溃")
    try {
        DzOrderReport rpt{};
        rpt.order_id = req.order_id;
        copy_string(rpt.strategy_id, req.strategy_id, true);
        copy_string(rpt.instrument_id, req.instrument_id, true);
        copy_string(rpt.account_id, req.account_id, true);
        rpt.direction = req.direction;
        rpt.price_type = req.price_type;
        rpt.position_effect = req.position_effect;
        rpt.status = DZ_ORDER_REJECTED;
        rpt.price = req.price;
        rpt.volume = req.volume;
        rpt.volume_traded = 0;
        rpt.date = trading_day_;
        rpt.time = 0;  // 拒单时间未定义, 留 0 (策略进程可按接收时刻处理)
        // exchange_id 从映射表查 (place_order 已校验存在, 这里兜底防异常)
        auto it = instrument_exchange_map_.find(req.instrument_id);
        if (it != instrument_exchange_map_.end()) {
            copy_string(rpt.exchange_id, it->second.exchange_id.c_str(), true);
        }
        copy_string(rpt.remark, reason.c_str(), true);

        // 持久化 OrderRecord (含 CTP 扩展字段, 留 0/空)
        OrderRecord rec{};
        rec.base = rpt;
        rec.is_external = 0;
        rec.volume_canceled = req.volume;
        rec.error_id = -1;  // 本地拒绝, 用 -1 区分 CTP 错误码
        copy_string(rec.error_msg, reason.c_str(), true);
        if (trading_day_ > 0) {
            dztrader::Date d{trading_day_};
            auto* end = std::format_to_n(rec.trading_day, sizeof(rec.trading_day) - 1,
                                         "{:04d}{:02d}{:02d}", d.year(), d.month(), d.day())
                            .out;
            *end = '\0';
        }

        // Task 5: 本地拒单不经重放过滤器 (本地事件必新, spec §4.1 含本地拒单路径收口).
        // 分配 seq (与 CTP 回报同主线程交错时, 同一函数内先 ++seq 后 write_*,
        // 无并发, 帧序 = seq 序单调性成立) + 更新过滤器基准 (防后续重放误转发).
        rec.base.seq = ++seq_counter_;
        if (report_filter_) {
            report_filter_->accept_order(rec);
        }

        write_order_rpt(rec.base);
        persist_order(rec);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td reject_order failed | account={} order_id={} error=\"{}\"", account_id_,
                     req.order_id, e.what());
    }
}

void AccountSession::write_risk_reject(const std::string& account_id,
                                       const std::string& rule_name,
                                       const std::string& reason) {
    // C3: 推 DZ_FRAME_TD_RISK_REJECT, payload=JSON (契约 td-risk-reject)
    // 异常不传播 (设计 §8.2)
    try {
        platform::DzRiskReject reject;
        reject.account_id = account_id;
        reject.rule_name = rule_name;
        reject.reason = reason;
        reject.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        platform::write_ext_json(event_writer_, DZ_FRAME_TD_RISK_REJECT, reject);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td write_risk_reject failed | account={} error=\"{}\"", account_id_,
                     e.what());
    }
}

void AccountSession::persist_order(const OrderRecord& r) {
    persist_writer_.enqueue(PersistTask{PersistTask::Kind::Order, r});
}

void AccountSession::persist_trade(const TradeRecord& r) {
    persist_writer_.enqueue(PersistTask{PersistTask::Kind::Trade, r});
}

PositionHolding* AccountSession::ensure_holding(const std::string& instrument_id,
                                                const std::string& exchange_hint) {
    auto it = holdings_.find(instrument_id);
    if (it != holdings_.end()) return &it->second;
    std::string exchange_id = exchange_hint;
    if (exchange_id.empty()) {
        auto ex = instrument_exchange_map_.find(instrument_id);
        if (ex != instrument_exchange_map_.end()) exchange_id = ex->second.exchange_id;
    }
    auto ins = holdings_.emplace(instrument_id, PositionHolding(instrument_id, exchange_id)).first;
    return &ins->second;
}

bool AccountSession::is_spread_instrument(const std::string& instrument_id) const {
    auto it = instrument_exchange_map_.find(instrument_id);
    // 合约表未命中 (尚未加载 / 柜台没返回该合约): 按普通合约处理, 不猜 —— 漏判比误判成
    // 组合合约安全 (误判会让一笔真实单腿持仓静默消失).
    return it != instrument_exchange_map_.end() && it->second.product_class == DZ_PRODUCT_SPREAD;
}

void AccountSession::note_live_report_time(int64_t report_epoch_s) {
    if (report_epoch_s > last_live_report_epoch_s_) {
        last_live_report_epoch_s_ = report_epoch_s;
    }
}

void AccountSession::check_replay_external_order(const OrderRecord& rpt) {
    if (!replay_external_only_alarm(replaying_, rpt.insert_time, last_live_report_epoch_s_)) {
        return;
    }
    ++replay_external_missed_count_;
    // 逐笔 ERROR 留证 (假设失效时重放会成批命中, 需要能追到具体委托); 通知按最小间隔限频,
    // 避免一次重放风暴把前端通知环刷爆.
    SPDLOG_ERROR(
        "td external order only in login replay | account={} instrument={} order_ref={} "
        "insert_time={} last_live_time={} missed_count={}",
        account_id_, rpt.base.instrument_id, rpt.order_ref, rpt.insert_time,
        last_live_report_epoch_s_, replay_external_missed_count_);
    const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    if (!alert_report_due(now_s, replay_external_last_alert_s_, kReplayExternalAlertIntervalS)) {
        return;
    }
    replay_external_last_alert_s_ = now_s;
    if (!notify_) {
        return;
    }
    try {
        notify_(DZ_NOTIFY_ERROR,
                std::format("外部单仅出现在登录重放中（实时流未推送，累计 {} 笔）：私有流可能不是"
                            "投资者级或存在推送缺口，持仓偏差将持续到下次登录",
                            replay_external_missed_count_),
                /*popup=*/false);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td replay external alert notify failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void AccountSession::fill_position_info(DzPositionInfo& pos,
                                        const PositionHolding& h,
                                        DzDirection dir) {
    const PositionSide& s = h.side(dir);
    copy_string(pos.instrument_id, h.instrument_id().c_str(), true);
    copy_string(pos.exchange_id, h.exchange_id().c_str(), true);
    copy_string(pos.account_id, account_id_.c_str(), true);
    pos.direction = dir;
    pos.volume = s.volume();
    pos.today_volume = s.today;
    pos.yd_volume = s.yd;
    pos.frozen_volume = s.frozen();
    pos.price = s.price;
    pos.date = trading_day_;
    pos.seq = s.seq;
}

void AccountSession::push_position(PositionHolding& h, DzDirection dir, bool persist) {
    uint64_t seq = ++seq_counter_;
    h.set_seq(dir, seq);
    DzPositionInfo pos{};
    fill_position_info(pos, h, dir);
    platform::write_struct(event_writer_, DZ_FRAME_POSITION_INFO, pos);
    if (persist) {
        persist_writer_.enqueue(PersistTask{.kind = PersistTask::Kind::Position,
                                            .data = std::vector<DzPositionInfo>{pos},
                                            .account_id = account_id_,
                                            .trading_day = trading_day_});
    }
    SPDLOG_DEBUG("td position push | account={} instrument={} dir={} volume={} frozen={} seq={}",
                 account_id_, h.instrument_id(), static_cast<int>(dir), pos.volume,
                 pos.frozen_volume, seq);
}

void AccountSession::apply_position_query() {
    bool any_change = false;
    bool any_gone = false;
    std::unordered_set<PositionQueryKey, PositionQueryKeyHash> group;
    for (const auto& [key, agg] : position_query_agg_) {
        PositionHolding* h = ensure_holding(key.instrument_id, agg.exchange_id);
        DzDirection dir = static_cast<DzDirection>(key.direction);
        // 均价 = PositionCost / (持仓量 × 合约乘数); 缺乘数时留 0 (防除零/错价).
        double price = (agg.volume > 0 && agg.volume_multiple > 0)
                           ? agg.cost / (static_cast<double>(agg.volume) * agg.volume_multiple)
                           : 0.0;
        auto ch = h->apply_query_side(dir, agg.volume, agg.yd, price);
        if (ch.long_changed) {
            push_position(*h, DZ_DIRECTION_LONG);
            any_change = true;
        }
        if (ch.short_changed) {
            push_position(*h, DZ_DIRECTION_SHORT);
            any_change = true;
        }
        group.insert(key);
    }
    for (auto& [inst, h] : holdings_) {
        for (DzDirection dir : {DZ_DIRECTION_LONG, DZ_DIRECTION_SHORT}) {
            PositionQueryKey key{inst, static_cast<int8_t>(dir)};
            if (group.contains(key)) continue;
            auto ch = h.apply_query_side(dir, 0, 0, 0.0);
            if ((dir == DZ_DIRECTION_LONG && ch.long_changed) ||
                (dir == DZ_DIRECTION_SHORT && ch.short_changed)) {
                push_position(h, dir, /*persist=*/false);  // 零帧只推不落库, 行由重灌 DELETE
                any_gone = true;
            }
        }
    }
    // 活动平仓挂单全量种入 (含外部单; rebuild 内部按合约过滤), 先于重灌组构建
    if (report_filter_) {
        auto orders = report_filter_->active_orders(trading_day_);
        std::vector<ActiveOrderUpdate> updates;
        updates.reserve(orders.size());
        for (const auto& o : orders) {
            updates.push_back(ActiveOrderUpdate{
                std::string(o.base.instrument_id), std::string(o.order_ref), o.base.direction,
                o.base.position_effect, o.base.status, o.base.volume, o.base.volume_traded});
        }
        for (auto& [inst, h] : holdings_) {
            auto ch = h.rebuild_active_orders(updates);
            if (ch.long_changed) push_position(h, DZ_DIRECTION_LONG);
            if (ch.short_changed) push_position(h, DZ_DIRECTION_SHORT);
        }
    }
    // 冻结对账: 活动平仓挂单全量种入后再比对 (首个查询/重连时本地冻结尚未种入,
    // 先比会误报); CTP 冻结总量与本地推导不符仅 WARN (不覆盖, CTP 无今昨拆分).
    for (const auto& [key, agg] : position_query_agg_) {
        auto it = holdings_.find(key.instrument_id);
        if (it == holdings_.end()) continue;
        int64_t local_frozen = it->second.side(static_cast<DzDirection>(key.direction)).frozen();
        if (agg.ctp_frozen != local_frozen) {
            SPDLOG_WARN("td position frozen mismatch | account={} instrument={} ctp={} local={}",
                        account_id_, key.instrument_id, agg.ctp_frozen, local_frozen);
        }
    }
    if (!position_baseline_ready_ || any_change || any_gone) {
        position_query_group_.clear();
        for (const auto& [key, agg] : position_query_agg_) {
            auto it = holdings_.find(key.instrument_id);
            if (it == holdings_.end()) continue;
            DzPositionInfo pos{};
            fill_position_info(pos, it->second, static_cast<DzDirection>(key.direction));
            position_query_group_.push_back(pos);
        }
        persist_writer_.enqueue(PersistTask{.kind = PersistTask::Kind::PositionRebuild,
                                            .data = std::move(position_query_group_),
                                            .account_id = account_id_,
                                            .trading_day = trading_day_});
        position_query_group_.clear();
    }
    position_baseline_ready_ = true;
}

// ============================================================================
// 内部辅助: 缓冲回报 (设计 §5.3)
// ============================================================================

void AccountSession::buffer_order_rpt(const OnRtnOrderField& f) {
    if (buffered_orders_.size() >= kMaxBuffered) {
        buffered_orders_.pop_front();
        SPDLOG_ERROR("td buffer overflow, drop oldest order | account={}", account_id_);
    }
    buffered_orders_.push_back(f);
}

void AccountSession::buffer_trade_rpt(const OnRtnTradeField& f) {
    if (buffered_trades_.size() >= kMaxBuffered) {
        buffered_trades_.pop_front();
        SPDLOG_ERROR("td buffer overflow, drop oldest trade | account={}", account_id_);
    }
    buffered_trades_.push_back(f);
}

void AccountSession::replay_buffered_reports() {
    if (buffered_orders_.empty() && buffered_trades_.empty()) return;
    SPDLOG_INFO("td replay buffered | account={} orders={} trades={}", account_id_,
                buffered_orders_.size(), buffered_trades_.size());
    // Task 6: 重放发生在状态机转 Ready 之前 (flush 屏障前), 状态仍是
    // LoadingInstruments, on_rtn_order/on_rtn_trade 会走缓冲分支把回报重新入缓冲.
    // 置 replaying_ 标志使缓冲分支放行, 重放完清除.
    replaying_ = true;
    while (!buffered_orders_.empty()) {
        on_rtn_order(buffered_orders_.front());
        buffered_orders_.pop_front();
    }
    while (!buffered_trades_.empty()) {
        on_rtn_trade(buffered_trades_.front());
        buffered_trades_.pop_front();
    }
    replaying_ = false;
}

// ============================================================================
// C5: 补齐缺失的 SPI 事件处理
// 设计原则: "宁肯乱码也不能崩溃", 所有 handler 异常不传播, 仅记日志
// ============================================================================

// === on_rsp_qry_order: 委托查询响应 (RESTART 崩溃恢复补登, 设计 §5.6) ===
// 仅记日志, 不修改 OrderRefMap (避免覆盖活跃订单). is_last 时记 INFO.
void AccountSession::on_rsp_qry_order(const OnRspQryOrderField& f) {
    try {
        if (f.order) {
            SPDLOG_DEBUG(
                "td qry order | account={} instrument={} order_ref={} status={} is_last={}",
                account_id_, f.order->InstrumentID, f.order->OrderRef,
                magic_enum::enum_name(STATUS_CTP2VT(f.order->OrderStatus)), f.is_last);
        }
        if (f.is_last) {
            SPDLOG_INFO("td qry order done | account={}", account_id_);
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_order failed | account={} error=\"{}\"", account_id_, e.what());
    }
}

// === on_rsp_qry_trading_account: 资金查询响应 ===
// Task 6 (spec §4.2 查询链路响应侧 2003 写端 + 登录收尾):
// to_dz_trading_account -> seq 分配 -> 推 DZ_FRAME_TRADING_ACCOUNT -> persist.
// is_last/失败驱动登录收尾状态机 (资金查询完成).
void AccountSession::apply_trading_account_row(const CThostFtdcTradingAccountField& row,
                                               TradingAccountWriteSource source) {
    // 契约 ADR 0045 决策 4 / ADR 0046：**资金行只在持仓查询成功之后写入**。
    // 两个后果都必须避免，所以守卫放在唯一写入点（而不是只在某个调用点）：
    // 1) 锚点日一致性——锚点日 = positions / trading_accounts 两张快照表的 MAX(date)；
    //    持仓查询失败时若资金行仍写当日，锚点推进到当日而 positions 当日无行 → 持仓
    //    REST 返回空数组 = 假空仓（ADR 0045 决策 3 判定最危险的形态）。
    // 2) 「资金行与持仓行同日」——锚点推进到当日时持仓行集已一致。
    // 运行期刷新同样受此守卫（ADR 0055 决策 4）：不因盘中事件把锚点推前。
    if (!position_query_ok_) {
        SPDLOG_WARN(
            "td trading account skipped, position not ready | account={}"
            " reason=position_query_failed",
            account_id_);
        return;
    }
    DzTradingAccount acct = to_dz_trading_account(row, account_id_, trading_day_);
    acct.seq = ++seq_counter_;
    platform::write_struct(event_writer_, DZ_FRAME_TRADING_ACCOUNT, acct);
    persist_writer_.enqueue(PersistTask{.kind = PersistTask::Kind::TradingAccount,
                                        .data = acct,
                                        .account_id = account_id_,
                                        .trading_day = trading_day_});
    if (source == TradingAccountWriteSource::kRuntime) {
        SPDLOG_INFO("td runtime fund refresh | account={} balance={} available={} seq={}",
                    account_id_, acct.balance, acct.available, acct.seq);
    } else {
        SPDLOG_INFO("td qry trading account | account={} balance={} available={} seq={}",
                    account_id_, acct.balance, acct.available, acct.seq);
    }
}

// === on_rsp_qry_trading_account: 资金查询响应（登录收尾 + 运行期刷新，ADR 0055） ===
void AccountSession::on_rsp_qry_trading_account(const OnRspQryTradingAccountField& f) {
    // 终检发现 1: 陈旧响应防护 — 旧会话迟到的资金查询响应会错误分配 seq (污染取号器)
    // 并重复触发收尾, 一律丢弃。
    if (query_gen_ != generation_) {
        return;
    }
    const bool runtime =
        runtime_account_request_id_ != 0 && f.request_id == runtime_account_request_id_;
    // 登录链：生产 id 非 0；测试/未发起场景 id=0 仍接受（与 #457 前一致）。
    const bool login = f.request_id == account_query_request_id_;
    if (!runtime && !login) {
        return;  // 迟到 / 重复 / 非本链响应
    }

    if (runtime) {
        // 运行期分支必须自带异常边界：写入（帧/persist）或通知抛出时若不收口，
        // 在途 lane 会永久悬挂（超时已撤、id 已清），该账户刷新停摆到重连。
        try {
            if (state_machine_.state() != TdState::Ready) {
                // 离开 Ready（登出 / 日切）后的迟到运行期响应：复位通道，不写表不推帧。
                cancel_runtime_query_timeout();
                if (query_lane_timer_id_ != 0) {
                    timer_queue_.cancel(query_lane_timer_id_);
                    query_lane_timer_id_ = 0;
                    query_lane_timer_deadline_ = QueryPacer::TimePoint{};
                }
                runtime_account_request_id_ = 0;
                query_lane_.reset();
                return;
            }
            cancel_runtime_query_timeout();
            runtime_account_request_id_ = 0;
            const bool failed = f.rsp_info && f.rsp_info->ErrorID != 0;
            if (failed) {
                SPDLOG_WARN("td runtime fund query failed | account={} error_id={} error=\"{}\"",
                            account_id_, f.rsp_info->ErrorID,
                            dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
                query_lane_.note_failure(query_now(), QueryKind::TradingAccount);
                handle_runtime_query_failure(QueryKind::TradingAccount);
                return;
            }
            if (f.trading_account) {
                apply_trading_account_row(*f.trading_account, TradingAccountWriteSource::kRuntime);
            }
            if (f.is_last) {
                query_lane_.note_response(query_now(), QueryKind::TradingAccount);
                // 发送后到达的事件（pending）在收口后补一笔（不丢）。
                schedule_query_lane_pump();
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("td on_rsp_qry_trading_account runtime failed | account={} error=\"{}\"",
                         account_id_, e.what());
            cancel_runtime_query_timeout();
            runtime_account_request_id_ = 0;
            query_lane_.note_failure(query_now(), QueryKind::TradingAccount);
            handle_runtime_query_failure(QueryKind::TradingAccount);
        } catch (...) {
            SPDLOG_ERROR(
                "td on_rsp_qry_trading_account runtime failed | account={} error=\"unknown\"",
                account_id_);
            cancel_runtime_query_timeout();
            runtime_account_request_id_ = 0;
            query_lane_.note_failure(query_now(), QueryKind::TradingAccount);
            handle_runtime_query_failure(QueryKind::TradingAccount);
        }
        return;
    }

    // 登录收尾链（行为与 ADR 0045/0046 一致，不因运行期刷新改变）。
    try {
        if (f.trading_account) {
            apply_trading_account_row(*f.trading_account, TradingAccountWriteSource::kLogin);
        } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            SPDLOG_ERROR("td qry trading account error | account={} error_id={} error=\"{}\"",
                         account_id_, f.rsp_info->ErrorID,
                         dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
        }
        if (f.is_last) {
            note_query_done();  // 本查询收口（成功或错误均由 is_last 收尾）
            account_query_request_id_ = 0;
            // 资金查询完成 -> 进入收尾序列 (replay -> flush -> ready), 由 drive_finalizer 推进.
            // 幂等防御: 超时/失败路径已推进过 kQueryAccount 时, 迟到的 is_last 不再重复收尾.
            if (finalizer_.phase() == Phase::kQueryAccount) {
                finalizer_.on_account_done();
                finalize_login();
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_trading_account failed | account={} error=\"{}\"", account_id_,
                     e.what());
    }
}

// === on_rsp_qry_investor_position: 持仓查询响应 ===
// 登录收尾阶段一 (spec §4.2 查询链路响应侧): 逐行累加 (同 key 多行合并), is_last 收口应用 —
// 聚合模型 diff 推帧 / 全平零帧 / 活动委托重灌 / PositionRebuild 全量重灌落库.
// 失败按有限重试处理 (ADR 0049 决策 3); 成功后才发起资金查询 (CTP 流控串行).
void AccountSession::on_rsp_qry_investor_position(const OnRspQryInvestorPositionField& f) {
    if (query_gen_ != generation_ || f.request_id != position_query_request_id_) return;
    // 本次登录已判失败 (重试耗尽): 迟到/重复响应一律忽略, 不再触发任何查询.
    if (finalizer_.position_failed()) return;
    try {
        // 只处理当前持仓 > 0 的行: CTP 会返回 Position=0 的「已全平」行 (其他字段可能非 0),
        // 它对今昨/总量没有贡献; 消失的合约由整组原子替换移除, 不落 0 量行.
        // 不再用 YdPosition 判行存在性——它是交易日开始时的静态昨仓, 不代表当前有持仓.
        const bool has_row = f.investor_position && f.investor_position->Position > 0;
        if (has_row) {
            const auto& p = *f.investor_position;
            // 套利 / 组合合约的持仓行跳过: 不入库、不推帧, 只记 DEBUG 说明「为什么持仓比柜台
            // 少一行」(ADR 0049 决策 6). 盘后自动组合是交易所行为, 客户未申请也会生成组合
            // 持仓; 若当普通合约聚合, 会凭空造出一条「净持仓→多头、均价 0」的持仓.
            if (is_spread_instrument(p.InstrumentID)) {
                SPDLOG_DEBUG("td position spread row skipped | account={} instrument={} volume={}",
                             account_id_, p.InstrumentID, p.Position);
            } else {
                // 均价换算需合约乘数 (PositionCost 为金额); 合约表未命中/缺失时留 0 -> 均价 0.
                double volume_multiple = 0.0;
                if (auto ex = instrument_exchange_map_.find(p.InstrumentID);
                    ex != instrument_exchange_map_.end()) {
                    volume_multiple = ex->second.volume_multiple;
                }
                const std::optional<DzPositionInfo> mapped =
                    to_dz_position(p, account_id_, trading_day_, volume_multiple);
                // 净持仓('1'): 合法 CTP 枚举, 但持仓记账只有多/空两侧 (查询聚合键 /
                // PositionHolding / positions 唯一键; Net 是回报/展示的兼容口径), 且持仓查询
                // 实际按多/空返回 (ADR 0049 决策 9) —— 跳过该行 + WARN, 不产生持仓记录.
                // **不得**走未知方向分支: 那会作废整轮快照 → 基准查询失败 → 账户登录不上,
                // 等于让一个实际不存在的输入把账户打死; 也**不得**归为多头 (凭空造持仓).
                if (!mapped.has_value()) {
                    SPDLOG_WARN(
                        "td position net direction row skipped | account={} instrument={} "
                        "volume={}",
                        account_id_, p.InstrumentID, p.Position);
                } else {
                    const DzPositionInfo& pos = *mapped;
                    // TodayPosition 越界 = 柜台坏数据: 映射层已按契约 td-data-sync §3 夹取并
                    // 派生非负 yd, 这里补 WARN 留证 (映射层保持纯函数, 不自己记日志). 判据直接
                    // 用映射结果与原始字段的差异, 不重复夹取边界; 带方向使同合约多空两行可区分.
                    if (pos.today_volume != p.TodayPosition) {
                        SPDLOG_WARN(
                            "td position today/yd anomalous, clamped | account={} instrument={} "
                            "position={} today_position={} direction={}",
                            account_id_, p.InstrumentID, p.Position, p.TodayPosition,
                            static_cast<int>(pos.direction));
                    }
                    // 未知方向不能当多头写进持仓 (那会把空头写成多头, 净敞口反向), 也不能只把
                    // 这一行丢掉——那会让本轮快照看起来「这个合约空了」，apply_position_query
                    // 会把它的已知持仓清零（幽灵空仓, ADR 0045 决策 3 判定的最危险形态）。
                    // 读不懂的一行等价于本轮快照不可信: 整轮不应用, 保留上次已知快照。
                    if (pos.direction == DZ_DIRECTION_UNKNOWN) {
                        UnknownEnumSink unknown;
                        unknown.add(kUnknownCategoryDirection, p.PosiDirection);
                        report_unknown_enums(unknown);
                        position_query_unknown_direction_ = true;
                    } else {
                        PositionQueryKey key{std::string(pos.instrument_id),
                                             static_cast<int8_t>(pos.direction)};
                        PositionQueryAgg& agg = position_query_agg_[key];
                        // 今昨以映射层的 TodayPosition 口径为准 (契约 td-data-sync §3): 逐行先
                        // 算 today/yd 再累加, YdPosition 是静态昨仓, 不参与当前昨仓
                        // (多行合并也不能只累加 Position).
                        agg.volume += pos.volume;
                        agg.yd += pos.yd_volume;
                        agg.cost += p.PositionCost;
                        agg.volume_multiple = volume_multiple;
                        agg.ctp_frozen += static_cast<int64_t>(p.LongFrozen) + p.ShortFrozen;
                        if (agg.exchange_id.empty()) agg.exchange_id = p.ExchangeID;
                    }
                }
            }
        } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            SPDLOG_ERROR("td qry position error | account={} error_id={} error=\"{}\"", account_id_,
                         f.rsp_info->ErrorID, dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
        }
        if (f.is_last) {
            position_query_in_flight_ = false;
            cancel_position_query_timeout_timer();
            note_query_done();
            // 本分支负责的失败判据: is_last 带 ErrorID, 或本轮出现读不懂的方向
            // (整轮快照不可信 → 等价「查询结果不可用」). 其余判据 (请求返回非 0 /
            // 超时) 在发起侧处理, 完整清单见 on_position_query_attempt_failed 注释.
            // **零行不是失败**: is_last 无错即成功, 空账户合法, 由 apply_position_query
            // 整组替换为当日空集. 注意这里不把「查不到」和「查失败」混为一谈.
            if ((f.rsp_info && f.rsp_info->ErrorID != 0) || position_query_unknown_direction_) {
                const std::string reason =
                    position_query_unknown_direction_
                        ? std::string("unknown position direction")
                        : std::format("is_last ErrorID={}", f.rsp_info ? f.rsp_info->ErrorID : -1);
                on_position_query_attempt_failed(reason);
                return;
            }
            position_query_ok_ = true;
            if (!position_rebuild_consumed_) {
                position_rebuild_consumed_ = true;
                apply_position_query();
            }
            if (finalizer_.phase() == Phase::kQueryPosition) {
                finalizer_.on_position_done();
                req_qry_trading_account();
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_investor_position failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_order_insert: 报单录入响应 (CTP 同步拒单, 设计 §11.1) ===
// rsp_info 有错误时, 查 OrderRefMap 找 order_id, 构建 REJECTED OrderRecord 推 SHM + 持久化
void AccountSession::on_rsp_order_insert(const OnRspOrderInsertField& f) {
    try {
        if (f.rsp_info && f.rsp_info->ErrorID == 0) {
            // 无错误 (罕见), 仅记 DEBUG
            if (f.input_order) {
                SPDLOG_DEBUG(
                    "td rsp order insert (no error) | account={} instrument={} order_ref={}",
                    account_id_, f.input_order->InstrumentID, f.input_order->OrderRef);
            }
            return;
        }

        // I8: rsp_info 缺失视为失败 (与 on_rsp_authenticate / on_rsp_user_login 一致)
        int error_id = f.rsp_info ? f.rsp_info->ErrorID : -1;
        std::string error_msg =
            f.rsp_info ? dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg) : "rsp_info is null";

        if (!f.input_order) {
            SPDLOG_ERROR(
                "td rsp order insert error, no input_order | account={} error_id={} error=\"{}\"",
                account_id_, error_id, error_msg);
            return;
        }

        // 查 OrderRefMap 找 order_id
        const DzOrderId* local = order_ref_map_.find_by_order_ref(f.input_order->OrderRef);
        if (local == nullptr) {
            SPDLOG_WARN(
                "td rsp order insert error, order_ref not found | account={} order_ref={} "
                "error_id={} error=\"{}\"",
                account_id_, f.input_order->OrderRef, error_id, error_msg);
            return;
        }

        // 内联构建 REJECTED OrderRecord
        OrderRecord rec{};
        rec.base.order_id = *local;
        // 本地单: 回填 strategy_id (策略 SDK 按 strategy_id 定向过滤回报)
        if (const std::string* sid = order_ref_map_.find_strategy(*local)) {
            copy_string(rec.base.strategy_id, sid->c_str(), true);
        }
        copy_string(rec.base.instrument_id, f.input_order->InstrumentID, true);
        copy_string(rec.base.account_id, account_id_.c_str(), true);
        copy_string(rec.base.exchange_id, f.input_order->ExchangeID, true);
        // 未知开平/价格类型/方向落成 UNKNOWN 并上报, 不兜成 Open/Limit/Sell.
        UnknownEnumSink unknown;
        map_input_order_enums(*f.input_order, rec, unknown);
        report_unknown_enums(unknown);
        rec.base.status = DZ_ORDER_REJECTED;
        rec.base.price = f.input_order->LimitPrice;
        rec.base.volume = f.input_order->VolumeTotalOriginal;
        rec.base.volume_traded = 0;
        rec.base.date = trading_day_;
        rec.base.time = 0;
        copy_string(rec.base.remark, error_msg.c_str(), true);

        copy_string(rec.order_ref, f.input_order->OrderRef, true);
        rec.is_external = 0;
        rec.volume_canceled = f.input_order->VolumeTotalOriginal;
        rec.insert_time = 0;
        rec.update_time = 0;
        rec.error_id = error_id;
        copy_string(rec.error_msg, error_msg.c_str(), true);
        if (trading_day_ > 0) {
            dztrader::Date d{trading_day_};
            auto* end = std::format_to_n(rec.trading_day, sizeof(rec.trading_day) - 1,
                                         "{:04d}{:02d}{:02d}", d.year(), d.month(), d.day())
                            .out;
            *end = '\0';
        }

        // 本地拒单路径 (同 reject_order): 不经过滤器 (本地事件必新), 分配 seq + 更新基准.
        rec.base.seq = ++seq_counter_;
        if (report_filter_) {
            report_filter_->accept_order(rec);
        }

        write_order_rpt(rec.base);
        persist_order(rec);

        SPDLOG_ERROR(
            "td rsp order insert rejected | account={} order_id={} order_ref={} error_id={} "
            "error=\"{}\"",
            account_id_, *local, f.input_order->OrderRef, error_id, error_msg);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_order_insert failed | account={} error=\"{}\"", account_id_,
                     e.what());
    }
}

// === on_rsp_order_action: 报单操作响应 (撤单同步拒绝) ===
// 撤单失败不影响订单状态 (CTP 会通过 OnRtnOrder 推 CANCELLED), 仅记 WARN
void AccountSession::on_rsp_order_action(const OnRspOrderActionField& f) {
    try {
        if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
            SPDLOG_WARN(
                "td rsp order action error | account={} error_id={} error=\"{}\" order_ref={}",
                account_id_, f.rsp_info->ErrorID, error_msg,
                f.input_order_action ? f.input_order_action->OrderRef : "");
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_order_action failed | account={} error=\"{}\"", account_id_,
                     e.what());
    }
}

// === on_err_rtn_order_insert: 报单录入错误回报 (交易所拒单, 设计 §11.1) ===
// 类似 on_rsp_order_insert, 但 f.input_order 可能 null
void AccountSession::on_err_rtn_order_insert(const OnErrRtnOrderInsertField& f) {
    try {
        if (!f.rsp_info || f.rsp_info->ErrorID == 0) {
            return;  // 无错误, 忽略
        }

        int error_id = f.rsp_info->ErrorID;
        std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);

        if (!f.input_order) {
            SPDLOG_ERROR(
                "td err rtn order insert, no input_order | account={} error_id={} error=\"{}\"",
                account_id_, error_id, error_msg);
            return;
        }

        const DzOrderId* local = order_ref_map_.find_by_order_ref(f.input_order->OrderRef);
        if (local == nullptr) {
            SPDLOG_WARN(
                "td err rtn order insert, order_ref not found | account={} order_ref={} "
                "error_id={} error=\"{}\"",
                account_id_, f.input_order->OrderRef, error_id, error_msg);
            return;
        }

        // 内联构建 REJECTED OrderRecord (与 on_rsp_order_insert 一致)
        OrderRecord rec{};
        rec.base.order_id = *local;
        // 本地单: 回填 strategy_id (策略 SDK 按 strategy_id 定向过滤回报)
        if (const std::string* sid = order_ref_map_.find_strategy(*local)) {
            copy_string(rec.base.strategy_id, sid->c_str(), true);
        }
        copy_string(rec.base.instrument_id, f.input_order->InstrumentID, true);
        copy_string(rec.base.account_id, account_id_.c_str(), true);
        copy_string(rec.base.exchange_id, f.input_order->ExchangeID, true);
        // 未知开平/价格类型/方向落成 UNKNOWN 并上报, 不兜成 Open/Limit/Sell.
        UnknownEnumSink unknown;
        map_input_order_enums(*f.input_order, rec, unknown);
        report_unknown_enums(unknown);
        rec.base.status = DZ_ORDER_REJECTED;
        rec.base.price = f.input_order->LimitPrice;
        rec.base.volume = f.input_order->VolumeTotalOriginal;
        rec.base.volume_traded = 0;
        rec.base.date = trading_day_;
        rec.base.time = 0;
        copy_string(rec.base.remark, error_msg.c_str(), true);

        copy_string(rec.order_ref, f.input_order->OrderRef, true);
        rec.is_external = 0;
        rec.volume_canceled = f.input_order->VolumeTotalOriginal;
        rec.insert_time = 0;
        rec.update_time = 0;
        rec.error_id = error_id;
        copy_string(rec.error_msg, error_msg.c_str(), true);
        if (trading_day_ > 0) {
            dztrader::Date d{trading_day_};
            auto* end = std::format_to_n(rec.trading_day, sizeof(rec.trading_day) - 1,
                                         "{:04d}{:02d}{:02d}", d.year(), d.month(), d.day())
                            .out;
            *end = '\0';
        }

        // 本地拒单路径 (同 reject_order): 不经过滤器 (本地事件必新), 分配 seq + 更新基准.
        rec.base.seq = ++seq_counter_;
        if (report_filter_) {
            report_filter_->accept_order(rec);
        }

        write_order_rpt(rec.base);
        persist_order(rec);

        SPDLOG_ERROR(
            "td err rtn order insert rejected | account={} order_id={} order_ref={} error_id={} "
            "error=\"{}\"",
            account_id_, *local, f.input_order->OrderRef, error_id, error_msg);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_err_rtn_order_insert failed | account={} error=\"{}\"", account_id_,
                     e.what());
    }
}

// === on_err_rtn_order_action: 报单操作错误回报 (撤单被拒) ===
// 同 on_rsp_order_action, 仅记 WARN (撤单被拒不影响订单状态)
void AccountSession::on_err_rtn_order_action(const OnErrRtnOrderActionField& f) {
    try {
        if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
            SPDLOG_WARN(
                "td err rtn order action | account={} error_id={} error=\"{}\" order_ref={}",
                account_id_, f.rsp_info->ErrorID, error_msg,
                f.order_action ? f.order_action->OrderRef : "");
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_err_rtn_order_action failed | account={} error=\"{}\"", account_id_,
                     e.what());
    }
}

// === on_rsp_transfer: 出入金响应 ===
// 转 platform::DzTransferRsp, 推 SHM DZ_FRAME_TD_TRANSFER_RSP (JSON, 契约 td-account-ops)
void AccountSession::on_rsp_transfer(const OnRspFromBankToFutureByFutureField& f) {
    try {
        if (!f.req_transfer) {
            SPDLOG_WARN("td rsp transfer, no req_transfer | account={}", account_id_);
            return;
        }

        platform::DzTransferRsp rec;
        rec.account_id = account_id_;
        rec.trade_code = f.req_transfer->TradeCode;
        rec.error_id = (f.rsp_info && f.rsp_info->ErrorID != 0) ? f.rsp_info->ErrorID : 0;
        if (f.rsp_info) {
            rec.error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
        }
        rec.bank_balance = 0;  // ReqTransferField 无银行余额字段
        rec.trade_amount = f.req_transfer->TradeAmount;
        rec.transfer_status = f.req_transfer->TransferStatus == '\0'
                                  ? std::string()
                                  : std::string(1, f.req_transfer->TransferStatus);
        rec.time = parse_ctp_time(f.req_transfer->TradeTime);

        platform::write_ext_json(event_writer_, DZ_FRAME_TD_TRANSFER_RSP, rec);
        // ADR 0055 决策 2：出入金响应置脏资金刷新（CTP 侧余额变化）。
        mark_fund_refresh_dirty();

        SPDLOG_INFO("td rsp transfer | account={} trade_code={} amount={} error_id={}", account_id_,
                    f.req_transfer->TradeCode, f.req_transfer->TradeAmount, rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_transfer failed | account={} error=\"{}\"", account_id_, e.what());
    }
}

// === on_rtn_transfer: 出入金实时通知 (银行权威结果) ===
// 转 platform::DzTransferRsp, 推 SHM DZ_FRAME_TD_TRANSFER_RTN (JSON, 契约 td-account-ops)
void AccountSession::on_rtn_transfer(const OnRtnFromBankToFutureByFutureField& f) {
    try {
        platform::DzTransferRsp rec;
        rec.account_id = account_id_;
        rec.trade_code = f.rsp_transfer.TradeCode;
        rec.error_id = f.rsp_transfer.ErrorID;
        rec.error_msg = dztrader::to_utf8_from_gbk(f.rsp_transfer.ErrorMsg);
        rec.bank_balance = 0;  // RspTransferField 无明确银行余额字段
        rec.trade_amount = f.rsp_transfer.TradeAmount;
        rec.transfer_status = f.rsp_transfer.TransferStatus == '\0'
                                  ? std::string()
                                  : std::string(1, f.rsp_transfer.TransferStatus);
        rec.time = parse_ctp_time(f.rsp_transfer.TradeTime);

        platform::write_ext_json(event_writer_, DZ_FRAME_TD_TRANSFER_RTN, rec);
        // ADR 0055 决策 2：出入金实时通知（银行权威）置脏资金刷新。
        mark_fund_refresh_dirty();

        SPDLOG_INFO("td rtn transfer | account={} trade_code={} amount={} error_id={}", account_id_,
                    f.rsp_transfer.TradeCode, f.rsp_transfer.TradeAmount, rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rtn_transfer failed | account={} error=\"{}\"", account_id_, e.what());
    }
}

// === on_rsp_user_password_update: 修改登录密码响应 ===
// 转 platform::DzPasswordUpdateRsp (password_type="U"), 推 SHM (JSON, 契约 td-account-ops)
void AccountSession::on_rsp_user_password_update(const OnRspUserPasswordUpdateField& f) {
    try {
        platform::DzPasswordUpdateRsp rec;
        rec.account_id = account_id_;
        rec.password_type = "U";
        rec.error_id = (f.rsp_info && f.rsp_info->ErrorID != 0) ? f.rsp_info->ErrorID : 0;
        if (f.rsp_info) {
            rec.error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
        }
        rec.time = 0;  // CTP 无时间字段, 留 0

        platform::write_ext_json(event_writer_, DZ_FRAME_TD_PASSWORD_UPDATE_RSP, rec);

        SPDLOG_INFO("td rsp user password update | account={} error_id={}", account_id_,
                    rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_user_password_update failed | account={} error=\"{}\"", account_id_,
                     e.what());
    }
}

// === on_rsp_trading_account_password_update: 修改资金密码响应 ===
// 转 platform::DzPasswordUpdateRsp (password_type="A"), 推 SHM (JSON, 契约 td-account-ops)
void AccountSession::on_rsp_trading_account_password_update(
    const OnRspTradingAccountPasswordUpdateField& f) {
    try {
        platform::DzPasswordUpdateRsp rec;
        rec.account_id = account_id_;
        rec.password_type = "A";
        rec.error_id = (f.rsp_info && f.rsp_info->ErrorID != 0) ? f.rsp_info->ErrorID : 0;
        if (f.rsp_info) {
            rec.error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
        }
        rec.time = 0;  // CTP 无时间字段, 留 0

        platform::write_ext_json(event_writer_, DZ_FRAME_TD_PASSWORD_UPDATE_RSP, rec);

        SPDLOG_INFO("td rsp trading account password update | account={} error_id={}", account_id_,
                    rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_trading_account_password_update failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_error: 通用错误响应 (ADR 0055 / #457) ===
// CTP 对查询流控拒绝等场景经 OnRspError 返回（可能没有对应的 OnRspQry* 应答）。
// 按在途查询的 nRequestID 路由：结束该查询并走既有失败语义，避免悬挂到超时。
void AccountSession::on_rsp_error(const OnRspErrorTdField& f) {
    if (query_gen_ != generation_) {
        return;  // 旧会话/旧代际的迟到错误
    }
    const int error_id = f.rsp_info ? f.rsp_info->ErrorID : -1;
    const std::string error_msg = f.rsp_info ? dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg)
                                             : std::string("rsp_info is null");
    switch (route_query_error(f.request_id, instrument_query_request_id_,
                              position_query_request_id_, account_query_request_id_,
                              runtime_account_request_id_)) {
        case QueryErrorTarget::Instrument:
            if (state_machine_.state() != TdState::LoadingInstruments ||
                instrument_query_request_id_ == 0) {
                SPDLOG_WARN(
                    "td stale instrument query error ignored | account={} request_id={} state={}",
                    account_id_, f.request_id, static_cast<int>(state_machine_.state()));
                return;
            }
            SPDLOG_ERROR("td qry instrument error response | account={} error_id={} error=\"{}\"",
                         account_id_, error_id, error_msg);
            note_query_done();
            instrument_query_request_id_ = 0;
            // #488: on_instruments_load_failed 的通知从 notify_instruments_load_failed 统一上浮;
            // 原因用柜台原文 (中文弹窗)。
            // #496: instruments_rejected 的第二种来源 = OnRspError 错误响应 (契约 td-status §2.5)。
            notify_instruments_load_failed(
                TdFailureCode::InstrumentsRejected,
                std::format("柜台返回错误，错误码 {}：{}", error_id, error_msg), error_id);
            return;
        case QueryErrorTarget::Position:
            if (!position_query_in_flight_ || finalizer_.phase() != Phase::kQueryPosition) {
                SPDLOG_WARN("td stale position query error ignored | account={} request_id={}",
                            account_id_, f.request_id);
                return;
            }
            SPDLOG_ERROR("td qry position error response | account={} error_id={} error=\"{}\"",
                         account_id_, error_id, error_msg);
            on_position_query_attempt_failed(std::format("OnRspError error_id={}", error_id));
            return;
        case QueryErrorTarget::TradingAccount:
            if (runtime_account_request_id_ != 0 && f.request_id == runtime_account_request_id_) {
                SPDLOG_WARN(
                    "td runtime fund query error response | account={} error_id={} error=\"{}\"",
                    account_id_, error_id, error_msg);
                cancel_runtime_query_timeout();
                runtime_account_request_id_ = 0;
                query_lane_.note_failure(query_now(), QueryKind::TradingAccount);
                handle_runtime_query_failure(QueryKind::TradingAccount);
                return;
            }
            if (finalizer_.phase() != Phase::kQueryAccount || account_query_request_id_ == 0) {
                SPDLOG_WARN("td stale account query error ignored | account={} request_id={}",
                            account_id_, f.request_id);
                return;
            }
            SPDLOG_ERROR("td qry account error response | account={} error_id={} error=\"{}\"",
                         account_id_, error_id, error_msg);
            note_query_done();
            account_query_request_id_ = 0;
            finalizer_.on_account_failed();
            finalize_login();
            return;
        case QueryErrorTarget::None:
            // 非查询类请求的错误（下单/出入金等）沿用「仅日志」语义；Session 级 WARN 留证。
            SPDLOG_WARN(
                "td rsp error unmatched | account={} error_id={} request_id={} error=\"{}\"",
                account_id_, error_id, f.request_id, error_msg);
            return;
    }
}

}  // namespace dztrader::ctp
