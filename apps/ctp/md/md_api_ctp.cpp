#include "md/md_api.h"

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>
#include <ThostFtdcMdApi.h>

#include <dztrader/data_type.h>
#include <dztrader/struct.h>
#include <dztrader/core/encoding.h>
#include <dztrader/core/string_util.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/log/log.h>

namespace dztrader::ctp {

using namespace dztrader::shm;

// md_api_ctp.cpp: CTP 接口实现
// - 连接管理: connect / disconnect
// - 登录: req_user_login / try_recover_login
// - CTP SPI 回调 (保留 on_* 前缀, CTP 接口约定):
//   on_front_connected / on_front_disconnected / on_heartbeat_warning
//   on_rsp_user_login / on_rsp_user_logout / on_rsp_error

void MdApi::connect() {
    if (state_machine_.state() != MdState::Idle) {
        SPDLOG_WARN("connect rejected | state={}", magic_enum::enum_name(state_machine_.state()));
        return;
    }

    if (ctp_md_config_.config().current_broker_name.empty()) {
        SPDLOG_WARN("connect rejected | reason=\"no_current_broker\"");
        return;
    }

    const dztrader::platform::CtpBrokerEntry* current_broker = find_current_broker();
    if (current_broker == nullptr) {
        SPDLOG_WARN("connect rejected | reason=\"current_broker_not_found\" value=\"{}\"",
                    ctp_md_config_.config().current_broker_name);
        return;
    }

    // 检查是否有 enabled 前置（在创建 api 之前，避免创建后无前置可注册）
    bool has_enabled = false;
    for (const auto& fe : current_broker->frontends) {
        if (fe.enabled) { has_enabled = true; break; }
    }
    if (!has_enabled) {
        SPDLOG_WARN("connect rejected | reason=\"no_enabled_frontend\" broker=\"{}\"",
                    current_broker->name);
        return;
    }

    api_ = CThostFtdcMdApi::CreateFtdcMdApi(flow_dir_.string().c_str());
    if (api_ == nullptr) {
        SPDLOG_ERROR("create api failed");
        return;
    }
    api_->RegisterSpi(&spi_);
    // 注册所有 enabled 前置地址（CTP 支持多前置自动故障切换）
    for (const auto& fe : current_broker->frontends) {
        if (!fe.enabled) continue;
        std::string front_addr = fe.address;
        if (!front_addr.starts_with("tcp://") && !front_addr.starts_with("ssl://") &&
            !front_addr.starts_with("socks")) {
            front_addr = "tcp://" + front_addr;
        }
        api_->RegisterFront(const_cast<char*>(front_addr.c_str()));  // NOLINT
    }
    api_->Init();
    state_machine_.on_connect();
    report_progress();
    // 30s 连接超时: 未收到 OnFrontConnected 则自动 disconnect 回 Idle
    connect_timer_id_ = timer_queue_.schedule_after_replace(
        "connect_timeout", std::chrono::seconds(30), [this]() {
            if (state_machine_.state() == MdState::Connecting) {
                SPDLOG_WARN("connect timeout, disconnecting | timeout=30s");
                disconnect();
            }
        });
}

void MdApi::cancel_login_timer() {
    if (login_timer_id_ != 0) {
        timer_queue_.cancel(login_timer_id_);
        login_timer_id_ = 0;
    }
}

void MdApi::req_user_login() {
    if (api_ == nullptr) {
        SPDLOG_WARN("login api is null");
        return;
    }
    if (state_machine_.state() != MdState::Connected) {
        SPDLOG_WARN("login rejected | state={}", magic_enum::enum_name(state_machine_.state()));
        return;
    }

    if (ctp_md_config_.config().current_broker_name.empty()) {
        SPDLOG_WARN("login rejected | reason=\"no_current_broker\"");
        return;
    }

    const dztrader::platform::CtpBrokerEntry* current_broker = find_current_broker();
    if (current_broker == nullptr) {
        SPDLOG_WARN("login rejected | reason=\"current_broker_not_found\" value=\"{}\"",
                    ctp_md_config_.config().current_broker_name);
        return;
    }

    CThostFtdcReqUserLoginField req{};
    copy_string(req.BrokerID, current_broker->broker_id.c_str(), true);
    copy_string(req.UserID, current_broker->user_id.c_str(), true);
    copy_string(req.Password, current_broker->password.c_str(), true);

    ++request_id_;
    auto ret = api_->ReqUserLogin(&req, request_id_);
    if (ret != 0) {
        SPDLOG_WARN("login req failed | ret={} request_id={} user_id={}", ret, request_id_,
                    req.UserID);
        // 状态未转移, 仍在 Connected, 可由自动调度重试
        return;
    }
    // ReqUserLogin 成功后才转移状态, 避免失败时永久 stuck LoggingIn
    state_machine_.on_req_login();
    // 登录超时: 10s 内未收到 OnRspUserLogin 则回退到 Connected
    login_timer_id_ = timer_queue_.schedule_after_replace(
        "login_timeout", std::chrono::seconds(10), [this]() {
            if (state_machine_.state() == MdState::LoggingIn) {
                SPDLOG_WARN("login timeout, fallback to connected | timeout=10s");
                if (auto n = state_machine_.on_login_failed()) notify_ui(*n);
                report_md_status();
                report_progress();
            }
        });
    report_progress();
    SPDLOG_DEBUG("login req sent | request_id={} user_id={}", request_id_, req.UserID);
}

void MdApi::on_front_connected(const OnFrontConnectedField& rsp [[maybe_unused]]) {
    if (connect_timer_id_ != 0) {
        timer_queue_.cancel(connect_timer_id_);
        connect_timer_id_ = 0;
    }
    SPDLOG_INFO("connected");
    if (auto n = state_machine_.on_front_connected()) notify_ui(*n);
    report_progress();
    req_user_login();
}

// ============================================================================
// 会话断开的三种路径
//
// 三个函数都执行"清理订阅链路 + report_md_status() + report_progress()",
// 但触发场景和清理范围不同, 修改时务必同步检查三者一致性:
//
// - on_front_disconnected: CTP 前置网络断开 (OnFrontDisconnected 回调)。
//   不 Release API (CTP SDK 自动重连)。状态 -> Disconnected。
//   需取消 connect/login/sub 三个定时器 (断线可能发生在任意阶段)。
//   sub_manager_.on_session_lost(): 重置 sub_state + 清零 count, 不清理条目。
//
// - disconnect: 主动断开 (UI 登出按钮 / 自动调度 Logout)。
//   强制 Release API + 状态 -> Idle。可重复调用 (幂等)。
//   需取消 connect/login/sub 定时器, Release API。
//   sub_manager_.on_idle(): 重置 sub_state + 清理空订阅者条目 + 清零 count。
//
// - on_rsp_user_logout: CTP 服务器主动登出 (OnRspUserLogout 回调)。
//   不 Release API (前置仍连接, on_sched_timer 可经 req_user_login 恢复)。
//   状态 -> Connected (非 Idle), set_state(Connected) 不触碰订阅状态,
//   故需 sub_manager_.on_session_lost() 重置 sub_state + 清零 count。
//   不取消 connect/login 定时器 (服务器登出时前置存活, 二者不可能挂起)。
//
// 共同的订阅链路清理 (5 项, 三处一致, 修改时务必同步):
//   pending_batches_.clear() / sub_check_active_=false / sub_retry_count_=0 /
//   cancel(sub_timer_id_) / ++sub_generation_
//
// 保留不触碰的定时器: auto_sched / md_shm_maint / event_shm_maint
// ============================================================================

void MdApi::on_front_disconnected(const OnFrontDisconnectedField& rsp) {
    SPDLOG_WARN("disconnected | reason={} desc={}", rsp.reason, disconnect_reason_str(rsp.reason));
    if (auto n = state_machine_.on_front_disconnected(rsp.reason)) notify_ui(*n);
    sub_manager_.on_session_lost();
    // 清理批次队列, 重置补订链路状态, 取消挂起的定时器并使回调失效
    pending_batches_.clear();
    sub_check_active_ = false;
    sub_retry_count_ = 0;
    if (sub_timer_id_ != 0) {
        timer_queue_.cancel(sub_timer_id_);
        sub_timer_id_ = 0;
        SPDLOG_DEBUG("sub timer cancelled | on disconnect");
    }
    cancel_login_timer();  // 断线时取消可能挂起的登录超时
    // 取消可能挂起的连接超时 (Connecting -> Disconnected 场景)
    if (connect_timer_id_ != 0) {
        timer_queue_.cancel(connect_timer_id_);
        connect_timer_id_ = 0;
        SPDLOG_DEBUG("connect timer cancelled | on disconnect");
    }
    ++sub_generation_;  // 双保险: 使 cancel 前已被 tick 取出的回调也失效
    report_md_status();
    report_progress();
}

void MdApi::disconnect() {
    if (connect_timer_id_ != 0) {
        timer_queue_.cancel(connect_timer_id_);
        connect_timer_id_ = 0;
    }
    cancel_login_timer();                    // 取消可能挂起的登录超时
    if (api_ != nullptr) {
        api_->Release();
        api_ = nullptr;
    }
    if (auto n = state_machine_.on_disconnect()) notify_ui(*n);
    sub_manager_.on_idle();
    // 清理订阅链路 (与 on_front_disconnected 一致), 保留 auto_sched/md_shm_maint 等无关定时器
    pending_batches_.clear();
    sub_check_active_ = false;
    sub_retry_count_ = 0;
    if (sub_timer_id_ != 0) {
        timer_queue_.cancel(sub_timer_id_);
        sub_timer_id_ = 0;
    }
    ++sub_generation_;  // 双保险: 使 cancel 前已被 tick 取出的回调也失效
    report_md_status();
    report_progress();
}

void MdApi::on_rsp_user_logout(const OnRspUserLogoutField& rsp) {
    if (!rsp.rsp_info.has_value()) {
        SPDLOG_ERROR("logout rsp_info missing");
        return;
    }
    if (rsp.rsp_info->ErrorID != 0) {
        SPDLOG_ERROR("logout rejected | error_id={} error_msg=\"{}\"", rsp.rsp_info->ErrorID,
                     dztrader::to_utf8_from_gbk(rsp.rsp_info->ErrorMsg));
        return;
    }
    SPDLOG_INFO("logout ok");
    if (auto n = state_machine_.on_server_logout()) notify_ui(*n);
    sub_manager_.on_session_lost();
    // 服务器登出后会话失效, 订阅全部无效, 清理订阅链路 (与 on_front_disconnected 一致),
    // 保留 auto_sched/md_shm_maint 等无关定时器
    pending_batches_.clear();
    sub_check_active_ = false;
    sub_retry_count_ = 0;
    if (sub_timer_id_ != 0) {
        timer_queue_.cancel(sub_timer_id_);
        sub_timer_id_ = 0;
    }
    ++sub_generation_;  // 双保险: 使 cancel 前已被 tick 取出的回调也失效
    report_md_status();
    report_progress();
}

void MdApi::on_heartbeat_warning(const OnHeartBeatWarningField& rsp) {
    // CTP 官方称此回调"暂不启用"。保持仅日志, 不重置 sub_state。
    // 真正的断线由 OnFrontDisconnected 兜底处理, 会重置所有 sub_state 并触发重订阅。
    SPDLOG_WARN("heartbeat | time_lapse={}", rsp.time_lapse);
}

void MdApi::on_rsp_user_login(const OnRspUserLoginField& rsp) {
    if (state_machine_.state() != MdState::LoggingIn) {
        SPDLOG_WARN("login unexpected state | state={}",
                    magic_enum::enum_name(state_machine_.state()));
        return;
    }
    cancel_login_timer();  // 收到任何登录响应, 取消超时
    if (!rsp.rsp_info.has_value()) {
        SPDLOG_ERROR("login rsp_info missing, fallback to connected");
        if (auto n = state_machine_.on_login_failed()) notify_ui(*n);
        report_md_status();
        report_progress();
        return;
    }
    if (rsp.rsp_info->ErrorID != 0) {
        SPDLOG_ERROR("login rejected | error_id={} error_msg=\"{}\"", rsp.rsp_info->ErrorID,
                     dztrader::to_utf8_from_gbk(rsp.rsp_info->ErrorMsg));
        if (auto n = state_machine_.on_login_failed()) notify_ui(*n);
        report_md_status();
        report_progress();
        return;
    }
    if (!rsp.rsp_user_login.has_value()) {
        SPDLOG_ERROR("login rsp_user_login missing, fallback to connected");
        if (auto n = state_machine_.on_login_failed()) notify_ui(*n);
        report_md_status();
        report_progress();
        return;
    }
    if (rsp.trading_day_parse_error.has_value()) {
        SPDLOG_ERROR(
            "login trading day parse failed | trading_day={} days_since_epoch={} error=\"{}\"",
            rsp.rsp_user_login->TradingDay, rsp.days_since_epoch,
            rsp.trading_day_parse_error.value());
        if (auto n = state_machine_.on_login_parse_error()) notify_ui(*n);
        report_md_status();
        report_progress();
        return;
    }

    std::string login_time;
    try {
        login_time = DateTime::system_to_local(
            DateTime::from_timestamp(std::chrono::system_clock::to_time_t(rsp.rsp_time)))
            .to_string();
    } catch (const std::exception& e) {
        SPDLOG_WARN("login time parse failed, use empty | error=\"{}\"", e.what());
        // login_time 保持空串, 继续执行 on_login_success 确保状态转移
    }

    if (auto n = state_machine_.on_login_success(rsp.rsp_user_login->SysVersion,
                                                 rsp.rsp_user_login->TradingDay, login_time)) {
        notify_ui(*n);
    }

    SPDLOG_INFO("login ok | trading_day={} days_since_epoch={}", rsp.rsp_user_login->TradingDay,
                rsp.days_since_epoch);

    // 订阅链路: on_logged_in 内部完成清孤儿 -> 全量重置 -> 收集重订
    auto plan = sub_manager_.on_logged_in();
    if (!plan.to_unsubscribe.empty()) {
        SPDLOG_INFO("orphan cleanup | unsub_count={}", plan.to_unsubscribe.size());
        batch_unsubscribe(plan.to_unsubscribe);
    }
    pending_batches_.clear();
    sub_check_active_ = false;
    sub_retry_count_ = 0;
    if (sub_timer_id_ != 0) {
        timer_queue_.cancel(sub_timer_id_);
        sub_timer_id_ = 0;
        SPDLOG_DEBUG("sub timer cancelled | on relogin");
    }
    ++sub_generation_;
    if (!plan.to_resubscribe.empty()) {
        sub_manager_.mark_pending(plan.to_resubscribe);
        enqueue_batches(std::move(plan.to_resubscribe));
        if (!sub_check_active_) {
            sub_check_active_ = true;
            send_next_batch();
        }
    }
    SPDLOG_INFO("relogin sub done | orphan_unsub={} resub={} expected={}",
                plan.to_unsubscribe.size(), plan.to_resubscribe.size(),
                sub_manager_.expected_subscribe_count());
    report_md_status();
    report_progress();
}

void MdApi::on_rsp_error(const OnRspErrorField& rsp) {
    if (!rsp.rsp_info.has_value()) {
        SPDLOG_ERROR("error rsp rsp_info missing");
        return;
    }
    if (rsp.rsp_info->ErrorID != 0) {
        SPDLOG_ERROR("error rsp | error_id={} error_msg=\"{}\"", rsp.rsp_info->ErrorID,
                     dztrader::to_utf8_from_gbk(rsp.rsp_info->ErrorMsg));
    }
}

void MdApi::try_recover_login() {
    auto now = DateTime::local_now();
    auto hh_mm = now.to_string("%H:%M");
    int weekday = static_cast<int>(now.weekday());

    // 星期过滤
    if (!is_trading_window(weekday, hh_mm)) {
        SPDLOG_INFO("recover skipped | reason=\"non_trading_window\" time={} weekday={}", hh_mm,
                    weekday);
        return;
    }

    // 会话区间判断（排程单一真相源：auto_login_config_）
    auto sched = to_sched_view(auto_login_config_.config());
    if (!should_be_logged_in(sched, hh_mm)) {
        SPDLOG_INFO("recover skipped | reason=\"not_in_session\" time={}", hh_mm);
        return;
    }

    // 当前已登录则不重复
    if (state_machine_.state() != MdState::Idle) {
        SPDLOG_INFO("recover skipped | reason=\"not_idle\" state={}",
                    magic_enum::enum_name(state_machine_.state()));
        return;
    }

    SPDLOG_INFO("recover login | time={} weekday={}", hh_mm, weekday);
    connect();
}

}  // namespace dztrader::ctp
