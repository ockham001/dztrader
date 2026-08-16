#include "md/md_state.h"

#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

namespace dztrader::ctp {

// --- 工具函数 (无 CTP 依赖) ---

std::string disconnect_reason_str(int reason) {
    switch (reason) {
        case 0x1001:
            return "network read failed";
        case 0x1002:
            return "network write failed";
        case 0x2001:
            return "heartbeat receive timeout";
        case 0x2002:
            return "heartbeat send failed";
        case 0x2003:
            return "invalid packet received";
        default:
            return std::format("unknown: {}", reason);
    }
}

int32_t trading_day_to_days(const char* s) {
    using namespace std::chrono;
    if (!s || std::strlen(s) < 8) {
        throw std::runtime_error("invalid trading day format");
    }
    auto digit = [](char c) -> int { return (c >= '0' && c <= '9') ? c - '0' : -1; };
    int d0 = digit(s[0]), d1 = digit(s[1]), d2 = digit(s[2]), d3 = digit(s[3]);
    int d4 = digit(s[4]), d5 = digit(s[5]), d6 = digit(s[6]), d7 = digit(s[7]);
    if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0 || d4 < 0 || d5 < 0 || d6 < 0 || d7 < 0) {
        throw std::runtime_error("invalid trading day format");
    }
    int y = (d0 * 1000) + (d1 * 100) + (d2 * 10) + d3;
    int m = (d4 * 10) + d5;
    int d = (d6 * 10) + d7;

    if (m < 1 || m > 12 || d < 1 || d > 31) {
        throw std::runtime_error("invalid trading day format");
    }

    auto ymd = year{y} / month{static_cast<unsigned>(m)} / day{static_cast<unsigned>(d)};
    if (!ymd.ok()) {
        throw std::runtime_error("invalid trading day format");
    }
    return static_cast<int32_t>(sys_days{ymd}.time_since_epoch().count());
}

// --- MdStateMachine ---

MdStateMachine::MdStateMachine() {
    // 初始状态为 Idle, 进度参数与 set_state(Idle) 保持一致
    status_.progress_desc = "未登录";
    status_.progress_max = 4;
}

std::optional<nlohmann::json> MdStateMachine::on_connect() {
    if (status_.state != MdState::Idle) {
        SPDLOG_WARN("connect rejected | state={}", magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(MdState::Connecting);
    return std::nullopt;
}

std::optional<nlohmann::json> MdStateMachine::on_disconnect() {
    if (status_.state == MdState::Idle) {
        return std::nullopt;
    }
    set_state(MdState::Idle);
    return nlohmann::json{{"level", DZ_NOTIFY_INFO}, {"message", "行情已断开"}, {"popup", false}};
}

std::optional<nlohmann::json> MdStateMachine::on_front_connected() {
    if (status_.state != MdState::Connecting && status_.state != MdState::Disconnected) {
        SPDLOG_ERROR("connected: unexpected state | state={}",
                     magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(MdState::Connected);
    return nlohmann::json{{"level", DZ_NOTIFY_INFO}, {"message", "行情连接成功"}, {"popup", false}};
}

std::optional<nlohmann::json> MdStateMachine::on_front_disconnected(int reason) {
    if (status_.state == MdState::Idle) {
        SPDLOG_DEBUG("disconnected in idle, ignoring");
        return std::nullopt;
    }
    set_state(MdState::Disconnected);
    return nlohmann::json{{"level", DZ_NOTIFY_WARN},
                          {"message", "行情断开: " + disconnect_reason_str(reason)},
                          {"popup", false}};
}

std::optional<nlohmann::json> MdStateMachine::on_req_login() {
    if (status_.state != MdState::Connected) {
        SPDLOG_WARN("login req rejected | state={}", magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(MdState::LoggingIn);
    return std::nullopt;
}

std::optional<nlohmann::json> MdStateMachine::on_login_success(const std::string& sys_version,
                                                               const std::string& trading_day,
                                                               const std::string& login_time) {
    if (status_.state != MdState::LoggingIn) {
        SPDLOG_WARN("login ok: unexpected state | state={}", magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    status_.sys_version = sys_version;
    status_.trading_day = trading_day;
    status_.login_time = login_time;
    set_state(MdState::LoggedIn);
    SPDLOG_INFO("login success | sys_version={} trading_day={} login_time={}", sys_version,
                trading_day, login_time);
    return nlohmann::json{{"level", DZ_NOTIFY_INFO}, {"message", "行情登录成功"}, {"popup", false}};
}

std::optional<nlohmann::json> MdStateMachine::on_login_failed() {
    if (status_.state != MdState::LoggingIn) {
        SPDLOG_WARN("login failed: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    // 登录失败, 清空残留的登录时间避免 UI 误显示
    status_.login_time.clear();
    set_state(MdState::Connected);
    SPDLOG_ERROR("login failed | state=LoggingIn");
    return nlohmann::json{{"level", DZ_NOTIFY_ERROR}, {"message", "行情登录失败"}, {"popup", true}};
}

std::optional<nlohmann::json> MdStateMachine::on_login_parse_error() {
    if (status_.state != MdState::LoggingIn) {
        SPDLOG_WARN("login parse error: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    // 登录失败, 清空残留的登录时间避免 UI 误显示
    status_.login_time.clear();
    set_state(MdState::Connected);
    SPDLOG_ERROR("login parse error | state=LoggingIn");
    return nlohmann::json{{"level", DZ_NOTIFY_ERROR}, {"message", "行情登录失败"}, {"popup", true}};
}

std::optional<nlohmann::json> MdStateMachine::on_server_logout() {
    if (status_.state != MdState::LoggedIn) {
        SPDLOG_WARN("server logout: unexpected state | state={}",
                    magic_enum::enum_name(status_.state));
        return std::nullopt;
    }
    set_state(MdState::Connected);
    SPDLOG_WARN("server logout | state=LoggedIn");
    return nlohmann::json{{"level", DZ_NOTIFY_WARN}, {"message", "行情被服务器登出"}, {"popup", true}};
}

void MdStateMachine::set_state(MdState new_state) {
    auto old_state = status_.state;
    if (old_state == new_state) {
        return;
    }
    SPDLOG_INFO("state transition | old={} new={}", magic_enum::enum_name(old_state),
                magic_enum::enum_name(new_state));

    status_.state = new_state;

    // progress_max=4: Idle=0, Connecting/Disconnected=1, Connected=2, LoggingIn=3, LoggedIn=4
    // progress 仅反映连接/登录阶段, 不依赖订阅完成
    // 契约 progress 消费者约定（跨进程稳定契约）: 前端以 current 数值映射驱动登录态判定
    //   (current==max → 登录完成; current==min → 已登出; 其余 → 中间态),
    //   数值映射不可变更; desc 仅为展示文案, 前端不得依赖 desc 做状态判定。
    switch (new_state) {
        case MdState::Idle:
            status_.progress_desc = "未登录";
            status_.progress_min = 0;
            status_.progress_max = 4;
            status_.progress_current = 0;
            status_.login_time.clear();
            status_.sys_version.clear();
            status_.trading_day.clear();
            status_.subscribed_count = 0;
            break;
        case MdState::Connecting:
            status_.progress_desc = "前置连接...";
            status_.progress_min = 0;
            status_.progress_max = 4;
            status_.progress_current = 1;
            break;
        case MdState::Connected:
            status_.progress_desc = "已连接";
            status_.progress_min = 0;
            status_.progress_max = 4;
            status_.progress_current = 2;
            break;
        case MdState::LoggingIn:
            status_.progress_desc = "用户登录...";
            status_.progress_min = 0;
            status_.progress_max = 4;
            status_.progress_current = 3;
            break;
        case MdState::LoggedIn:
            status_.progress_desc = "已登录";
            status_.progress_min = 0;
            status_.progress_max = 4;
            status_.progress_current = 4;
            break;
        case MdState::Disconnected:
            status_.progress_desc = "重连中...";
            status_.progress_min = 0;
            status_.progress_max = 4;
            status_.progress_current = 1;
            status_.subscribed_count = 0;
            break;
    }
}

void MdStateMachine::set_subscription_stats(size_t expected, size_t subscribed) {
    status_.expected_subscribe_count = expected;
    status_.subscribed_count = subscribed;
}

nlohmann::json build_md_status_payload(const MdStatus& s) {
    // 契约 md-status: 仅 6 字段，始终全量。登录/进度状态由 RTN_PROGRESS（契约 progress）覆盖，
    // 本帧不包含状态与进度字段。
    return {
        {"api_version", s.api_version},
        {"sys_version", s.sys_version},
        {"trading_day", s.trading_day},
        {"login_time", s.login_time},
        {"expected_subscribe_count", s.expected_subscribe_count},
        {"subscribed_count", s.subscribed_count},
    };
}

}  // namespace dztrader::ctp
