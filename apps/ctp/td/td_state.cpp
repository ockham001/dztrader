#include "td/td_state.h"

#include <ctime>
#include <format>

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

namespace dztrader::ctp {

namespace {

/// 当前 Unix 秒级时间戳（契约 td-status §2.5 last_error.timestamp）
int64_t now_epoch_seconds() { return static_cast<int64_t>(std::time(nullptr)); }

}  // namespace

const char* td_failure_code_str(TdFailureCode code) {
    switch (code) {
        case TdFailureCode::ConnectTimeout:
            return "connect_timeout";
        case TdFailureCode::AuthRejected:
            return "auth_rejected";
        case TdFailureCode::AuthTimeout:
            return "auth_timeout";
        case TdFailureCode::LoginRejected:
            return "login_rejected";
        case TdFailureCode::LoginTimeout:
            return "login_timeout";
        case TdFailureCode::LoginParseError:
            return "login_parse_error";
        case TdFailureCode::DayLoadFailed:
            return "day_load_failed";
    }
    return "unknown";
}

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
    // 新连接/登录尝试被受理: 清空最近一次失败 (契约 td-status §2.5 清空时机).
    // 与 md 的 on_connect 同一口径 —— 前端据此撤下「上次失败」展示.
    status_.last_error.reset();
    set_state(TdState::Connecting);
    return std::nullopt;
}

std::optional<TdNotification> TdStateMachine::on_disconnect() {
    if (status_.state == TdState::Idle) {
        return std::nullopt;
    }
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
    //
    // 例外 (ADR 0059 决策 7): 这是唯一写 last_error 却不进失败态的失败路径。Idle 是 Offline
    // 三态成员、也是唯一允许直接发起登录的端点; 给它失败态会把「从未连上」报成「登录中」,
    // 改变 ACCOUNT_STATUS 对策略侧的语义。原因只能从 last_error.code 读。
    set_last_error(TdFailureCode::ConnectTimeout, std::string(kConnectTimeoutReason), 0);
    set_state(TdState::Idle);
    SPDLOG_ERROR("connect timeout | state=Connecting");
    return TdNotification{.level = DZ_NOTIFY_ERROR, .popup = true, .message = "交易连接超时"};
}

std::optional<TdNotification> TdStateMachine::on_req_authenticate() {
    if (status_.state != TdState::Connected) {
        SPDLOG_WARN("authenticate req rejected | state={}", magic_enum::enum_name(status_.state));
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
    set_state(TdState::Authenticated);
    return std::nullopt;
}

std::optional<TdNotification> TdStateMachine::on_authenticate_failed(TdFailureCode code,
                                                                     const std::string& message,
                                                                     int error_id) {
    if (status_.state != TdState::Authenticating) {
        SPDLOG_WARN("authenticate failed: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    // ADR 0059 决策 1: 停在失败阶段 (AuthFailed, 3/10「认证失败」), 不回退到 Connected (2).
    // 段内原因写 last_error (契约 td-status §2.5); message 空串 = 无详情, 前端按 code 兜底.
    set_last_error(code, message, error_id);
    set_state(TdState::AuthFailed);
    SPDLOG_ERROR("authenticate failed | code={} error_id={}", td_failure_code_str(code), error_id);
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
                                                               const std::string& date,
                                                               const std::string& login_time) {
    if (status_.state != TdState::LoggingIn) {
        SPDLOG_WARN("login ok: unexpected state | state={}", magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    status_.ext.sys_version = sys_version;
    status_.date = date;
    status_.login_time = login_time;
    set_state(TdState::LoggedIn);
    SPDLOG_INFO("login success | sys_version={} date={} login_time={}", sys_version, date,
                login_time);
    return TdNotification{.level = DZ_NOTIFY_INFO, .popup = false, .message = "交易登录成功"};
}

std::optional<TdNotification> TdStateMachine::on_login_failed(TdFailureCode code,
                                                              const std::string& message,
                                                              int error_id) {
    if (status_.state != TdState::LoggingIn) {
        SPDLOG_WARN("login failed: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    // 登录失败, 清空残留的登录时间避免 UI 误显示
    status_.login_time.clear();
    // ADR 0059 决策 1: 停在失败阶段 (LoginFailed, 5/10「登录失败」), 不回退到
    // Authenticated (4) / Connected (2)。失败位置与「是否经过认证」无关 —— 链走到哪一段
    // 就停在哪一段, 不再借用回退来表达失败。
    // 段内原因写 last_error (契约 td-status §2.5); message 空串 = 无详情, 前端按 code 兜底.
    set_last_error(code, message, error_id);
    set_state(TdState::LoginFailed);
    SPDLOG_ERROR("login failed | code={} error_id={}", td_failure_code_str(code), error_id);
    // message 保留通用文案: AccountSession 侧改用 login_failed_message() 构造带场景的正文,
    // 只取本结构的 level/popup (#407).
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
    // C6: 回退到 LoggedIn (不卡死在 Confirming). 回退后本进程不自动重试该账户
    // (#406); 恢复路径 = 先登出再登录 (ADR 0051 决策 4). 失败原因由调用方上浮 UI (#488).
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
    // 转 Ready = 本次会话建立成功: 清空最近一次失败 (契约 td-status §2.5 清空时机).
    status_.last_error.reset();
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
    // 回退到 LoggedIn (不进入 Ready, 不接受下单). 同样不自动重试 (#406); 恢复路径 =
    // 先登出再登录. 失败原因由调用方上浮 UI (#488).
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
    // Authenticating/AuthFailed=3, Authenticated=4, LoggingIn/LoginFailed=5, LoggedIn=6,
    // Confirming=7, LoadingInstruments=8, Ready=10
    // 失败态取**它停住的那一段**的数值 (ADR 0059 决策 1): 不回退到上一段, 也不占用端点.
    switch (new_state) {
        case TdState::Idle:
            status_.progress_desc = "未启动";
            status_.progress_min = 0;
            status_.progress_max = 10;
            status_.progress_current = 0;
            status_.login_time.clear();
            status_.ext.sys_version.clear();
            status_.date.clear();
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
        case TdState::AuthFailed:
            // 与 Authenticating 同格 (3/10): 停在失败发生的那一段, 不回退到 Connected (2).
            status_.progress_desc = "认证失败";
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
        case TdState::LoginFailed:
            // 与 LoggingIn 同格 (5/10): 停在失败发生的那一段, 不回退到 Authenticated (4).
            status_.progress_desc = "登录失败";
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
        case TdState::AuthFailed:  // 失败态属会话未就绪 (契约 account-status 三态聚合)
        case TdState::Authenticated:
        case TdState::LoggingIn:
        case TdState::LoginFailed:  // 失败态属会话未就绪 (契约 account-status 三态聚合)
        case TdState::LoggedIn:
        case TdState::Confirming:
        case TdState::LoadingInstruments:
            return DZ_ACCOUNT_LOGGING_IN;
        case TdState::Ready:
            return DZ_ACCOUNT_READY;
    }
    return DZ_ACCOUNT_OFFLINE;  // 防御: 未枚举值按 Offline
}

void TdStateMachine::set_last_error(TdFailureCode code, const std::string& message, int error_id) {
    status_.last_error =
        TdLastError{td_failure_code_str(code), message, error_id, now_epoch_seconds()};
}

nlohmann::json td_status_payload(const TdStatus& status, const UnknownEnumCounts& counts) {
    // 契约 td-status §2.3: 未知枚举计数挂在 ext 下按种类分开 (原因与处置都不同, 见 #367),
    // 与状态机自身的字段互不覆盖 —— 状态机不感知计数器, 拼接只在写帧这一处发生。
    // 键名刻意不复用 td_ctp_mapping.h 的 kUnknownCategory*: 该头 include CTP 头
    // (ThostFtdcUserApiStruct.h), 在 td_state.cpp 引入会把 CTP 头拖进本不链 CTP 的测试目标
    // (td_state.h 的设计说明即为此). 两份字符串必须逐字相同, 改动须同步 mapping 头的常量.
    // last_error 由 NLOHMANN 宏随 status 一起带出 (std::optional → null)，无需在此拼接。
    nlohmann::json payload = status;
    payload["ext"]["unknown_order_status"] = counts.order_status;
    payload["ext"]["unknown_direction"] = counts.direction;
    payload["ext"]["unknown_price_type"] = counts.price_type;
    payload["ext"]["unknown_position_effect"] = counts.position_effect;
    // 契约 td-status §2.5: last_error 随 NLOHMANN 宏带出, 无未清空失败时为 null
    // (std::optional 的默认序列化; 不是空对象、也不是省略键 —— 前端用「非 null +
    // timestamp 变化」作登录 pending 的清除信号, 缺键与 null 必须同义).
    return payload;
}

}  // namespace dztrader::ctp
