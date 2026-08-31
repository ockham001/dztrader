#include "td/td_state.h"

#include <format>

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

namespace dztrader::ctp {

// --- TdStateMachine ---

TdStateMachine::TdStateMachine() {
    // 初始状态为 Idle, 进度参数与 set_state(Idle) 保持一致
    status_.progress_desc = "未启动";
    status_.progress_min = 0;
    status_.progress_max = 10;
}

std::optional<TdNotification> TdStateMachine::on_connect() {
    if (status_.state != TdState::Idle) {
        SPDLOG_WARN("connect rejected | state={}", magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(TdState::Connecting);
    return std::nullopt;
}

std::optional<TdNotification> TdStateMachine::on_disconnect() {
    if (status_.state == TdState::Idle) {
        return std::nullopt;
    }
    was_authenticated_ = false;
    set_state(TdState::Idle);
    return TdNotification{.level = DZ_NOTIFY_INFO, .popup = false, .message = "交易已断开"};
}

std::optional<TdNotification> TdStateMachine::on_front_connected() {
    if (status_.state != TdState::Connecting && status_.state != TdState::Disconnected) {
        SPDLOG_ERROR("connected: unexpected state | state={}",
                     magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(TdState::Connected);
    return TdNotification{.level = DZ_NOTIFY_INFO, .popup = false, .message = "交易连接成功"};
}

std::optional<TdNotification> TdStateMachine::on_front_disconnected(int reason) {
    if (status_.state == TdState::Idle) {
        SPDLOG_DEBUG("disconnected in idle, ignoring");
        return std::nullopt;
    }
    if (status_.state == TdState::Disconnected) {
        // 已在 Disconnected 状态, 忽略重复回调 (CTP 重连失败可能多次触发)
        SPDLOG_DEBUG("disconnected repeat callback, ignoring | reason={}", reason);
        return std::nullopt;
    }
    // I11: 重置 was_authenticated_ 标记, 重连后会重新走认证流程
    // 否则重连后若 auth_code 为空直接 req_login 失败, 会错误回退到 Authenticated
    was_authenticated_ = false;
    set_state(TdState::Disconnected);
    // 复用 mdctp 的 disconnect_reason_str (在 md_state.h 声明, md_state.cpp 实现)
    // td_state 不依赖 md_state, 此处内联简单描述
    return TdNotification{.level = DZ_NOTIFY_WARN,
                          .popup = false,
                          .message = std::format("交易断开: reason={}", reason)};
}

std::optional<TdNotification> TdStateMachine::on_connect_timeout() {
    if (status_.state != TdState::Connecting) {
        SPDLOG_DEBUG("connect timeout: unexpected state | state={}",
                     magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    // 连接超时: 从未连上前置, 回退到 Idle (区别于断线回退 Disconnected)
    // 调用方 (TdApi) 负责 cleanup CTP API 并决定是否重试 open()
    set_state(TdState::Idle);
    SPDLOG_ERROR("connect timeout | state=Connecting");
    return TdNotification{.level = DZ_NOTIFY_ERROR, .popup = true, .message = "交易连接超时"};
}

std::optional<TdNotification> TdStateMachine::on_req_authenticate() {
    if (status_.state != TdState::Connected) {
        SPDLOG_WARN("authenticate req rejected | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(TdState::Authenticating);
    return std::nullopt;
}

std::optional<TdNotification> TdStateMachine::on_authenticate_success() {
    if (status_.state != TdState::Authenticating) {
        SPDLOG_WARN("authenticate ok: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    was_authenticated_ = true;
    set_state(TdState::Authenticated);
    return std::nullopt;
}

std::optional<TdNotification> TdStateMachine::on_authenticate_failed() {
    if (status_.state != TdState::Authenticating) {
        SPDLOG_WARN("authenticate failed: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(TdState::Connected);
    SPDLOG_ERROR("authenticate failed | state=Authenticating");
    return TdNotification{.level = DZ_NOTIFY_ERROR, .popup = true, .message = "认证失败"};
}

std::optional<TdNotification> TdStateMachine::on_req_login() {
    if (status_.state != TdState::Connected && status_.state != TdState::Authenticated) {
        SPDLOG_WARN("login req rejected | state={}", magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(TdState::LoggingIn);
    return std::nullopt;
}

std::optional<TdNotification> TdStateMachine::on_login_success(const std::string& sys_version,
                                                                const std::string& trading_day,
                                                                const std::string& login_time) {
    if (status_.state != TdState::LoggingIn) {
        SPDLOG_WARN("login ok: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    status_.sys_version = sys_version;
    status_.trading_day = trading_day;
    status_.login_time = login_time;
    set_state(TdState::LoggedIn);
    SPDLOG_INFO("login success | sys_version={} trading_day={} login_time={}",
                sys_version, trading_day, login_time);
    return TdNotification{.level = DZ_NOTIFY_INFO, .popup = false, .message = "交易登录成功"};
}

std::optional<TdNotification> TdStateMachine::on_login_failed() {
    if (status_.state != TdState::LoggingIn) {
        SPDLOG_WARN("login failed: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    // 登录失败, 清空残留的登录时间避免 UI 误显示
    status_.login_time.clear();
    // 根据是否认证过决定回退目标:
    // - 认证过 -> Authenticated (重新登录, 无需重新认证)
    // - 未认证 -> Connected (无认证路径, 重新走 Connected -> LoggingIn)
    TdState rollback = was_authenticated_ ? TdState::Authenticated : TdState::Connected;
    set_state(rollback);
    SPDLOG_ERROR("login failed | state=LoggingIn rollback={}",
                 magic_enum::enum_name(rollback));
    return TdNotification{.level = DZ_NOTIFY_ERROR, .popup = true, .message = "交易登录失败"};
}

std::optional<TdNotification> TdStateMachine::on_req_settlement_confirm() {
    if (status_.state != TdState::LoggedIn) {
        SPDLOG_WARN("settlement confirm req rejected | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(TdState::Confirming);
    return std::nullopt;
}

std::optional<TdNotification> TdStateMachine::on_settlement_confirmed() {
    if (status_.state != TdState::Confirming) {
        SPDLOG_WARN("settlement confirmed: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(TdState::LoadingInstruments);
    return std::nullopt;
}

std::optional<TdNotification> TdStateMachine::on_settlement_confirm_failed(const std::string& err) {
    if (status_.state != TdState::Confirming) {
        SPDLOG_WARN("settlement confirm failed: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    // C6: 回退到 LoggedIn, 等待下一个调度点重试 (不卡死在 Confirming)
    set_state(TdState::LoggedIn);
    SPDLOG_ERROR("settlement confirm failed | error=\"{}\"", err);
    return TdNotification{
        .level = DZ_NOTIFY_ERROR,
        .popup = true,
        .message = std::format("结算单确认失败: {}", err),
    };
}

std::optional<TdNotification> TdStateMachine::on_instruments_loaded() {
    if (status_.state != TdState::LoadingInstruments) {
        SPDLOG_WARN("instruments loaded: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(TdState::Ready);
    SPDLOG_INFO("instruments loaded, ready to trade");
    return TdNotification{.level = DZ_NOTIFY_INFO, .popup = false, .message = "交易就绪"};
}

std::optional<TdNotification> TdStateMachine::on_instruments_load_failed(const std::string& err) {
    if (status_.state != TdState::LoadingInstruments) {
        SPDLOG_WARN("instruments load failed: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    // 回退到 LoggedIn, 等待重试 (不进入 Ready, 不接受下单)
    set_state(TdState::LoggedIn);
    SPDLOG_ERROR("instruments load failed | error=\"{}\"", err);
    return TdNotification{
        .level = DZ_NOTIFY_ERROR,
        .popup = true,
        .message = std::format("合约加载失败: {}", err),
    };
}

void TdStateMachine::set_state(TdState new_state) {
    auto old_state = status_.state;
    if (old_state == new_state) {
        return;
    }
    SPDLOG_INFO("state transition | old={} new={}", magic_enum::enum_name(old_state),
                magic_enum::enum_name(new_state));

    status_.state = new_state;

    // progress_max=10: Idle=0, Connecting/Disconnected=1, Connected=2,
    // Authenticating=3, Authenticated=4, LoggingIn=5, LoggedIn=6,
    // Confirming=7, LoadingInstruments=8, Ready=10
    switch (new_state) {
        case TdState::Idle:
            status_.progress_desc = "未启动";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 0;
            status_.login_time.clear();
            status_.sys_version.clear();
            status_.trading_day.clear();
            break;
        case TdState::Connecting:
            status_.progress_desc = "前置连接...";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 1;
            break;
        case TdState::Connected:
            status_.progress_desc = "已连接";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 2;
            break;
        case TdState::Authenticating:
            status_.progress_desc = "授权验证...";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 3;
            break;
        case TdState::Authenticated:
            status_.progress_desc = "已授权";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 4;
            break;
        case TdState::LoggingIn:
            status_.progress_desc = "用户登录...";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 5;
            break;
        case TdState::LoggedIn:
            status_.progress_desc = "已登录";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 6;
            break;
        case TdState::Confirming:
            status_.progress_desc = "结算单确认...";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 7;
            break;
        case TdState::LoadingInstruments:
            status_.progress_desc = "加载合约...";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 8;
            break;
        case TdState::Ready:
            status_.progress_desc = "就绪";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 10;
            break;
        case TdState::Disconnected:
            status_.progress_desc = "重连中...";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 1;
            break;
    }
}

DzAccountState account_state_of(TdState state) noexcept {
    switch (state) {
        case TdState::Idle:
        case TdState::Disconnected:
            return DZ_ACCOUNT_OFFLINE;
        case TdState::Connecting:
        case TdState::Connected:
        case TdState::Authenticating:
        case TdState::Authenticated:
        case TdState::LoggingIn:
        case TdState::LoggedIn:
        case TdState::Confirming:
        case TdState::LoadingInstruments:
            return DZ_ACCOUNT_LOGGING_IN;
        case TdState::Ready:
            return DZ_ACCOUNT_READY;
    }
    return DZ_ACCOUNT_OFFLINE;  // 防御: 未枚举值按 Offline
}

}  // namespace dztrader::ctp
