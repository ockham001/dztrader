#include "td/td_account_session.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <unordered_set>
#include <utility>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/encoding.h>
#include <dztrader/core/path.h>
#include <dztrader/core/string_util.h>
#include <dztrader/data_type.h>
#include <dztrader/date_time/date.h>
#include <dztrader/db/database_sqlite.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/risk_reject.h>
#include <dztrader/platform/td_account_ops.h>
#include <dztrader/struct.h>
#include <dztrader/tdstore/instrument_store.h>
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

}  // namespace

AccountSession::AccountSession(std::string account_id,
                               shm::OrderIdMeta& order_id_meta,
                               shm::MultiWriter& event_writer,
                               PersistWriter& persist_writer,
                               const MpmcQueuePtr& event_queue,
                               dztrader::core::TimerQueue& timer_queue,
                               std::function<void(DzAccountState)> account_state_cb,
                               SessionBootData boot)
    : account_id_(std::move(account_id)),
      order_id_meta_(order_id_meta),
      event_writer_(event_writer),
      persist_writer_(persist_writer),
      event_queue_(event_queue),
      timer_queue_(timer_queue),
      account_state_cb_(std::move(account_state_cb)),
      risk_gate_(false) {
    if (!event_queue_) {
        throw std::runtime_error("AccountSession: event_queue is null");
    }
    init_from_boot(std::move(boot));
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

void AccountSession::init_from_boot(const SessionBootData& boot) {
    // Task 5 §2.2 启动恢复: 内存 seq = 该账户 DB 已提交最大 seq + 1.
    // 累积不归零 (spec §2.1): MAX 对垃圾行只会抬高计数, 天然安全收敛.
    seq_counter_ = boot.start_seq;
    // 重放过滤器基准 = 该账户全部订单最新态 + 全部 trade_id 集合 (spec §4.1).
    report_filter_ = std::make_unique<ReportFilter>(account_id_);
    *report_filter_ = ReportFilter::load(boot.orders, boot.trades);
    max_seq_at_disconnect_ = boot.start_seq;
}

void AccountSession::reload_incremental(SQLite::Database& db) {
    // spec §4.3: 账户断开重连 (含重登) 时, 从断开时刻之后提交的 DB 行重新装载基准.
    // 增量: seq > max_seq_at_disconnect_ 的 orders/trades, 合并进现有基准 (防重登重放全转发).
    // 失败降级 (保留现有基准): 不阻塞重连, 重放风暴由过滤器吞同兜底.
    try {
        auto orders = load_orders_since(db, account_id_, max_seq_at_disconnect_);
        auto trades = load_trades_since(db, account_id_, max_seq_at_disconnect_);
        for (const auto& o : orders) {
            report_filter_->accept_order(o);
        }
        for (const auto& t : trades) {
            report_filter_->accept_trade(t);
        }
        if (!orders.empty() || !trades.empty()) {
            SPDLOG_INFO("td incremental baseline reloaded | account={} orders={} trades={}",
                        account_id_, orders.size(), trades.size());
        }
        // 水位推进到本次装载的最大 seq (含断连时水位): 防下次重连重复装载.
        max_seq_at_disconnect_ = std::max(max_seq_at_disconnect_,
                                          query_max_seq(db, account_id_));
        seq_counter_ = std::max(seq_counter_, max_seq_at_disconnect_);
    } catch (const std::exception& e) {
        SPDLOG_WARN("td incremental baseline reload failed, keep existing | account={} error=\"{}\"",
                    account_id_, e.what());
    }
}

// ============================================================================
// 生命周期
// ============================================================================

void AccountSession::open(const std::string& flow_dir,
                          const std::vector<std::string>& front_addrs,
                          const std::string& broker_id,
                          const std::string& user_id,
                          const std::string& password,
                          const std::string& auth_code,
                          const std::string& app_id) {
    if (api_ != nullptr) {
        SPDLOG_WARN("td session already opened | account={}", account_id_);
        return;
    }
    broker_id_ = broker_id;
    user_id_ = user_id;
    password_ = password;
    auth_code_ = auth_code;
    app_id_ = app_id;
    front_addrs_ = front_addrs;
    flow_dir_ = flow_dir;

    api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(flow_dir.c_str());
    if (api_ == nullptr) {
        throw std::runtime_error("CreateFtdcTraderApi returned null");
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
    connect_timer_id_ = timer_queue_.schedule_after(
        std::chrono::seconds(30),
        [this, gen]() {
            if (gen != generation_) return;  // 陈旧回调
            if (state_machine_.state() == TdState::Connecting) {
                SPDLOG_ERROR("td connect timeout | account={}", account_id_);
                state_machine_.on_connect_timeout();
                // 契约 account-status: 连接超时回 Offline (本 lambda 是唯一不经
                // TdApi dispatch 路径的直调点, 经状态通知回调走统一写出口)
                if (account_state_cb_) {
                    account_state_cb_(DZ_ACCOUNT_OFFLINE);
                }
            }
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
    cancel_instruments_load_timer();
    if (position_poll_timer_id_ != 0) {
        timer_queue_.cancel(position_poll_timer_id_);
        position_poll_timer_id_ = 0;
    }
    ++generation_;  // 使已挂起定时器回调失效
    api_->RegisterSpi(nullptr);
    api_->Release();
    api_ = nullptr;
    spi_.reset();
    state_machine_.on_disconnect();
    // Task 5 §4.3: 记录断开时刻水位, 供重连时增量装载基准 (seq > 该值的行).
    max_seq_at_disconnect_ = seq_counter_;
    // 断线持仓基准失效: 重连后重新查询重建; 在途查询响应变陈旧 (代际已失效).
    position_baseline_ready_ = false;
    position_query_in_flight_ = false;
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
    // Task 5 §4.3: 前置重连 (含重登) 时增量重建过滤器基准 — 装载断开时刻之后提交的
    // DB 行 (seq > max_seq_at_disconnect_), 合并进现有基准, 防重登重放全转发.
    // 库不可用 (provider 空 / 查询失败) 时降级保留现有基准, 由过滤器吞同兜底.
    if (prescan_db_provider_) {
        if (SQLite::Database* db = prescan_db_provider_(); db != nullptr) {
            reload_incremental(*db);
        }
    }
    auto notif = state_machine_.on_front_connected();
    SPDLOG_INFO("td front connected | account={} state={}",
                account_id_, magic_enum::enum_name(state_machine_.state()));
    if (!auth_code_.empty()) {
        req_authenticate();
    } else {
        req_login();
    }
}

void AccountSession::on_front_disconnected(int reason) {
    state_machine_.on_front_disconnected(reason);
    cancel_connect_timer();
    cancel_login_timer();
    cancel_instruments_load_timer();
    if (position_poll_timer_id_ != 0) {
        timer_queue_.cancel(position_poll_timer_id_);
        position_poll_timer_id_ = 0;
    }
    ++generation_;  // 使已挂起定时器回调失效
    // 终检发现 1: 作废在途登录收尾查询链 (CTP 断连后在途查询响应作废)。
    // 否则首次登录已收尾 (finalizer 停在 kDone), 重连重登的持仓 is_last 被
    // phase()==kQueryPosition 门挡住不推进不发资金查询, 5min 兜底同门被挡,
    // resync 被 !is_ready() 挡 → 账户永久停在 LoadingInstruments。
    finalizer_ = LoginFinalizer{};  // 复位收尾状态机 (kQueryPosition)
    position_query_group_.clear();
    position_rebuild_consumed_ = false;
    position_query_ok_ = false;
    account_query_ok_ = false;
    data_query_ok_ = false;  // 新链重新判定 (失败由重连收尾/补查兜底)
    position_baseline_ready_ = false;
    position_query_in_flight_ = false;
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
    cancel_login_timer();
    bool ok = f.rsp_info && f.rsp_info->ErrorID == 0;
    if (!ok) {
        SPDLOG_ERROR("td authenticate failed | account={} error_id={}",
                     account_id_, f.rsp_info ? f.rsp_info->ErrorID : -1);
        state_machine_.on_authenticate_failed();
        return;
    }
    state_machine_.on_authenticate_success();
    SPDLOG_INFO("td authenticated | account={}", account_id_);
    req_login();
}

void AccountSession::on_rsp_user_login(const OnRspTdUserLoginField& f) {
    cancel_login_timer();
    if (!f.rsp_info || f.rsp_info->ErrorID != 0) {
        SPDLOG_ERROR("td login failed | account={} error_id={}",
                     account_id_, f.rsp_info ? f.rsp_info->ErrorID : -1);
        state_machine_.on_login_failed();
        return;
    }
    // 同步 order_ref (设计 §9.4): new = max(local+1, ctp_max+1)
    if (f.rsp_user_login) {
        int64_t ctp_max = parse_max_order_ref(f.rsp_user_login->MaxOrderRef);
        order_ref_ = sync_order_ref(order_ref_, ctp_max);
    }
    // I4: 同步 trading_day, 解析失败 (INT32_MIN) 时记 WARN 但不中断登录.
    // 发现 3 (评审 Important): 走 set_trading_day 而非直接赋值 — 日切/重连后新交易日的
    // 持仓为绝对态, 旧镜像 (key 不含日期) 会拦截新日首报, 必须在登录时清空 (spec §4.1 跨日清空).
    if (f.days_since_epoch != std::numeric_limits<int32_t>::min()) {
        set_trading_day(f.days_since_epoch);
    } else {
        SPDLOG_WARN("td trading_day parse failed, keep previous | account={} trading_day={}",
                    account_id_, trading_day_);
    }
    // I1: sys_version 改用 CTP API 版本 (CZCETime 是郑商所时间, 不是系统版本)
    const std::string& sys_version = api_version_;
    std::string trading_day_str = f.rsp_user_login ? f.rsp_user_login->TradingDay : "";
    std::string login_time = f.rsp_user_login ? f.rsp_user_login->LoginTime : "";
    state_machine_.on_login_success(sys_version, trading_day_str, login_time);
    SPDLOG_INFO("td login success | account={} order_ref={} trading_day={}",
                account_id_, order_ref_, trading_day_str);
    req_settlement_confirm();
}

void AccountSession::on_rsp_settlement_confirm(const OnRspSettlementInfoConfirmField& f) {
    // I8: 统一 rsp_info 处理: 缺失视为失败 (与 on_rsp_authenticate / on_rsp_user_login 一致)
    if (!f.rsp_info || f.rsp_info->ErrorID != 0) {
        std::string err = f.rsp_info
            ? dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg)
            : "rsp_info is null";
        SPDLOG_ERROR("td settlement confirm failed | account={} error_id={} error=\"{}\"",
                     account_id_, f.rsp_info ? f.rsp_info->ErrorID : -1, err);
        // C6: 回退到 LoggedIn, 等待下个调度点重试 (不卡死在 Confirming)
        state_machine_.on_settlement_confirm_failed(err);
        return;
    }
    state_machine_.on_settlement_confirmed();
    SPDLOG_INFO("td settlement confirmed | account={}", account_id_);
    // 登录收尾链起点重置费率查询广播模式为 false (只入库不广播, 全量费率洪泛防护).
    fee_rate_broadcast_ = false;
    fee_query_pending_commission_ = false;
    fee_query_instrument_.clear();
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
    // 先处理数据, 再判 is_last (避免 null instrument + is_last 时状态机卡死)
    if (f.instrument) {
        // C2: 存储 instrument_id -> exchange_id/price_tick, 供 place_order 查表与 Task 7 用
        instrument_exchange_map_[f.instrument->InstrumentID] =
            InstrumentBrief{f.instrument->ExchangeID, f.instrument->PriceTick,
                            static_cast<double>(f.instrument->VolumeMultiple)};
        try {
            // 持久化合约信息 (供审计/复盘/策略查询)
            // update_day[9]: "YYYYMMDD" 文本 (从 DzDate 距纪元天数转换)
            std::string update_day_str = "00000000";
            if (trading_day_ > 0) {
                dztrader::Date d{trading_day_};
                char update_day[9];
                auto* end = std::format_to_n(update_day, sizeof(update_day) - 1,
                                             "{:04d}{:02d}{:02d}",
                                             d.year(), d.month(), d.day()).out;
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
        SPDLOG_DEBUG("td instrument | account={} instrument={}",
                     account_id_, f.instrument->InstrumentID);
    } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
        // CTP 查询失败 (流控超时/权限错误等): pInstrument=null, pRspInfo={ErrorID!=0}
        SPDLOG_ERROR("td qry instrument error | account={} error_id={} error_msg=\"{}\"",
                     account_id_, f.rsp_info->ErrorID,
                     dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
    }
    if (f.is_last) {
        // 定向刷新 (Ready 后) 的 is_last 不驱动登录收尾链
        if (state_machine_.state() != TdState::LoadingInstruments) {
            SPDLOG_INFO("td instrument refresh done | account={} instrument={}",
                        account_id_, f.instrument ? f.instrument->InstrumentID : "");
            return;
        }
        // Task 6 (spec §4.2 登录完成协议): 合约加载完成**不立即**转 Ready.
        // 留在 LoadingInstruments (缓冲分支继续生效, CTP 私有流重放期间回报继续进缓冲),
        // 进入登录收尾查询阶段: 发起持仓查询 (CTP 流控 1 次/秒, 串行 -> 资金).
        bool failed = (f.rsp_info && f.rsp_info->ErrorID != 0) || !f.instrument;
        if (failed) {
            // 合约加载失败: 回退到 LoggedIn, 不进入 Ready (设计 §2.4.1).
            std::string err = f.rsp_info
                ? dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg)
                : "instrument is null on is_last";
            cancel_instruments_load_timer();
            state_machine_.on_instruments_load_failed(err);
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
        account_query_ok_ = false;
        position_rebuild_consumed_ = false;
        position_query_group_.clear();
        req_qry_investor_position();
    }
}

void AccountSession::on_rtn_order(const OnRtnOrderField& f) {
    // LoadingInstruments 期间缓冲, Ready 后重放 (设计 §5.3).
    // Task 6: 缓冲重放阶段 (replaying_) 状态仍是 LoadingInstruments, 须放行,
    // 否则重放回报会被重新入缓冲 (无限循环).
    if (state_machine_.state() == TdState::LoadingInstruments && !replaying_) {
        buffer_order_rpt(f);
        return;
    }

    try {
        // CTP OrderField -> OrderRecord (含 DzOrderReport base + CTP 扩展字段)
        OrderRecord rpt = to_order_record(f.order, account_id_, trading_day_);

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
            SPDLOG_WARN("td order outdated dropped | account={} order_id={}",
                        account_id_, rpt.base.order_id);
            return;
        }

        // ③ 转发: 分配 seq -> 更新基准 -> 推帧 -> 落库 (同一事件 shm 帧与 DB 行同 seq).
        rpt.base.seq = ++seq_counter_;
        report_filter_->accept_order(rpt);
        write_order_rpt(rpt.base);
        persist_order(rpt);

        // 活动平仓挂单 → 冻结重算 (kReplay 阶段同样应用; 基准未就绪时跳过)
        if (position_baseline_ready_) {
            if (auto* h = ensure_holding(rpt.base.instrument_id, std::string(rpt.base.exchange_id));
                h != nullptr) {
                ActiveOrderUpdate upd{std::string(rpt.base.instrument_id),
                                      std::string(rpt.order_ref), rpt.base.direction,
                                      rpt.base.position_effect, rpt.base.status,
                                      rpt.base.volume, rpt.base.volume_traded};
                auto ch = h->apply_order(upd);
                if (ch.long_changed) push_position(*h, DZ_DIRECTION_LONG);
                if (ch.short_changed) push_position(*h, DZ_DIRECTION_SHORT);
            }
        }

        SPDLOG_DEBUG("td rtn order | account={} order_id={} order_ref={} status={} traded={} seq={}",
                     account_id_, rpt.base.order_id, f.order.OrderRef,
                     magic_enum::enum_name(rpt.base.status), rpt.base.volume_traded,
                     rpt.base.seq);
    } catch (const std::exception& e) {
        // "宁肯乱码也不能崩溃": 不传播异常到 TdApi 主循环
        SPDLOG_ERROR("td rtn order process failed | account={} error=\"{}\" instrument={} order_ref={}",
                     account_id_, e.what(), f.order.InstrumentID, f.order.OrderRef);
    }
}

void AccountSession::on_rtn_trade(const OnRtnTradeField& f) {
    // Task 6: 重放阶段放行 (同 on_rtn_order, 防止重放回报被重新入缓冲).
    if (state_machine_.state() == TdState::LoadingInstruments && !replaying_) {
        buffer_trade_rpt(f);
        return;
    }

    try {
        // CTP TradeField -> TradeRecord (含 DzTradeReport base + 扩展字段)
        TradeRecord rpt = to_trade_record(f.trade, account_id_, trading_day_);

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
        if (report_filter_ &&
            report_filter_->check_trade(rpt) == ReportFilter::Verdict::kSkip) {
            return;
        }

        // 转发: 分配 seq -> 更新基准 -> 推帧 -> 落库 (与委托同取号器, 全类型共享).
        rpt.base.seq = ++seq_counter_;
        report_filter_->accept_trade(rpt);
        write_trade_rpt(rpt.base);
        persist_trade(rpt);

        // 盘中持仓增量 (基准就绪且非缓冲重放; 重放成交已含在查询快照中)
        if (position_baseline_ready_ && !replaying_) {
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
        SPDLOG_ERROR("td rtn trade process failed | account={} error=\"{}\" instrument={} trade_id={}",
                     account_id_, e.what(), f.trade.InstrumentID, f.trade.TradeID);
    }
}

// ============================================================================
// 业务接口
// ============================================================================

void AccountSession::place_order(const DzOrderReq& req) {
    if (!is_ready()) {
        SPDLOG_WARN("td place_order rejected, not ready | account={} state={}",
                    account_id_, magic_enum::enum_name(state_machine_.state()));
        reject_order(req, std::format("交易未就绪: {}",
                                       magic_enum::enum_name(state_machine_.state())));
        return;
    }
    if (req.position_effect == DZ_POSITION_EFFECT_AUTO) {
        SPDLOG_WARN("td place_order rejected: auto offset unimplemented | account={} order_id={} instrument={}",
                    account_id_, req.order_id, req.instrument_id);
        reject_order(req, "AUTO 开平未实现");
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
    build_ctx.account_id = user_id_;  // CTP InvestorID
    build_ctx.order_ref = order_ref_;
    build_ctx.request_id = ++request_id_;
    build_ctx.exchange_id = it->second.exchange_id;

    CThostFtdcInputOrderField input = to_input_order_field(req, build_ctx);
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
        SPDLOG_ERROR("td cancel_order failed: api is null | account={} order_id={}",
                     account_id_, order_id);
        return false;
    }

    CThostFtdcInputOrderActionField action{};
    copy_string(action.BrokerID, broker_id_.c_str(), true);
    copy_string(action.InvestorID, user_id_.c_str(), true);
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
    trading_day_ = trading_day;
    for (auto& [inst, h] : holdings_) {
        auto ch = h.on_day_switch();
        if (ch.long_changed) push_position(h, DZ_DIRECTION_LONG);
        if (ch.short_changed) push_position(h, DZ_DIRECTION_SHORT);
    }
    SPDLOG_INFO("td trading day updated | account={} trading_day={}", account_id_, trading_day);
}

void AccountSession::resync_account_data() {
    // Task 6 (spec §4.2 查询失败降级补查): 登录时持仓/资金查询失败已转 Ready,
    // 由 td_api_scheduled 定时任务 60s 间隔调用直至成功.
    // 仅 Ready 且此前查询未成功时生效 (成功一次后不再重复补查).
    if (!is_ready() || data_query_ok_) {
        return;
    }
    if (position_query_in_flight_) return;  // 与周期重查互斥, 避免双查询链
    // 上一轮查询链仍在进行 (phase 停在查询阶段, 如长时间流控重试) 时不重复发起,
    // 避免双查询链并发. 仅上一轮已收尾 (kDone) 才重开新一轮.
    if (finalizer_.phase() != Phase::kDone) {
        return;
    }
    // 回到查询阶段重新发起 (状态机从 kQueryPosition 重走, 双查询完成即重入收尾).
    // 清空本轮成功标志, 由新查询响应重新判定 (避免残留旧值误判完整).
    position_query_ok_ = false;
    account_query_ok_ = false;
    finalizer_ = LoginFinalizer{};
    req_qry_investor_position();
}

void AccountSession::set_position_poll_interval(int seconds) {
    position_poll_interval_s_ = seconds > 0 ? seconds : 60;
    if (is_ready()) schedule_position_poll();
}

void AccountSession::schedule_position_poll() {
    // tag 带账户维度: TdApi 的 TimerQueue 为进程级共享 (多账户同进程),
    // 固定 tag 会互相 replace 导致其余账户轮询停摆.
    position_poll_timer_id_ = timer_queue_.schedule_after_replace(
        std::format("td_position_poll:{}", account_id_),
        std::chrono::seconds(position_poll_interval_s_),
        [this, gen = generation_]() {
            if (gen != generation_) return;  // 断连/登出后回调作废
            on_position_poll_timer();
        });
}

void AccountSession::on_position_poll_timer() {
    position_poll_timer_id_ = 0;
    if (!is_ready()) return;
    // data_query_ok_ false 时由既有 60s resync 兜底 (登录失败补查), 此处不并发
    if (position_baseline_ready_ && data_query_ok_ && !position_query_in_flight_) {
        req_qry_investor_position(false);
    }
    schedule_position_poll();
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
            SPDLOG_DEBUG("td repush last order | account={} order_id={} seq={}",
                         account_id_, latest_order->order_id, latest_order->seq);
        } else if (latest_trade != nullptr) {
            platform::write_struct(event_writer_, DZ_FRAME_TRADE_REPORT, *latest_trade);
            SPDLOG_DEBUG("td repush last trade | account={} trade_id={} seq={}",
                         account_id_, latest_trade->trade_id, latest_trade->seq);
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td repush last records failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void AccountSession::delete_event(Event& event) noexcept {
    if (event.data == nullptr) return;
    // td 关心的事件走 td_delete_event_data:
    //   - td 类型 (>=100)
    //   - OnFrontConnected / OnFrontDisconnected (td 进程用 td 版本 Field, 不可走 md 路径)
    // 其他 md 类型 (3, 4-13) 走 Event::delete_data (md 内联)
    if (static_cast<int16_t>(event.type) >= 100 ||
        event.type == EventType::OnFrontConnected ||
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
    copy_string(f.UserID, user_id_.c_str(), true);
    copy_string(f.AppID, app_id_.c_str(), true);
    copy_string(f.AuthCode, auth_code_.c_str(), true);
    int ret = api_->ReqAuthenticate(&f, ++request_id_);
    if (ret != 0) {
        SPDLOG_ERROR("td req authenticate failed | account={} ret={}", account_id_, ret);
        return;
    }
    state_machine_.on_req_authenticate();
    // 10s 认证超时
    uint64_t gen = generation_;
    login_timer_id_ = timer_queue_.schedule_after(
        std::chrono::seconds(10),
        [this, gen]() {
            if (gen != generation_) return;
            if (state_machine_.state() == TdState::Authenticating) {
                SPDLOG_ERROR("td authenticate timeout | account={}", account_id_);
                state_machine_.on_authenticate_failed();
            }
        });
}

void AccountSession::req_login() {
    CThostFtdcReqUserLoginField f{};
    copy_string(f.BrokerID, broker_id_.c_str(), true);
    copy_string(f.UserID, user_id_.c_str(), true);
    copy_string(f.Password, password_.c_str(), true);
    int ret = api_->ReqUserLogin(&f, ++request_id_);
    if (ret != 0) {
        SPDLOG_ERROR("td req login failed | account={} ret={}", account_id_, ret);
        return;
    }
    state_machine_.on_req_login();
    uint64_t gen = generation_;
    login_timer_id_ = timer_queue_.schedule_after(
        std::chrono::seconds(10),
        [this, gen]() {
            if (gen != generation_) return;
            if (state_machine_.state() == TdState::LoggingIn) {
                SPDLOG_ERROR("td login timeout | account={}", account_id_);
                state_machine_.on_login_failed();
            }
        });
}

void AccountSession::req_settlement_confirm() {
    CThostFtdcSettlementInfoConfirmField f{};
    copy_string(f.BrokerID, broker_id_.c_str(), true);
    copy_string(f.InvestorID, user_id_.c_str(), true);
    int ret = api_->ReqSettlementInfoConfirm(&f, ++request_id_);
    if (ret != 0) {
        SPDLOG_ERROR("td req settlement confirm failed | account={} ret={}", account_id_, ret);
        return;
    }
    state_machine_.on_req_settlement_confirm();
}

void AccountSession::req_qry_instrument() {
    // C2: 查询全部合约, 填充 instrument_exchange_map_ (设计 §7.2)
    // 终检发现 1: 发起时快照代际, 响应侧校验 (陈旧响应丢弃)
    query_gen_ = generation_;
    CThostFtdcQryInstrumentField qry{};
    // InstrumentID 留空: 查询所有合约
    int ret = api_->ReqQryInstrument(&qry, ++request_id_);
    if (ret != 0) {
        if (ret == -3) {
            // I3: 流控 (-3), 1.5s 后重试 (参考 mdctp 流控队列模式)
            SPDLOG_WARN("td qry instrument flow control, retry in 1.5s | account={}", account_id_);
            uint64_t gen = generation_;
            std::weak_ptr<void> weak = alive_token_;
            timer_queue_.schedule_after(std::chrono::milliseconds(1500),
                [this, weak, gen]() {
                    if (weak.expired()) return;
                    if (gen != generation_) return;
                    if (state_machine_.state() == TdState::LoadingInstruments) {
                        req_qry_instrument();
                    }
                });
            return;
        }
        // C2: 非 -3 错误, 调 on_instruments_load_failed 回退到 LoggedIn (设计 §2.4.1)
        SPDLOG_ERROR("td req qry instrument failed | account={} ret={}", account_id_, ret);
        state_machine_.on_instruments_load_failed(
            std::format("ReqQryInstrument ret={}", ret));
        return;
    }
    // I2: 排定 5 分钟超时, 防止 CTP 长期不回 is_last 导致卡死 (设计 §2.4.1)
    cancel_instruments_load_timer();
    uint64_t gen = generation_;
    instruments_load_timer_id_ = timer_queue_.schedule_after(
        std::chrono::minutes(5),
        [this, gen]() {
            if (gen != generation_) return;
            if (state_machine_.state() == TdState::LoadingInstruments) {
                SPDLOG_ERROR("td instruments load timeout | account={}", account_id_);
                state_machine_.on_instruments_load_failed("query timeout 5min");
            }
        });
}

void AccountSession::cancel_connect_timer() {
    if (connect_timer_id_ != 0) {
        timer_queue_.cancel(connect_timer_id_);
        connect_timer_id_ = 0;
    }
}

void AccountSession::req_qry_investor_position(bool login_chain) {
    ++position_query_token_;
    query_gen_ = generation_;
    position_query_agg_.clear();
    position_query_group_.clear();
    position_rebuild_consumed_ = false;
    position_query_in_flight_ = true;
    if (api_ == nullptr) {
        position_query_in_flight_ = false;
        if (login_chain) {
            finalizer_.on_position_failed();
            position_query_ok_ = false;
            req_qry_trading_account();
        }
        return;
    }
    CThostFtdcQryInvestorPositionField qry{};
    position_query_request_id_ = ++request_id_;
    int ret = api_->ReqQryInvestorPosition(&qry, position_query_request_id_);
    if (ret != 0) {
        position_query_in_flight_ = false;
        if (ret == -3 && login_chain) {
            SPDLOG_WARN("td qry position flow control, retry in 1.5s | account={}", account_id_);
            uint64_t gen = generation_;
            std::weak_ptr<void> weak = alive_token_;
            timer_queue_.schedule_after(std::chrono::milliseconds(1500), [this, weak, gen]() {
                if (weak.expired()) return;
                if (gen != generation_) return;
                if (finalizer_.phase() == Phase::kQueryPosition) {
                    req_qry_investor_position(true);
                }
            });
            return;
        }
        SPDLOG_ERROR("td req qry position failed | account={} ret={} login_chain={}",
                     account_id_, ret, login_chain);
        if (login_chain) {
            finalizer_.on_position_failed();
            position_query_ok_ = false;
            req_qry_trading_account();
        }
        return;
    }
    // 超时兜底 (保留现有语义): 登录 5min / Ready 90s; 任意超时都清在途标志,
    // 周期查询 (phase=kDone) 只清标志, 登录/补查链按原行为降级.
    const auto timeout = is_ready() ? std::chrono::seconds(90) : std::chrono::minutes(5);
    uint64_t gen = generation_;
    uint64_t token = position_query_token_;
    std::weak_ptr<void> weak = alive_token_;
    timer_queue_.schedule_after(timeout, [this, weak, gen, token, login_chain]() {
        if (weak.expired()) return;
        if (gen != generation_) return;
        if (token != position_query_token_) return;  // 已有更新的查询, 本次超时作废
        if (!position_query_in_flight_) return;  // 响应已到, 超时作废
        position_query_in_flight_ = false;
        if (state_machine_.state() == TdState::LoadingInstruments &&
            finalizer_.phase() == Phase::kQueryPosition) {
            SPDLOG_ERROR("td qry position timeout, degrade | account={}", account_id_);
            finalizer_.on_position_failed();
            position_query_ok_ = false;
            req_qry_trading_account();
        } else if (login_chain && is_ready() && finalizer_.phase() == Phase::kQueryPosition) {
            SPDLOG_ERROR("td resync qry position timeout, degrade to done | account={}",
                         account_id_);
            finalizer_.on_position_failed();
            position_query_ok_ = false;
            req_qry_trading_account();
        }
    });
}

void AccountSession::req_qry_trading_account() {
    // Task 6 (spec §4.2 登录收尾): 持仓完成后发起资金查询 (CTP 流控串行).
    // 终检发现 1: 发起时快照代际, 响应侧校验 (陈旧响应丢弃)
    query_gen_ = generation_;
    if (api_ == nullptr) {
        finalizer_.on_account_failed();
        account_query_ok_ = false;
        finalize_login();
        return;
    }
    CThostFtdcQryTradingAccountField qry{};
    int ret = api_->ReqQryTradingAccount(&qry, ++request_id_);
    if (ret != 0) {
        if (ret == -3) {
            SPDLOG_WARN("td qry account flow control, retry in 1.5s | account={}", account_id_);
            uint64_t gen = generation_;
            std::weak_ptr<void> weak = alive_token_;
            timer_queue_.schedule_after(std::chrono::milliseconds(1500),
                [this, weak, gen]() {
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
        account_query_ok_ = false;
        finalize_login();
        return;
    }
    // 超时兜底: 同 req_qry_investor_position — 登录阶段 5min, 补查 (Ready) 阶段
    // 90s (终检发现 5), 到期降级回 kDone 由下一轮 resync 重试。
    const auto timeout = is_ready() ? std::chrono::seconds(90) : std::chrono::minutes(5);
    uint64_t gen = generation_;
    std::weak_ptr<void> weak = alive_token_;
    timer_queue_.schedule_after(timeout,
        [this, weak, gen]() {
            if (weak.expired()) return;
            if (gen != generation_) return;
            if (state_machine_.state() == TdState::LoadingInstruments &&
                finalizer_.phase() == Phase::kQueryAccount) {
                SPDLOG_ERROR("td qry account timeout, degrade | account={}", account_id_);
                finalizer_.on_account_failed();
                account_query_ok_ = false;
                finalize_login();
            } else if (is_ready() && finalizer_.phase() == Phase::kQueryAccount) {
                SPDLOG_ERROR("td resync qry account timeout, degrade to done | account={}",
                             account_id_);
                finalizer_.on_account_failed();
                account_query_ok_ = false;
                finalize_login();
            }
        });
}

void AccountSession::req_qry_margin_rate(const char* instrument_id) {
    // 登录收尾阶段三 (全量账户级) / 按需查询 (单合约, 阶段2).
    // 终检发现 1: 发起时快照代际, 响应侧校验 (陈旧响应丢弃)
    query_gen_ = generation_;
    if (api_ == nullptr) {
        finalizer_.on_margin_rate_failed();
        margin_rate_query_ok_ = false;
        req_qry_commission_rate();  // 串行链不中断
        return;
    }
    CThostFtdcQryInstrumentMarginRateField qry{};
    copy_string(qry.BrokerID, broker_id_.c_str(), true);
    copy_string(qry.InvestorID, account_id_.c_str(), true);
    qry.HedgeFlag = THOST_FTDC_HF_Speculation;  // 决策: 先只取投机保证金率
    if (instrument_id != nullptr && instrument_id[0] != '\0') {
        copy_string(qry.InstrumentID, instrument_id, true);
    }
    int ret = api_->ReqQryInstrumentMarginRate(&qry, ++request_id_);
    if (ret != 0) {
        if (ret == -3) {
            SPDLOG_WARN("td qry margin rate flow control, retry in 1.5s | account={}", account_id_);
            uint64_t gen = generation_;
            std::string inst = instrument_id ? instrument_id : "";
            std::weak_ptr<void> weak = alive_token_;
            timer_queue_.schedule_after(std::chrono::milliseconds(1500),
                [this, weak, gen, inst]() {
                    if (weak.expired()) return;
                    if (gen != generation_) return;
                    // 登录链由 finalizer phase 门, 按需查询 (Ready 后) 直接重试.
                    if (finalizer_.phase() == Phase::kQueryMarginRate || is_ready()) {
                        req_qry_margin_rate(inst.c_str());
                    }
                });
            return;
        }
        SPDLOG_ERROR("td req qry margin rate failed | account={} ret={}", account_id_, ret);
        finalizer_.on_margin_rate_failed();
        margin_rate_query_ok_ = false;
        req_qry_commission_rate();
        return;
    }
    // 超时兜底: 同 req_qry_trading_account — 登录 5min / 补查 (Ready) 90s,
    // 到期降级回 kDone 由下一轮 resync 重试.
    const auto timeout = is_ready() ? std::chrono::seconds(90) : std::chrono::minutes(5);
    uint64_t gen = generation_;
    std::weak_ptr<void> weak = alive_token_;
    timer_queue_.schedule_after(timeout, [this, weak, gen]() {
        if (weak.expired()) return;
        if (gen != generation_) return;
        if (finalizer_.phase() == Phase::kQueryMarginRate) {
            SPDLOG_ERROR("td qry margin rate timeout, degrade | account={}", account_id_);
            finalizer_.on_margin_rate_failed();
            margin_rate_query_ok_ = false;
            req_qry_commission_rate();
        }
    });
}

void AccountSession::req_qry_commission_rate(const char* instrument_id) {
    // 登录收尾阶段四 (全量账户级) / 按需查询 (单合约, 阶段2).
    query_gen_ = generation_;
    if (api_ == nullptr) {
        finalizer_.on_commission_rate_failed();
        commission_rate_query_ok_ = false;
        finalize_login();  // 四查询链尾: 收尾
        return;
    }
    CThostFtdcQryInstrumentCommissionRateField qry{};
    copy_string(qry.BrokerID, broker_id_.c_str(), true);
    copy_string(qry.InvestorID, account_id_.c_str(), true);
    if (instrument_id != nullptr && instrument_id[0] != '\0') {
        copy_string(qry.InstrumentID, instrument_id, true);
    }
    int ret = api_->ReqQryInstrumentCommissionRate(&qry, ++request_id_);
    if (ret != 0) {
        if (ret == -3) {
            SPDLOG_WARN("td qry commission rate flow control, retry in 1.5s | account={}",
                        account_id_);
            uint64_t gen = generation_;
            std::string inst = instrument_id ? instrument_id : "";
            std::weak_ptr<void> weak = alive_token_;
            timer_queue_.schedule_after(std::chrono::milliseconds(1500),
                [this, weak, gen, inst]() {
                    if (weak.expired()) return;
                    if (gen != generation_) return;
                    if (finalizer_.phase() == Phase::kQueryCommissionRate) {
                        req_qry_commission_rate(inst.c_str());
                    }
                });
            return;
        }
        SPDLOG_ERROR("td req qry commission rate failed | account={} ret={}", account_id_, ret);
        finalizer_.on_commission_rate_failed();
        commission_rate_query_ok_ = false;
        finalize_login();
        return;
    }
    const auto timeout = is_ready() ? std::chrono::seconds(90) : std::chrono::minutes(5);
    uint64_t gen = generation_;
    std::weak_ptr<void> weak = alive_token_;
    timer_queue_.schedule_after(timeout, [this, weak, gen]() {
        if (weak.expired()) return;
        if (gen != generation_) return;
        if (finalizer_.phase() == Phase::kQueryCommissionRate) {
            SPDLOG_ERROR("td qry commission rate timeout, degrade | account={}", account_id_);
            finalizer_.on_commission_rate_failed();
            commission_rate_query_ok_ = false;
            finalize_login();
        }
    });
}

void AccountSession::query_fee_rate(const char* instrument_id, int8_t query_type) {
    // 阶段2 按需查询: 设广播模式 (入库+广播 2015/2016), 单合约. 异步回填 (发后即返).
    // query_type=2 (两者) 时先发保证金, 其 is_last 后串行发手续费 (CTP 流控 1 次/秒).
    if (instrument_id == nullptr || instrument_id[0] == '\0') {
        SPDLOG_WARN("td query fee rate empty instrument | account={}", account_id_);
        return;
    }
    fee_rate_broadcast_ = true;
    fee_query_pending_commission_ = (query_type == 2);
    fee_query_instrument_ = instrument_id;
    if (query_type == 0 || query_type == 2) {
        req_qry_margin_rate(instrument_id);
    } else if (query_type == 1) {
        req_qry_commission_rate(instrument_id);
    }
}

void AccountSession::query_instrument(const std::string& instrument_id) {
    // 单合约定向刷新 (契约 instrument): 优先用 DB 行的 symbol (CZCE 人工消歧), 无行则回退 instrument_id.
    // 注意 1: PersistWriter 的 SQLite 连接归 writer 线程独占, 此处用独立只读连接 (WAL 下多连接安全).
    // 注意 2: Ready 前的刷新请求直接拒绝 (登录链会全量查, 无需刷新).
    if (!is_ready()) {
        SPDLOG_WARN("td query instrument rejected | account={} reason=not_ready", account_id_);
        return;
    }
    std::string symbol;
    try {
        if (!lookup_db_) {
            lookup_db_ = std::make_unique<db::SqliteDatabase>(
                dztrader::paths::td_db().string(), SQLite::OPEN_READONLY);
        }
        symbol = tdstore::lookup_symbol(*lookup_db_, instrument_id);
    } catch (const std::exception& e) {
        SPDLOG_WARN("td query instrument: db lookup failed | account={} err={}",
                    account_id_, e.what());
    }
    if (symbol.empty()) {
        symbol = instrument_id;
    }
    auto field = to_qry_instrument_field(symbol);
    const int ret = api_->ReqQryInstrument(&field, ++request_id_);
    if (ret != 0) {
        SPDLOG_WARN("td query instrument failed | account={} instrument={} ret={}",
                    account_id_, instrument_id, ret);
    }
}

void AccountSession::drive_finalizer() {
    // 登录收尾状态机线性推进 (spec §4.2).
    // 每个阶段动作完成后调 next() 取下一阶段; 未满足前置时 next() 停留.
    // 前置: 四查询齐 (含失败降级) 且仍停在最后一个查询阶段时, 先推进到首个
    // 收尾阶段 kReplay (SPI 路径 on_commission_rate_done 只置 done_, 不改变 phase,
    // 由这里跨过查询阶段).
    if (finalizer_.can_reach_ready() && finalizer_.phase() == Phase::kQueryCommissionRate) {
        (void)finalizer_.next();  // kQueryCommissionRate -> kReplay
    }
    while (finalizer_.phase() != Phase::kDone) {
        switch (finalizer_.phase()) {
            case Phase::kQueryPosition:
            case Phase::kQueryAccount:
            case Phase::kQueryMarginRate:
            case Phase::kQueryCommissionRate:
                // 查询阶段由 SPI 回调 (on_rsp_qry_* is_last/失败) 驱动, 这里不推进.
                return;
            case Phase::kReplay:
                replay_buffered_reports();
                break;
            case Phase::kFlush:
                // flush 屏障: 排空 persist 队列且末批已提交后才转 Ready (spec 屏障语义).
                // 登录不在 30μs 热路径, 几 ms 可接受; 5s 超时兜底 (超时仍继续, 不阻塞 Ready).
                {
                    auto token = persist_writer_.enqueue_flush_signal();
                    if (!persist_writer_.wait_flush(token, std::chrono::seconds(5))) {
                        SPDLOG_WARN("td flush barrier timeout, proceed to ready | account={}",
                                    account_id_);
                    }
                }
                break;
            case Phase::kReady:
                // 此刻 persist 已排空, DB 稳定. 状态机翻转 Ready (设计 §5.8);
                // handler 返回后 TdApi 检测 Ready 翻转 -> 广播 2018.
                // 补查重入时状态已是 Ready, 跳过 (不重复翻转/广播).
                if (state_machine_.state() == TdState::LoadingInstruments) {
                    state_machine_.on_instruments_loaded();
                }
                schedule_position_poll();
                break;
            case Phase::kDone:
                return;
        }
        (void)finalizer_.next();
    }
}

void AccountSession::finalize_login() {
    // 四查询完成 (含失败降级) 后进入收尾序列. 防御: 未达前置时 no-op.
    if (!finalizer_.can_reach_ready()) {
        return;
    }
    // 四查询都成功才算数据完整 (供补查节流); 任一失败则由定时补查重试 (spec §4.2).
    data_query_ok_ = position_query_ok_ && account_query_ok_ &&
                     margin_rate_query_ok_ && commission_rate_query_ok_;
    drive_finalizer();
}

void AccountSession::cancel_login_timer() {
    if (login_timer_id_ != 0) {
        timer_queue_.cancel(login_timer_id_);
        login_timer_id_ = 0;
    }
}

void AccountSession::cancel_instruments_load_timer() {
    if (instruments_load_timer_id_ != 0) {
        timer_queue_.cancel(instruments_load_timer_id_);
        instruments_load_timer_id_ = 0;
    }
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
                                         "{:04d}{:02d}{:02d}",
                                         d.year(), d.month(), d.day()).out;
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
        SPDLOG_ERROR("td reject_order failed | account={} order_id={} error=\"{}\"",
                     account_id_, req.order_id, e.what());
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
            std::chrono::system_clock::now().time_since_epoch()).count();
        platform::write_ext_json(event_writer_, DZ_FRAME_TD_RISK_REJECT, reject);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td write_risk_reject failed | account={} error=\"{}\"",
                     account_id_, e.what());
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

void AccountSession::fill_position_info(DzPositionInfo& pos, const PositionHolding& h,
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
        if (ch.long_changed) { push_position(*h, DZ_DIRECTION_LONG); any_change = true; }
        if (ch.short_changed) { push_position(*h, DZ_DIRECTION_SHORT); any_change = true; }
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
            updates.push_back(ActiveOrderUpdate{std::string(o.base.instrument_id),
                                                std::string(o.order_ref), o.base.direction,
                                                o.base.position_effect, o.base.status,
                                                o.base.volume, o.base.volume_traded});
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
    SPDLOG_INFO("td replay buffered | account={} orders={} trades={}",
                account_id_, buffered_orders_.size(), buffered_trades_.size());
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
            SPDLOG_DEBUG("td qry order | account={} instrument={} order_ref={} status={} is_last={}",
                         account_id_, f.order->InstrumentID, f.order->OrderRef,
                         magic_enum::enum_name(STATUS_CTP2VT(f.order->OrderStatus)), f.is_last);
        }
        if (f.is_last) {
            SPDLOG_INFO("td qry order done | account={}", account_id_);
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_order failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_qry_trading_account: 资金查询响应 ===
// Task 6 (spec §4.2 查询链路响应侧 2003 写端 + 登录收尾):
// to_dz_trading_account -> seq 分配 -> 推 DZ_FRAME_TRADING_ACCOUNT -> persist.
// is_last/失败驱动登录收尾状态机 (资金查询完成).
void AccountSession::on_rsp_qry_trading_account(const OnRspQryTradingAccountField& f) {
    // 终检发现 1: 陈旧响应防护 — 旧会话迟到的资金查询响应会错误分配 seq (污染取号器)
    // 并重复触发收尾, 一律丢弃。
    if (query_gen_ != generation_) {
        return;
    }
    try {
        if (f.trading_account) {
            DzTradingAccount acct = to_dz_trading_account(*f.trading_account, account_id_, trading_day_);
            acct.seq = ++seq_counter_;
            platform::write_struct(event_writer_, DZ_FRAME_TRADING_ACCOUNT, acct);
            persist_writer_.enqueue(PersistTask{.kind = PersistTask::Kind::TradingAccount,
                                                .data = acct,
                                                .account_id = account_id_,
                                                .trading_day = trading_day_});
            SPDLOG_INFO("td qry trading account | account={} balance={} available={} seq={}",
                        account_id_, acct.balance, acct.available, acct.seq);
        } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            SPDLOG_ERROR("td qry trading account error | account={} error_id={} error=\"{}\"",
                         account_id_, f.rsp_info->ErrorID,
                         dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
        }
        if (f.is_last) {
            // 资金查询完成 -> 发起保证金率查询 (CTP 流控 1 次/秒, 串行).
            // 收尾序列移至手续费率查询 is_last (四查询链尾) 触发.
            // 无错误 (ErrorID==0) 视为查询成功, 供补查节流 (空账户 is_last 无数据也成功).
            account_query_ok_ = !(f.rsp_info && f.rsp_info->ErrorID != 0);
            // 幂等防御: 超时/失败路径已把 finalizer 推进过 kQueryAccount 时,
            // 迟到的 is_last 仅更新 ok 标志, 不重复触发查询链 (phase 门防御).
            if (finalizer_.phase() == Phase::kQueryAccount) {
                finalizer_.on_account_done();  // -> kQueryMarginRate
                req_qry_margin_rate();         // 全量账户级 (InstrumentID 留空)
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_trading_account failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_qry_investor_position: 持仓查询响应 ===
// Task 5 (spec §4.2 查询链路响应侧): 逐行累加 (同 key 多行合并), is_last 收口应用 —
// 聚合模型 diff 推帧 / 全平零帧 / 活动委托重灌 / PositionRebuild 全量重灌落库.
// Task 6: is_last 完成持仓查询 -> 发起资金查询 (CTP 流控串行).
void AccountSession::on_rsp_qry_investor_position(const OnRspQryInvestorPositionField& f) {
    if (query_gen_ != generation_ || f.request_id != position_query_request_id_) return;
    try {
        const bool has_row = f.investor_position &&
                             (f.investor_position->Position > 0 ||
                              f.investor_position->YdPosition > 0);
        if (has_row) {
            const auto& p = *f.investor_position;
            // 均价换算需合约乘数 (PositionCost 为金额); 合约表未命中/缺失时留 0 -> 均价 0.
            double volume_multiple = 0.0;
            if (auto ex = instrument_exchange_map_.find(p.InstrumentID);
                ex != instrument_exchange_map_.end()) {
                volume_multiple = ex->second.volume_multiple;
            }
            DzPositionInfo pos = to_dz_position(p, account_id_, trading_day_, volume_multiple);
            PositionQueryKey key{std::string(pos.instrument_id),
                                 static_cast<int8_t>(pos.direction)};
            PositionQueryAgg& agg = position_query_agg_[key];
            agg.volume += p.Position;
            agg.yd += p.YdPosition;
            agg.cost += p.PositionCost;
            agg.volume_multiple = volume_multiple;
            agg.ctp_frozen += static_cast<int64_t>(p.LongFrozen) + p.ShortFrozen;
            if (agg.exchange_id.empty()) agg.exchange_id = p.ExchangeID;
        } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            SPDLOG_ERROR("td qry position error | account={} error_id={} error=\"{}\"",
                         account_id_, f.rsp_info->ErrorID,
                         dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
        }
        if (f.is_last) {
            position_query_in_flight_ = false;
            position_query_ok_ = !(f.rsp_info && f.rsp_info->ErrorID != 0);
            if (position_query_ok_ && !position_rebuild_consumed_) {
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

// === on_rsp_qry_instrument_margin_rate: 保证金率查询响应 ===
// 转 DzMarginRate, 按 broadcast 模式 (按需=true / 登录批量=false) 推 SHM + 持久化.
void AccountSession::on_rsp_qry_instrument_margin_rate(const OnRspQryInstrumentMarginRateField& f) {
    // 终检发现 1: 陈旧响应防护 — 断连重连后旧会话迟到的查询响应一律丢弃.
    if (query_gen_ != generation_) {
        return;
    }
    try {
        if (f.margin_rate && (!f.rsp_info || f.rsp_info->ErrorID == 0)) {
            // InvestorRange: IR_All='1'=交易所对所有投资者统一, IR_Group='2'=经纪公司,
            // IR_Single='3'=单一投资者. 只取账户特异性行 (Group/Single), 跳过交易所统一行
            // — 避免同合约多行被 UNIQUE REPLACE 覆盖, 且账户实际费率应优先于交易所标准.
            if (f.margin_rate->InvestorRange == THOST_FTDC_IR_All) {
                // 交易所统一行 (无账户特异性): 登录批量查询时忽略, 由 Group/Single 行承载.
                goto margin_is_last;
            }
            DzMarginRate rec{};
            copy_string(rec.account_id, account_id_.c_str(), true);
            copy_string(rec.instrument_id, f.margin_rate->InstrumentID, true);
            std::string product = normalize_to_product(f.margin_rate->InstrumentID);
            copy_string(rec.product_code, product.c_str(), true);
            copy_string(rec.exchange_id, f.margin_rate->ExchangeID, true);
            rec.hedge_flag = static_cast<int8_t>(f.margin_rate->HedgeFlag);
            rec.is_relative = static_cast<int8_t>(f.margin_rate->IsRelative);
            rec.long_margin_ratio_by_money = f.margin_rate->LongMarginRatioByMoney;
            rec.long_margin_ratio_by_volume = f.margin_rate->LongMarginRatioByVolume;
            rec.short_margin_ratio_by_money = f.margin_rate->ShortMarginRatioByMoney;
            rec.short_margin_ratio_by_volume = f.margin_rate->ShortMarginRatioByVolume;
            rec.date = trading_day_;

            if (fee_rate_broadcast_) {
                platform::write_struct(event_writer_, DZ_FRAME_TD_MARGIN_RATE, rec);
            }
            persist_writer_.enqueue(PersistTask{PersistTask::Kind::MarginRate, rec});

            SPDLOG_INFO("td qry margin rate | account={} instrument={} long={} short={}",
                        account_id_, f.margin_rate->InstrumentID,
                        rec.long_margin_ratio_by_money, rec.short_margin_ratio_by_money);
        } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            SPDLOG_ERROR("td qry margin rate error | account={} error_id={} error=\"{}\"",
                         account_id_, f.rsp_info->ErrorID,
                         dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_instrument_margin_rate failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
margin_is_last:
    if (f.is_last) {
        // 保证金率查询完成.
        margin_rate_query_ok_ = !(f.rsp_info && f.rsp_info->ErrorID != 0);
        if (finalizer_.phase() == Phase::kQueryMarginRate) {
            // 登录收尾链: 推进到手续费率查询 (CTP 流控 1 次/秒, 串行).
            finalizer_.on_margin_rate_done();  // -> kQueryCommissionRate
            req_qry_commission_rate();         // 全量账户级 (InstrumentID 留空)
        } else if (fee_query_pending_commission_) {
            // 按需查询 query_type=2: margin 完成后续发 commission (异步回填).
            fee_query_pending_commission_ = false;
            req_qry_commission_rate(fee_query_instrument_.c_str());
        }
    }
}

// === on_rsp_qry_instrument_commission_rate: 手续费率查询响应 ===
// 转 DzCommissionRate, 按 broadcast 模式 (按需=true / 登录批量=false) 推 SHM + 持久化.
void AccountSession::on_rsp_qry_instrument_commission_rate(const OnRspQryInstrumentCommissionRateField& f) {
    // 终检发现 1: 陈旧响应防护 — 断连重连后旧会话迟到的查询响应一律丢弃.
    if (query_gen_ != generation_) {
        return;
    }
    try {
        if (f.commission_rate && (!f.rsp_info || f.rsp_info->ErrorID == 0)) {
            // 手续费率响应同含 InvestorRange (IR_All/Group/Single): 同 margin, 只取账户特异性行.
            if (f.commission_rate->InvestorRange == THOST_FTDC_IR_All) {
                goto commission_is_last;
            }
            DzCommissionRate rec{};
            copy_string(rec.account_id, account_id_.c_str(), true);
            copy_string(rec.instrument_id, f.commission_rate->InstrumentID, true);
            std::string product = normalize_to_product(f.commission_rate->InstrumentID);
            copy_string(rec.product_code, product.c_str(), true);
            copy_string(rec.exchange_id, f.commission_rate->ExchangeID, true);
            rec.open_ratio_by_money = f.commission_rate->OpenRatioByMoney;
            rec.open_ratio_by_volume = f.commission_rate->OpenRatioByVolume;
            rec.close_ratio_by_money = f.commission_rate->CloseRatioByMoney;
            rec.close_ratio_by_volume = f.commission_rate->CloseRatioByVolume;
            rec.close_today_ratio_by_money = f.commission_rate->CloseTodayRatioByMoney;
            rec.close_today_ratio_by_volume = f.commission_rate->CloseTodayRatioByVolume;
            rec.date = trading_day_;

            if (fee_rate_broadcast_) {
                platform::write_struct(event_writer_, DZ_FRAME_TD_COMMISSION_RATE, rec);
            }
            persist_writer_.enqueue(PersistTask{PersistTask::Kind::CommissionRate, rec});

            SPDLOG_INFO("td qry commission rate | account={} instrument={} open_money={} close_money={}",
                        account_id_, f.commission_rate->InstrumentID,
                        rec.open_ratio_by_money, rec.close_ratio_by_money);
        } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            SPDLOG_ERROR("td qry commission rate error | account={} error_id={} error=\"{}\"",
                         account_id_, f.rsp_info->ErrorID,
                         dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_instrument_commission_rate failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
commission_is_last:
    if (f.is_last) {
        // 手续费率查询完成 (四查询链尾) -> 进入收尾序列.
        commission_rate_query_ok_ = !(f.rsp_info && f.rsp_info->ErrorID != 0);
        if (finalizer_.phase() == Phase::kQueryCommissionRate) {
            finalizer_.on_commission_rate_done();
            finalize_login();
        }
    }
}

// === on_rtn_instrument_status: 合约交易状态回报 ===
// 转 DzInstrumentStatus (内联转换), 推 SHM
void AccountSession::on_rtn_instrument_status(const OnRtnInstrumentStatusField& f) {
    try {
        DzInstrumentStatus rec{};
        copy_string(rec.instrument_id, f.instrument_status.InstrumentID, true);
        copy_string(rec.exchange_id, f.instrument_status.ExchangeID, true);
        rec.status = static_cast<int8_t>(f.instrument_status.InstrumentStatus);
        rec.time = parse_ctp_time(f.instrument_status.EnterTime);

        platform::write_struct(event_writer_, DZ_FRAME_TD_INSTRUMENT_STATUS, rec);

        SPDLOG_DEBUG("td rtn instrument status | account={} instrument={} status={}",
                     account_id_, f.instrument_status.InstrumentID,
                     f.instrument_status.InstrumentStatus);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rtn_instrument_status failed | account={} error=\"{}\"",
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
                SPDLOG_DEBUG("td rsp order insert (no error) | account={} instrument={} order_ref={}",
                             account_id_, f.input_order->InstrumentID, f.input_order->OrderRef);
            }
            return;
        }

        // I8: rsp_info 缺失视为失败 (与 on_rsp_authenticate / on_rsp_user_login 一致)
        int error_id = f.rsp_info ? f.rsp_info->ErrorID : -1;
        std::string error_msg = f.rsp_info
            ? dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg)
            : "rsp_info is null";

        if (!f.input_order) {
            SPDLOG_ERROR("td rsp order insert error, no input_order | account={} error_id={} error=\"{}\"",
                         account_id_, error_id, error_msg);
            return;
        }

        // 查 OrderRefMap 找 order_id
        const DzOrderId* local = order_ref_map_.find_by_order_ref(f.input_order->OrderRef);
        if (local == nullptr) {
            SPDLOG_WARN("td rsp order insert error, order_ref not found | account={} order_ref={} error_id={} error=\"{}\"",
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
        rec.base.direction = (f.input_order->Direction == THOST_FTDC_D_Buy) ? DZ_DIRECTION_LONG : DZ_DIRECTION_SHORT;
        switch (f.input_order->CombOffsetFlag[0]) {
            case THOST_FTDC_OF_Open:           rec.base.position_effect = DZ_POSITION_EFFECT_OPEN;           break;
            case THOST_FTDC_OF_Close:          rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE;          break;
            case THOST_FTDC_OF_CloseToday:     rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE_TODAY;    break;
            case THOST_FTDC_OF_CloseYesterday: rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE_YESTDAY;  break;
            default:                           rec.base.position_effect = DZ_POSITION_EFFECT_OPEN;           break;
        }
        switch (f.input_order->OrderPriceType) {
            case THOST_FTDC_OPT_AnyPrice:   rec.base.price_type = DZ_PRICE_MARKET; break;
            case THOST_FTDC_OPT_LimitPrice: rec.base.price_type = DZ_PRICE_LIMIT;  break;
            default:                        rec.base.price_type = DZ_PRICE_LIMIT;  break;
        }
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
                                         "{:04d}{:02d}{:02d}",
                                         d.year(), d.month(), d.day()).out;
            *end = '\0';
        }

        // 本地拒单路径 (同 reject_order): 不经过滤器 (本地事件必新), 分配 seq + 更新基准.
        rec.base.seq = ++seq_counter_;
        if (report_filter_) {
            report_filter_->accept_order(rec);
        }

        write_order_rpt(rec.base);
        persist_order(rec);

        SPDLOG_ERROR("td rsp order insert rejected | account={} order_id={} order_ref={} error_id={} error=\"{}\"",
                     account_id_, *local, f.input_order->OrderRef, error_id, error_msg);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_order_insert failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_order_action: 报单操作响应 (撤单同步拒绝) ===
// 撤单失败不影响订单状态 (CTP 会通过 OnRtnOrder 推 CANCELLED), 仅记 WARN
void AccountSession::on_rsp_order_action(const OnRspOrderActionField& f) {
    try {
        if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
            SPDLOG_WARN("td rsp order action error | account={} error_id={} error=\"{}\" order_ref={}",
                        account_id_, f.rsp_info->ErrorID, error_msg,
                        f.input_order_action ? f.input_order_action->OrderRef : "");
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_order_action failed | account={} error=\"{}\"",
                     account_id_, e.what());
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
            SPDLOG_ERROR("td err rtn order insert, no input_order | account={} error_id={} error=\"{}\"",
                         account_id_, error_id, error_msg);
            return;
        }

        const DzOrderId* local = order_ref_map_.find_by_order_ref(f.input_order->OrderRef);
        if (local == nullptr) {
            SPDLOG_WARN("td err rtn order insert, order_ref not found | account={} order_ref={} error_id={} error=\"{}\"",
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
        rec.base.direction = (f.input_order->Direction == THOST_FTDC_D_Buy) ? DZ_DIRECTION_LONG : DZ_DIRECTION_SHORT;
        switch (f.input_order->CombOffsetFlag[0]) {
            case THOST_FTDC_OF_Open:           rec.base.position_effect = DZ_POSITION_EFFECT_OPEN;           break;
            case THOST_FTDC_OF_Close:          rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE;          break;
            case THOST_FTDC_OF_CloseToday:     rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE_TODAY;    break;
            case THOST_FTDC_OF_CloseYesterday: rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE_YESTDAY;  break;
            default:                           rec.base.position_effect = DZ_POSITION_EFFECT_OPEN;           break;
        }
        switch (f.input_order->OrderPriceType) {
            case THOST_FTDC_OPT_AnyPrice:   rec.base.price_type = DZ_PRICE_MARKET; break;
            case THOST_FTDC_OPT_LimitPrice: rec.base.price_type = DZ_PRICE_LIMIT;  break;
            default:                        rec.base.price_type = DZ_PRICE_LIMIT;  break;
        }
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
                                         "{:04d}{:02d}{:02d}",
                                         d.year(), d.month(), d.day()).out;
            *end = '\0';
        }

        // 本地拒单路径 (同 reject_order): 不经过滤器 (本地事件必新), 分配 seq + 更新基准.
        rec.base.seq = ++seq_counter_;
        if (report_filter_) {
            report_filter_->accept_order(rec);
        }

        write_order_rpt(rec.base);
        persist_order(rec);

        SPDLOG_ERROR("td err rtn order insert rejected | account={} order_id={} order_ref={} error_id={} error=\"{}\"",
                     account_id_, *local, f.input_order->OrderRef, error_id, error_msg);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_err_rtn_order_insert failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_err_rtn_order_action: 报单操作错误回报 (撤单被拒) ===
// 同 on_rsp_order_action, 仅记 WARN (撤单被拒不影响订单状态)
void AccountSession::on_err_rtn_order_action(const OnErrRtnOrderActionField& f) {
    try {
        if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
            SPDLOG_WARN("td err rtn order action | account={} error_id={} error=\"{}\" order_ref={}",
                        account_id_, f.rsp_info->ErrorID, error_msg,
                        f.order_action ? f.order_action->OrderRef : "");
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_err_rtn_order_action failed | account={} error=\"{}\"",
                     account_id_, e.what());
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

        SPDLOG_INFO("td rsp transfer | account={} trade_code={} amount={} error_id={}",
                    account_id_, f.req_transfer->TradeCode, f.req_transfer->TradeAmount, rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_transfer failed | account={} error=\"{}\"",
                     account_id_, e.what());
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

        SPDLOG_INFO("td rtn transfer | account={} trade_code={} amount={} error_id={}",
                    account_id_, f.rsp_transfer.TradeCode, f.rsp_transfer.TradeAmount, rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rtn_transfer failed | account={} error=\"{}\"",
                     account_id_, e.what());
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

        SPDLOG_INFO("td rsp user password update | account={} error_id={}",
                    account_id_, rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_user_password_update failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_trading_account_password_update: 修改资金密码响应 ===
// 转 platform::DzPasswordUpdateRsp (password_type="A"), 推 SHM (JSON, 契约 td-account-ops)
void AccountSession::on_rsp_trading_account_password_update(const OnRspTradingAccountPasswordUpdateField& f) {
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

        SPDLOG_INFO("td rsp trading account password update | account={} error_id={}",
                    account_id_, rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_trading_account_password_update failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

}  // namespace dztrader::ctp
