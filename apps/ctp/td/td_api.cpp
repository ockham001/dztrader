#include "td/td_api.h"

#include <chrono>
#include <cstring>
#include <format>
#include <limits>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/core_struct.h>  // DzOrderReq / DzOrderCancelReq
#include <dztrader/core/string_util.h>  // copy_string
#include <dztrader/data_type.h>
#include <dztrader/date_time/date_time.h>  // Date::from_string ("YYYYMMDD" -> DzDate)
#include <dztrader/platform/frame_codec.h>
#include <dztrader/shm/frame_codec.h>  // decode_ext_inst_json, decode_ext_json
#include <dztrader/struct.h>

namespace dztrader::ctp {

using namespace dztrader::shm;

// ============================================================================
// 构造 / 析构 / set_configs
// ============================================================================

TdApi::TdApi(std::string name,
             std::filesystem::path shm_dir,
             std::shared_ptr<shm::ChannelMeta> event_meta,
             MpmcQueuePtr event_queue,
             std::filesystem::path flow_dir,
             std::string reader_name,
             std::filesystem::path cfg_path)
    : name_(std::move(name)),
      shm_dir_(std::move(shm_dir)),
      flow_dir_(std::move(flow_dir)),
      reader_name_(std::move(reader_name)),
      event_meta_(std::move(event_meta)),
      event_queue_(std::move(event_queue)),
      // reader/event_writer 均用 reader_name_ 注册 (与 md_api.cpp 一致, 含 pid 保证唯一):
      //   - reader: master notify_subscribers 唤醒本进程的信号量
      //   - writer: set_writer_page_index 用此 name 作 key (进程级唯一, 含 pid 避免同名进程冲突)
      reader_{shm::Reader::create(event_meta_, reader_name_)},
      event_writer_{shm::MultiWriter::create(event_meta_, reader_name_)},
      config_path_(std::move(cfg_path)),
      notify_ui_(name_, event_writer_),
      log_config_(name_, config_path_),
      auto_login_config_(name_, config_path_, event_writer_) {
    // 创建跨进程共享的 OrderIdMeta (本进程是创建者, open_or_create 模式)
    // 文件布局: shm_dir_/<name_>/order_id.dat
    // 多账户共享同一计数器 (TdApi 持有, AccountSession 通过引用获取)
    order_id_meta_ = std::make_unique<shm::OrderIdMeta>(
        shm::OrderIdMeta::open_or_create(name_, shm_dir_));

    // 创建并打开 PersistWriter (SQLite 持久化, 启动时一次性 open + start_writer)
    // db 路径: flow_dir_/<name_>.db (进程级单例, 多账户共享)
    auto db_path = flow_dir_ / (name_ + ".db");
    persist_writer_ = std::make_unique<PersistWriter>(db_path.string());
    persist_writer_->open();
    // 启动自检 (设计 §13 step 8): 必须在 start_writer() 前执行 (主线程独占 db)
    verify_order_id_against_db();
    persist_writer_->start_writer();

    // 加载日志配置 (从 dztd_ctp.json "log" section, 失败用默认值自愈, 与 spdlog 同步)
    log_config_.load();
    // 加载自动登录/登出排程 (从 dztd_ctp.json "auto_login" section, 失败用默认值自愈)
    auto_login_config_.load();
}

TdApi::~TdApi() {
    running_ = false;
    // 1. 显式 disconnect 所有 AccountSession (CTP API Release 同步等待 SPI 线程退出)
    //    AccountSession 析构会再次 disconnect (幂等), 但显式调用确保顺序: 先停 SPI 再停 PersistWriter
    for (auto& [account_id, session] : sessions_) {
        try {
            session->disconnect();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("td session disconnect failed during shutdown | account={} error=\"{}\"",
                         account_id, e.what());
        } catch (...) {
            SPDLOG_ERROR("td session disconnect unknown exception during shutdown | account={}",
                         account_id);
        }
    }
    // 2. stop PersistWriter (drain 残留队列 + join Writer 线程, 30s 超时则 quick_exit)
    if (persist_writer_ != nullptr) {
        try {
            persist_writer_->stop();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("persist writer stop failed | error=\"{}\"", e.what());
        } catch (...) {
            SPDLOG_ERROR("persist writer stop unknown exception");
        }
    }
    // 3. sessions_ 析构由 unique_ptr 自动处理 (AccountSession 析构幂等 disconnect + 清理 CTP 资源)
    // 4. 清空事件队列 (此时所有 SPI 线程已退出, 无新事件)
    drain_event_queue();
}

void TdApi::set_configs(TdConfig td_cfg) {
    config_ = std::move(td_cfg);
}

void TdApi::verify_order_id_against_db() {
    // 设计 §13 step 8 + 开放问题 7: 跨机导入数据库后, 保证原子计数器 > 库内最大 order_id.
    // 调用时机: PersistWriter::open() 后 start_writer() 前 (主线程独占 db, 无竞争).
    const int64_t db_max = persist_writer_->max_order_id();
    const DzOrderId before = order_id_meta_->current();
    if (db_max >= before) {
        order_id_meta_->ensure_at_least(db_max + 1);
        SPDLOG_WARN("order id self-check bumped | db_max={} counter_before={} counter_after={}",
                    db_max, before, order_id_meta_->current());
    } else {
        SPDLOG_INFO("order id self-check ok | db_max={} counter={}", db_max, before);
    }
}

// ============================================================================
// 主循环 (设计 §6.3)
// ============================================================================

void TdApi::run() {
    running_ = true;
    if (!started_) {
        // 首次进入: 上报完整快照 + 广播服务启动 + (可选) 补登 + 排定自动调度定时器
        // 重入时跳过, 避免重复广播和重复排定时器
        report_full_snapshot();
        platform::write_ext_inst_raw(event_writer_, DZ_FRAME_NOTIFY_TD_STARTED, name_);
        if (recover_) {
            try_recover_login();
        }
        schedule_auto_sched_timer();
        started_ = true;
    }
    while (running_) {
        for (;;) {
            int n = 0;
            int m = 0;
            // 批量处理 SHM 帧 (最多 32 个), 避免事件队列饿死
            for (; n < 32; ++n) {
                const auto* frame = reader_.next_frame();
                if (!frame) {
                    break;
                }
                handle_frame(frame);
            }
            // 批量处理 SPI 事件 (最多 32 个), 避免定时器饿死
            for (; m < 32; ++m) {
                Event event;
                if (!event_queue_->pop(event)) {
                    break;
                }
                process_event(event);
            }
            if (n < 32 && m < 32) {
                break;  // 帧和事件都处理完, 退出内层循环
            }
        }
        if (!running_) {
            break;  // 收到 DZ_FRAME_REQUEST_SHUTDOWN, 不阻塞等待
        }
        // 触发所有已到期的定时器 (自动调度 / SHM 维护 / 连接超时等)
        timer_queue_.tick();
        if (!running_) {
            break;
        }
        // 阻塞等待事件: 有定时器时精确超时唤醒, 无定时器时无限等待
        // 唤醒源: SPI 线程 notify / master notify / (有定时器时) 超时 / 信号
        if (timer_queue_.empty()) {
            event_queue_->wait();  // 无定时器, 无限等待 (CPU=0, 等事件唤醒)
        } else {
            // next_timeout() 返回 native duration (无 uint32_t 截断),
            // NamedSemaphore::wait_for 接受 uint32_t ms, 需 cast + clamp 防溢出
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          timer_queue_.next_timeout())
                          .count();
            constexpr auto kMaxMs = static_cast<long long>(std::numeric_limits<uint32_t>::max());
            uint32_t timeout_ms =
                ms >= kMaxMs ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(ms);
            event_queue_->wait_for(timeout_ms);  // 精确超时唤醒
        }
        // 信号唤醒后检查外部 shutdown 标志
        if (external_shutdown_flag_ != nullptr &&
            external_shutdown_flag_->load(std::memory_order_relaxed)) {
            SPDLOG_INFO("external shutdown requested by signal");
            handle_shutdown();
        }
    }
    SPDLOG_INFO("dztd_ctp run loop exited");
}

void TdApi::drain_event_queue() {
    // 析构时清理队列残留事件, 防止 shutdown 与 SPI 回调竞态导致泄漏。
    // 不调用 handler (进程即将退出), 仅按 EventType 释放 data。
    // AccountSession::delete_event 内部按 type 路由:
    //   - td 类型 (>=100) -> td_delete_event_data
    //   - md 类型 (0-13)  -> Event::delete_data (兜底, td 进程不应收到 md 事件)
    //   - data 为 nullptr 时 no-op
    Event event;
    while (event_queue_->pop(event)) {
        AccountSession::delete_event(event);
    }
}

// ============================================================================
// 帧处理 / 事件 dispatch
// ============================================================================

AccountSession* TdApi::find_session(const std::string& account_id) {
    // sessions_ 简单 map 查找, 未找到返回 nullptr
    auto it = sessions_.find(account_id);
    return it != sessions_.end() ? it->second.get() : nullptr;
}

void TdApi::handle_frame(const std::byte* frame) {
    try {
        handle_frame_inner(frame);
    } catch (const std::exception& e) {
        // 外层兜底: 防御性, 理论上不应触发 (各 case 内部已处理自身异常)
        // 仅记日志, 避免异常逃逸到 run() 导致进程崩溃
        SPDLOG_ERROR("handle_frame unexpected exception | error=\"{}\"", e.what());
    } catch (...) {
        // 兜底非 std 异常, 仅记日志不崩溃
        SPDLOG_ERROR("handle_frame unexpected non-std exception");
    }
}

void TdApi::handle_frame_inner(const std::byte* frame) {
    // 设计 §4.4 帧处理模式 (与 md_api.cpp 一致), 差异见契约 td-order:
    //   - basic 广播帧: 通用控制帧不做目标匹配; TD_ORDER_REQ / TD_ORDER_CANCEL_REQ 按 payload account_id 归属路由
    //   - 含 instance_id 帧: instance_id == name_ (进程级, 如 TD_CONNECT / TD_DISCONNECT / SET_LOG_CONFIG 等)
    //     或以 "name_:" 开头 (账户级, 设计 §1.1)
    FrameView view(frame);

    // 1. 无 instance_id 帧 (basic 帧)
    switch (view.type()) {
        case DZ_FRAME_PRELOAD_EVENT_SHM: {
            // 广播帧: 携带 DzShmPreload payload (pages/bytes), 随机延迟后预加载 event 通道
            const auto& params = view.payload<DzShmPreload>();
            schedule_event_shm_preload(params);
            return;
        }
        case DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER: {
            // 广播: 刷新 event writer 订阅者列表
            event_writer_.refresh_subscribers();
            return;
        }
        case DZ_FRAME_QUERY_FULL_SNAPSHOT: {
            // 全量快照查询 (所有进程处理): 触发各进程上报完整快照 (配置+状态)
            report_full_snapshot();
            return;
        }
        case DZ_FRAME_TD_QUERY_ACCOUNT_STATUS: {
            // 契约 account-status: 2115 basic 广播帧, 空=全量应答, 指定=命中配置才应答
            handle_query_account_status(frame);
            return;
        }
        case DZ_FRAME_TD_ORDER_REQ: {
            // 契约 td-order: basic 广播帧, 按 payload account_id 归属路由
            on_order_req(view);
            return;
        }
        case DZ_FRAME_TD_ORDER_CANCEL_REQ: {
            // 契约 td-order: basic 广播帧, 按 payload account_id 归属路由
            on_cancel_req(view);
            return;
        }
        default:
            break;
    }

    // 2. 含 instance_id 帧: instance_id == name_ (进程级) 或以 "name_:" 开头 (账户级, 设计 §1.1)
    //    (TD_ORDER_REQ / TD_ORDER_CANCEL_REQ 属 basic 广播帧, 已在第 1 层处理, 见契约 td-order)
    std::string_view inst(view.ext_inst_id());
    std::string prefix_with_colon = name_ + ":";
    if (inst != name_ && !inst.starts_with(prefix_with_colon)) {
        return;
    }

    switch (view.type()) {
        case DZ_FRAME_TD_CONNECT: {
            connect_account(view);
            return;
        }
        case DZ_FRAME_TD_DISCONNECT: {
            disconnect_account(view);
            return;
        }
        case DZ_FRAME_TD_REQ_MODIFY_CONFIG: {
            // 配置 op 变更: decode TdConfigOpReq -> apply_config_change (decode 失败仅记日志 + 通知 UI)
            try {
                apply_config_change(decode_ext_inst_json<TdConfigOpReq>(view));
            } catch (const std::exception& e) {
                SPDLOG_WARN("td config op decode rejected | error=\"{}\"", e.what());
                notify_ui_.error(std::format("交易配置解码失败: {}", e.what()));
            }
            return;
        }
        case DZ_FRAME_REQUEST_SHUTDOWN: {
            // 定向: instance_id == name_, 触发优雅退出
            handle_shutdown();
            return;
        }
        case DZ_FRAME_SET_LOG_CONFIG: {
            // 定向: instance_id == name_。payload = JSON patch。
            // 流程: decode -> set_log_config -> rtn
            // 失败: catch -> 日志 + notify_ui -> rtn (旧值)
            try {
                log_config_.set_log_config(
                    decode_ext_inst_json<nlohmann::json>(view));
            } catch (const std::exception& e) {
                SPDLOG_ERROR("set log config failed | error=\"{}\"", e.what());
                notify_ui_.error(std::format("日志配置更新失败: {}", e.what()));
            }
            log_config_.rtn_log_config(event_writer_);
            return;
        }
        case DZ_FRAME_SET_AUTO_LOGIN: {
            // 定向: instance_id == name_。payload = JSON patch。
            // 流程: decode -> set_auto_login -> rtn
            // 失败: catch -> 日志 + notify_ui -> rtn (旧值)
            try {
                auto_login_config_.set_auto_login(
                    decode_ext_inst_json<nlohmann::json>(view));
            } catch (const std::exception& e) {
                SPDLOG_ERROR("set auto login failed | error=\"{}\"", e.what());
                notify_ui_.error(std::format("自动登录配置更新失败: {}", e.what()));
            }
            auto_login_config_.rtn_auto_login();
            return;
        }
        case DZ_FRAME_FLUSH_LOG: {
            spdlog::default_logger()->flush();
            SPDLOG_INFO("log flushed by request");
            return;
        }
        default:
            break;
    }
}

void TdApi::process_event(Event& event) {
    // 设计 §1.1 多账户路由: 按 EventType 路由到对应 AccountSession on_* 方法
    // - OnFrontConnected / OnFrontDisconnected: 用 td 版本 Field (含 account_id), 但
    //   AccountSession::on_front_connected / on_front_disconnected 签名非 const T&,
    //   不能用 dispatch<T> 模板, 单独走 handle_front_event 辅助
    // - td 类型 (100+): dispatch<T> 模板按 rsp->account_id 路由
    // - md 类型 (3, 4-13): td 进程不应收到, 兜底释放防泄漏
    switch (event.type) {
        case EventType::OnFrontConnected:
        case EventType::OnFrontDisconnected:
            handle_front_event(event);
            return;
        case EventType::OnRspAuthenticate:
            dispatch<OnRspAuthenticateField>(event, &AccountSession::on_rsp_authenticate);
            return;
        case EventType::OnRspTdUserLogin:
            dispatch<OnRspTdUserLoginField>(event, &AccountSession::on_rsp_user_login);
            return;
        case EventType::OnRspSettlementInfoConfirm:
            dispatch<OnRspSettlementInfoConfirmField>(event, &AccountSession::on_rsp_settlement_confirm);
            return;
        case EventType::OnRspQryInstrument:
            dispatch<OnRspQryInstrumentField>(event, &AccountSession::on_rsp_qry_instrument);
            return;
        case EventType::OnRtnOrder:
            dispatch<OnRtnOrderField>(event, &AccountSession::on_rtn_order);
            return;
        case EventType::OnRtnTrade:
            dispatch<OnRtnTradeField>(event, &AccountSession::on_rtn_trade);
            return;
        // C5: 补齐缺失的 SPI 事件 dispatch (AccountSession::on_* 已实现)
        case EventType::OnRspQryOrder:
            dispatch<OnRspQryOrderField>(event, &AccountSession::on_rsp_qry_order);
            return;
        case EventType::OnRspQryTradingAccount:
            dispatch<OnRspQryTradingAccountField>(event, &AccountSession::on_rsp_qry_trading_account);
            return;
        case EventType::OnRspQryInvestorPosition:
            dispatch<OnRspQryInvestorPositionField>(event, &AccountSession::on_rsp_qry_investor_position);
            return;
        case EventType::OnRspQryInvestorPositionDetail:
            dispatch<OnRspQryInvestorPositionDetailField>(event, &AccountSession::on_rsp_qry_investor_position_detail);
            return;
        case EventType::OnRspQryInstrumentMarginRate:
            dispatch<OnRspQryInstrumentMarginRateField>(event, &AccountSession::on_rsp_qry_instrument_margin_rate);
            return;
        case EventType::OnRspQryInstrumentCommissionRate:
            dispatch<OnRspQryInstrumentCommissionRateField>(event, &AccountSession::on_rsp_qry_instrument_commission_rate);
            return;
        case EventType::OnRtnInstrumentStatus:
            dispatch<OnRtnInstrumentStatusField>(event, &AccountSession::on_rtn_instrument_status);
            return;
        case EventType::OnRspOrderInsert:
            dispatch<OnRspOrderInsertField>(event, &AccountSession::on_rsp_order_insert);
            return;
        case EventType::OnRspOrderAction:
            dispatch<OnRspOrderActionField>(event, &AccountSession::on_rsp_order_action);
            return;
        case EventType::OnErrRtnOrderInsert:
            dispatch<OnErrRtnOrderInsertField>(event, &AccountSession::on_err_rtn_order_insert);
            return;
        case EventType::OnErrRtnOrderAction:
            dispatch<OnErrRtnOrderActionField>(event, &AccountSession::on_err_rtn_order_action);
            return;
        case EventType::OnRspFromBankToFutureByFuture:
            dispatch<OnRspFromBankToFutureByFutureField>(event, &AccountSession::on_rsp_transfer);
            return;
        case EventType::OnRtnFromBankToFutureByFuture:
            dispatch<OnRtnFromBankToFutureByFutureField>(event, &AccountSession::on_rtn_transfer);
            return;
        case EventType::OnRspUserPasswordUpdate:
            dispatch<OnRspUserPasswordUpdateField>(event, &AccountSession::on_rsp_user_password_update);
            return;
        case EventType::OnRspTradingAccountPasswordUpdate:
            dispatch<OnRspTradingAccountPasswordUpdateField>(event, &AccountSession::on_rsp_trading_account_password_update);
            return;
        default:
            // md 类型 (3, 4-13) td 进程不应收到, 兜底释放防泄漏
            SPDLOG_WARN("unexpected event in td | type={}", static_cast<int>(event.type));
            AccountSession::delete_event(event);
            return;
    }
}

void TdApi::handle_front_event(Event& event) {
    // OnFrontConnected / OnFrontDisconnected 用 td 版本 Field (含 account_id)
    // AccountSession::on_front_connected() 无参数, on_front_disconnected(int reason) 非 const T&
    // 不能用 dispatch<T> 模板, 单独处理
    if (event.data == nullptr) {
        // push 端保证非 null, 这里防御性检查
        return;
    }
    std::string account_id;
    int reason = 0;
    if (event.type == EventType::OnFrontConnected) {
        auto* f = static_cast<OnTdFrontConnectedField*>(event.data);
        account_id = f->account_id;
    } else {
        auto* f = static_cast<OnTdFrontDisconnectedField*>(event.data);
        account_id = f->account_id;
        reason = f->reason;
    }
    auto* session = find_session(account_id);
    if (session == nullptr) {
        SPDLOG_WARN("front event for unknown account | type={} account={}",
                    magic_enum::enum_name(event.type), account_id);
        AccountSession::delete_event(event);
        return;
    }
    // 捕获 handler 前状态, 用于 Ready 翻转检测 (设计 §5.8 健康度广播)
    TdState before = session->state();
    try {
        if (event.type == EventType::OnFrontConnected) {
            session->on_front_connected();
        } else {
            session->on_front_disconnected(reason);
        }
    } catch (const dztrader::Exception& e) {
        SPDLOG_ERROR("handle front event failed | type={} account={} code={} error=\"{}\"",
                     magic_enum::enum_name(event.type), account_id, e.code(), e.what());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("handle front event failed | type={} account={} error=\"{}\"",
                     magic_enum::enum_name(event.type), account_id, e.what());
    } catch (...) {
        SPDLOG_ERROR("handle front event failed | type={} account={} error=unknown_exception",
                     magic_enum::enum_name(event.type), account_id);
    }
    // handler 后状态: 进入/离开 Ready 时广播健康度 (设计 §5.8 per-account TdHealth 翻转)
    TdState after = session->state();
    if (before != TdState::Ready && after == TdState::Ready) {
        broadcast_health(account_id, TdHealth::Up);
    } else if (before == TdState::Ready && after != TdState::Ready) {
        broadcast_health(account_id, TdHealth::Down);
    }
    // 契约 account-status 场景 2: 前置连接/断开的三态翻转推送 (非 force, 同上)
    write_account_status(account_id, account_state_of(after),
                         session->state_machine().status().trading_day);
    AccountSession::delete_event(event);
}

void TdApi::handle_shutdown() {
    // 设计 §5.7 优雅退出: running_=false + disconnect 所有 AccountSession + stop PersistWriter
    // 幂等: running_ 已 false 时直接返回 (避免多次调用导致重复 disconnect/stop)
    if (!running_) {
        return;
    }
    SPDLOG_INFO("shutdown request received, exiting");
    running_ = false;
    // 1. 对每个 AccountSession 调用 disconnect() (CTP Release 同步等待 SPI 线程退出)
    //    异常不传播, 仅记日志 (符合 "宁肯乱码也不能崩溃")
    for (auto& [account_id, session] : sessions_) {
        try {
            session->disconnect();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("disconnect failed on shutdown | account={} error=\"{}\"",
                         account_id, e.what());
        } catch (...) {
            SPDLOG_ERROR("disconnect unknown exception on shutdown | account={}", account_id);
        }
    }
    // 2. stop PersistWriter (drain 残留队列 + join Writer 线程)
    if (persist_writer_ != nullptr) {
        try {
            persist_writer_->stop();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("persist writer stop failed on shutdown | error=\"{}\"", e.what());
        } catch (...) {
            SPDLOG_ERROR("persist writer stop unknown exception on shutdown");
        }
    }
    SPDLOG_INFO("handle_shutdown completed");
}

void TdApi::connect_account(const shm::FrameView& view) {
    // 设计 §1.1: TD_CONNECT 帧的 payload 是 JSON {"account_id":"xxx"}
    // 解析 account_id 后委托给 connect_account_by_id (自动调度 / try_recover_login 共用)

    std::string account_id;
    try {
        auto j = decode_ext_inst_json<nlohmann::json>(view);
        account_id = j.value("account_id", "");
    } catch (const std::exception& e) {
        SPDLOG_WARN("td connect decode failed | error=\"{}\"", e.what());
        notify_ui_.error(std::format("连接请求解析失败: {}", e.what()));
        return;
    }
    if (account_id.empty()) {
        SPDLOG_WARN("td connect rejected | reason=empty_account_id");
        notify_ui_.error("连接请求缺少 account_id");
        return;
    }
    connect_account_by_id(account_id);
}

void TdApi::connect_account_by_id(const std::string& account_id) {
    // 查 config_.accounts -> 构造 AccountSession -> open()
    // 异常不传播 (仅记日志 + 通知 UI), 符合 "宁肯乱码也不能崩溃"

    // 1. 查 config_.accounts 找 AccountConfig (用 find_account_in 纯函数)
    const auto* cfg = find_account_in(config_, account_id);
    if (cfg == nullptr) {
        SPDLOG_WARN("td connect rejected | reason=account_not_found account={}", account_id);
        notify_ui_.error(std::format("账户未配置: {}", account_id));
        return;
    }
    if (!cfg->enabled) {
        SPDLOG_WARN("td connect rejected | reason=account_disabled account={}", account_id);
        notify_ui_.error(std::format("账户已禁用: {}", account_id));
        return;
    }

    // 2. sessions_ 已存在该 account_id 则跳过 (幂等)
    if (find_session(account_id) != nullptr) {
        SPDLOG_INFO("td connect skipped | reason=already_connected account={}", account_id);
        return;
    }

    // 3. 收集所有 enabled 前置地址 (无 enabled 时拒连, 对齐 MD)
    std::vector<std::string> front_addrs;
    for (const auto& f : cfg->broker.frontends) {
        if (f.enabled && !f.address.empty()) {
            front_addrs.push_back(f.address);
        }
    }
    if (front_addrs.empty()) {
        SPDLOG_WARN("td connect rejected | reason=no_frontends account={}", account_id);
        notify_ui_.error(std::format("账户无前置地址: {}", account_id));
        return;
    }

    // 4. flow_dir: cfg->flow_dir 优先 (用户指定), 否则用 flow_dir_ / account_id (每账户独立)
    std::filesystem::path session_flow_dir;
    if (!cfg->flow_dir.empty()) {
        session_flow_dir = cfg->flow_dir;
    } else {
        session_flow_dir = flow_dir_ / account_id;
    }
    std::error_code ec;
    std::filesystem::create_directories(session_flow_dir, ec);
    if (ec) {
        SPDLOG_ERROR("td connect flow dir create failed | account={} path={} error=\"{}\"",
                     account_id, session_flow_dir.string(), ec.message());
        notify_ui_.error(std::format("流目录创建失败: {}", ec.message()));
        return;
    }

    // 5. 构造 AccountSession 并 open (异常不传播, 仅记日志 + 通知 UI)
    try {
        auto session = std::make_unique<AccountSession>(
            account_id, *order_id_meta_, event_writer_, *persist_writer_,
            event_queue_, timer_queue_);
        session->open(session_flow_dir.string(), front_addrs,
                      cfg->broker.broker_id, cfg->broker.user_id,
                      cfg->broker.password, cfg->auth_code, cfg->app_id);
        sessions_[account_id] = std::move(session);
        SPDLOG_INFO("td account connected | account={} fronts={}", account_id, front_addrs.size());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td connect failed | account={} error=\"{}\"", account_id, e.what());
        notify_ui_.error(std::format("账户连接失败: {}", e.what()));
    }
}

void TdApi::disconnect_account(const shm::FrameView& view) {
    // 设计 §1.1: TD_DISCONNECT 帧的 payload 是 JSON {"account_id":"xxx"}
    // 解析 account_id 后委托给 disconnect_account_by_id (自动调度共用)

    std::string account_id;
    try {
        auto j = decode_ext_inst_json<nlohmann::json>(view);
        account_id = j.value("account_id", "");
    } catch (const std::exception& e) {
        SPDLOG_WARN("td disconnect decode failed | error=\"{}\"", e.what());
        return;
    }
    if (account_id.empty()) {
        SPDLOG_WARN("td disconnect rejected | reason=empty_account_id");
        return;
    }
    disconnect_account_by_id(account_id);
}

void TdApi::disconnect_account_by_id(const std::string& account_id) {
    // find_session -> disconnect() -> erase。无 session 时 no-op。
    auto* session = find_session(account_id);
    if (session == nullptr) {
        SPDLOG_INFO("td disconnect skipped | reason=no_session account={}", account_id);
        return;
    }
    try {
        session->disconnect();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td disconnect failed | account={} error=\"{}\"", account_id, e.what());
    }
    // 契约 account-status: 先推 Offline 帧, 后 erase 缓存与会话
    // (write_account_status 的 Offline 恒推分支保证缓存即使已 Offline 也再推一条)
    write_account_status(account_id, DZ_ACCOUNT_OFFLINE, "");
    account_last_state_.erase(account_id);  // 先推 Offline 再清缓存 (ensure Offline 恒达)
    sessions_.erase(account_id);
    SPDLOG_INFO("td account disconnected | account={}", account_id);
}

void TdApi::on_order_req(const shm::FrameView& view) {
    // 契约 td-order: TD_ORDER_REQ 是 basic 广播帧, payload = DzOrderReq (memcpy 解析, 避免对齐问题),
    // 按 payload account_id 归属路由: 每个 td 网关进程读所有下单帧, 只处理 account_id 在本进程配置中的订单.

    // 1. 校验 frame_size 足够容纳 DzFrameHeader + DzOrderReq (防止截断帧误读)
    constexpr auto kMinFrameSize = sizeof(DzFrameHeader) + sizeof(DzOrderReq);
    if (view.frame_size() < kMinFrameSize) {
        SPDLOG_WARN("td order req rejected | reason=short_payload frame_size={} expected={}",
                    view.frame_size(), sizeof(DzOrderReq));
        return;
    }
    DzOrderReq req;
    std::memcpy(&req, &view.payload<DzOrderReq>(), sizeof(DzOrderReq));

    // 2. account_id 空 = 帧不完整, 拒绝; 不在本进程配置中 = 其他 td 网关实例的账户, 忽略
    if (req.account_id[0] == '\0') {
        SPDLOG_WARN("td order req rejected | reason=empty_account_id order_id={}", req.order_id);
        return;
    }
    if (find_account_in(config_, req.account_id) == nullptr) {
        SPDLOG_DEBUG("td order req ignored | reason=account_not_in_config account={} order_id={}",
                     req.account_id, req.order_id);
        return;
    }

    // 3. find_session, 调用 place_order
    auto* session = find_session(req.account_id);
    if (session == nullptr) {
        SPDLOG_WARN("td order req rejected | reason=no_session account={} order_id={}",
                    req.account_id, req.order_id);
        // 账户已配置但从未连接 (会话懒创建于 connect): 推 REJECTED 回报让策略进程感知,
        // 避免静默丢单 (与 AccountSession::reject_order 的 C3 语义对齐)
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
        copy_string(rpt.remark, "账户未连接", true);
        platform::write_struct(event_writer_, DZ_FRAME_TD_ORDER_RPT, rpt);
        return;
    }
    try {
        session->place_order(req);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td place_order failed | account={} order_id={} error=\"{}\"",
                     req.account_id, req.order_id, e.what());
    }
}

void TdApi::on_cancel_req(const shm::FrameView& view) {
    // 契约 td-order: TD_ORDER_CANCEL_REQ 是 basic 广播帧, payload = DzOrderCancelReq (order_id + account_id),
    // 按 payload account_id 归属路由 (与 on_order_req 相同).

    // 1. 校验 frame_size 足够容纳 DzFrameHeader + DzOrderCancelReq
    constexpr auto kMinFrameSize = sizeof(DzFrameHeader) + sizeof(DzOrderCancelReq);
    if (view.frame_size() < kMinFrameSize) {
        SPDLOG_WARN("td cancel req rejected | reason=short_payload frame_size={} expected={}",
                    view.frame_size(), sizeof(DzOrderCancelReq));
        return;
    }
    DzOrderCancelReq cancel_req;
    std::memcpy(&cancel_req, &view.payload<DzOrderCancelReq>(), sizeof(DzOrderCancelReq));

    // 2. account_id 空 = 帧不完整, 拒绝; 不在本进程配置中 = 其他 td 网关实例的账户, 忽略
    if (cancel_req.account_id[0] == '\0') {
        SPDLOG_WARN("td cancel req rejected | reason=empty_account_id order_id={}",
                    cancel_req.order_id);
        return;
    }
    if (find_account_in(config_, cancel_req.account_id) == nullptr) {
        SPDLOG_DEBUG("td cancel req ignored | reason=account_not_in_config account={} order_id={}",
                     cancel_req.account_id, cancel_req.order_id);
        return;
    }

    // 3. find_session, 调用 cancel_order
    auto* session = find_session(cancel_req.account_id);
    if (session == nullptr) {
        SPDLOG_WARN("td cancel req rejected | reason=no_session account={} order_id={}",
                    cancel_req.account_id, cancel_req.order_id);
        return;
    }
    try {
        bool ok = session->cancel_order(cancel_req.order_id);
        if (!ok) {
            SPDLOG_WARN("td cancel_order returned false | account={} order_id={}",
                        cancel_req.account_id, cancel_req.order_id);
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td cancel_order failed | account={} order_id={} error=\"{}\"",
                     cancel_req.account_id, cancel_req.order_id, e.what());
    }
}

void TdApi::update_status(const std::string& account_id) {
    // 设计 §5.8: 推 DZ_FRAME_RTN_TD_STATUS, payload 为 TdStatus JSON (per-account)
    // instance_id 格式 "name_:account_id" (设计 §1.1), 让 dzweb 按 account_id 路由
    auto* session = find_session(account_id);
    if (session == nullptr) {
        return;
    }
    try {
        std::string inst_id = std::format("{}:{}", name_, account_id);
        const auto& status = session->state_machine().status();
        platform::write_ext_inst_json_obj(event_writer_, DZ_FRAME_TD_RTN_STATUS, inst_id, status);

        // 同步推送 RTN_PROGRESS（per-account, 与 RTN_TD_STATUS 的 progress 字段一致）
        nlohmann::json prog = {{"min", status.progress_min},
                               {"max", status.progress_max},
                               {"current", status.progress_current}};
        if (!status.progress_desc.empty()) {
            prog["desc"] = status.progress_desc;
        }
        platform::write_ext_inst_json_obj(event_writer_, DZ_FRAME_RTN_PROGRESS, inst_id, prog);

        // 契约 account-status: 2018 三态去重推送 (与 2104 同一异常保护, 异常路径一致)
        write_account_status(account_id, account_state_of(status.state), status.trading_day);
    } catch (const dztrader::Exception& e) {
        SPDLOG_ERROR("update_status failed | account={} code={} error=\"{}\"",
                     account_id, e.code(), e.what());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("update_status failed | account={} error=\"{}\"", account_id, e.what());
    }
}

void TdApi::write_account_status(const std::string& account_id, DzAccountState state,
                                 const std::string& trading_day, bool force) {
    auto& last = account_last_state_[account_id];
    if (!force && last == state && state != DZ_ACCOUNT_OFFLINE) {
        return;  // 三态未翻转, 不重复推 (Offline 恒推, 供删账户/崩溃兜底语义)
    }
    last = state;
    DzAccountStatus status{};
    dztrader::copy_string(status.account_id, account_id.c_str(), true);
    dztrader::copy_string(status.gateway_name, name_.c_str(), true);
    status.state = state;
    status.trading_day = 0;
    if (state == DZ_ACCOUNT_READY && !trading_day.empty()) {
        try {
            status.trading_day =
                dztrader::Date::from_string(trading_day, "%Y%m%d").days_since_epoch();
        } catch (const std::exception&) {
            status.trading_day = 0;  // 非法/缺失交易日回落 0 (契约 account-status)
        }
    }
    platform::write_struct(event_writer_, DZ_FRAME_ACCOUNT_STATUS, status);
}

void TdApi::report_account_status_all() {
    for (const auto& acct : config_.accounts) {
        if (auto* session = find_session(acct.account_id); session != nullptr) {
            const auto& st = session->state_machine().status();
            write_account_status(acct.account_id,
                                 account_state_of(session->state_machine().state()),
                                 st.trading_day, /*force=*/true);
        } else {
            write_account_status(acct.account_id, DZ_ACCOUNT_OFFLINE, "", /*force=*/true);
        }
    }
}

void TdApi::handle_query_account_status(const std::byte* frame) {
    const shm::FrameView view(frame);
    constexpr auto kMin = sizeof(DzFrameHeader) + sizeof(DzAccountStatusReq);
    if (view.frame_size() < kMin) {
        SPDLOG_WARN("td query account status rejected | reason=short_payload frame_size={}",
                    view.frame_size());
        return;
    }
    DzAccountStatusReq req;
    std::memcpy(&req, &view.payload<DzAccountStatusReq>(), sizeof(req));
    std::vector<std::string> configured;
    configured.reserve(config_.accounts.size());
    for (const auto& a : config_.accounts) {
        configured.emplace_back(a.account_id);
    }
    if (!account_query_matches(configured, req.account_id)) {
        return;  // 不在本网关配置, 交 master 兜底
    }
    if (req.account_id[0] == '\0') {
        report_account_status_all();
        return;
    }
    if (auto* session = find_session(std::string(req.account_id)); session != nullptr) {
        write_account_status(std::string(req.account_id),
                             account_state_of(session->state_machine().state()),
                             session->state_machine().status().trading_day, /*force=*/true);
    } else {
        write_account_status(std::string(req.account_id), DZ_ACCOUNT_OFFLINE, "", /*force=*/true);
    }
}

void TdApi::broadcast_health(const std::string& account_id, TdHealth now) {
    // 设计 §5.8: per-account 健康度翻转检测 + NOTIFY_TD_CONNECTED/DISCONNECTED 广播
    // 仅在 health 翻转时发送, 避免重复广播。instance_id 格式 "name_:account_id"
    auto& last = account_health_[account_id];  // 不存在则默认插入 TdHealth::Down (=0)
    if (now == last) {
        return;  // 未翻转, 不重复广播
    }
    last = now;
    std::string inst_id = std::format("{}:{}", name_, account_id);
    try {
        if (now == TdHealth::Up) {
            platform::write_ext_inst_raw(event_writer_, DZ_FRAME_NOTIFY_TD_CONNECTED, inst_id);
        } else {
            platform::write_ext_inst_raw(event_writer_, DZ_FRAME_NOTIFY_TD_DISCONNECTED, inst_id);
        }
        SPDLOG_INFO("health broadcast | account={} health={}", account_id,
                    magic_enum::enum_name(now));
    } catch (const std::exception& e) {
        SPDLOG_ERROR("broadcast_health failed | account={} error=\"{}\"", account_id, e.what());
    }
}

void TdApi::report_state() {
    // 设计 §5.8: 对所有 sessions_ 调用 update_status (用于定时上报 / QUERY_ALL 响应)
    for (const auto& [account_id, session] : sessions_) {
        (void)session;  // update_status 内部 find_session, 仅需 account_id
        update_status(account_id);
    }
}

// ============================================================================
// 自动调度 (schedule_auto_sched_timer / on_sched_timer / try_recover_login)
// 实现在 td_api_scheduled.cpp, 参考 md_api_scheduled.cpp
// ============================================================================

}  // namespace dztrader::ctp
