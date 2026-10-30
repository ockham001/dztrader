#include <gtest/gtest.h>
#include <ctime>
#include <optional>
#include <string>

#include "md/md_state.h"

using namespace dztrader::ctp;

class MdStateMachineTest : public ::testing::Test {
protected:
    MdStateMachine sm;
};

// --- State Transitions ---

TEST_F(MdStateMachineTest, InitialStateIsIdle) {
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_EQ(sm.status().progress_max, 4);
    EXPECT_EQ(sm.status().progress_current, 0);
    EXPECT_EQ(sm.status().progress_desc, "未登录");
}

TEST_F(MdStateMachineTest, ConnectFromIdleTransitionsToConnecting) {
    auto notif = sm.on_connect();
    EXPECT_EQ(sm.state(), MdState::Connecting);
    EXPECT_FALSE(notif.has_value());
    EXPECT_EQ(sm.status().progress_desc, "前置连接...");
    EXPECT_EQ(sm.status().progress_max, 4);
    EXPECT_EQ(sm.status().progress_current, 1);
}

TEST_F(MdStateMachineTest, ConnectFromNonIdleRejected) {
    sm.on_connect();               // 现处于 Connecting
    auto notif = sm.on_connect();  // 被拒绝
    EXPECT_EQ(sm.state(), MdState::Connecting);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(MdStateMachineTest, FrontConnectedFromConnectingTransitionsToConnected) {
    sm.on_connect();
    auto notif = sm.on_front_connected();
    EXPECT_EQ(sm.state(), MdState::Connected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情连接成功");
    EXPECT_EQ((*notif)["level"].get<int>(), DZ_NOTIFY_INFO);
    EXPECT_FALSE((*notif)["popup"].get<bool>());
    EXPECT_EQ(sm.status().progress_current, 2);
    EXPECT_EQ(sm.status().progress_desc, "已连接");
}

TEST_F(MdStateMachineTest, FrontConnectedFromDisconnectedTransitionsToConnected) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_success("v", "d", "t");
    sm.on_front_disconnected(0x1001);
    EXPECT_EQ(sm.state(), MdState::Disconnected);

    auto notif = sm.on_front_connected();
    EXPECT_EQ(sm.state(), MdState::Connected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情连接成功");
}

TEST_F(MdStateMachineTest, FrontConnectedFromUnexpectedStateIgnored) {
    // Idle 状态下 on_front_connected 应被忽略
    auto notif = sm.on_front_connected();
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(MdStateMachineTest, DisconnectFromAnyStateTransitionsToIdle) {
    sm.on_connect();
    sm.on_front_connected();
    auto notif = sm.on_disconnect();
    EXPECT_EQ(sm.state(), MdState::Idle);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情已断开");
    EXPECT_EQ((*notif)["level"].get<int>(), DZ_NOTIFY_INFO);
    EXPECT_FALSE((*notif)["popup"].get<bool>());
}

TEST_F(MdStateMachineTest, DisconnectFromIdleReturnsNullopt) {
    auto notif = sm.on_disconnect();
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(MdStateMachineTest, FrontDisconnectedFromNonIdleTransitionsToDisconnected) {
    sm.on_connect();
    sm.on_front_connected();
    auto notif = sm.on_front_disconnected(0x1001);
    EXPECT_EQ(sm.state(), MdState::Disconnected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["level"].get<int>(), DZ_NOTIFY_WARN);
    EXPECT_FALSE((*notif)["popup"].get<bool>());
    EXPECT_TRUE((*notif)["message"].get<std::string>().find("行情断开:") != std::string::npos);
}

TEST_F(MdStateMachineTest, FrontDisconnectedFromIdleIgnored) {
    auto notif = sm.on_front_disconnected(0x1001);
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(MdStateMachineTest, ReconnectFlow) {
    // 完整流程: 连接 → 登录 → 断开 → 重连
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_success("v6.7", "20260523", "09:00:00");
    EXPECT_EQ(sm.state(), MdState::LoggedIn);
    sm.set_subscription_stats(5, 5);

    // 断开连接：契约要求三个字符串与 expected_subscribe_count 保留，
    // subscribed_count 归零（重连成功后由新登录响应覆盖元信息）。
    sm.on_front_disconnected(0x1001);
    EXPECT_EQ(sm.state(), MdState::Disconnected);
    EXPECT_EQ(sm.status().progress_desc, "重连中...");
    EXPECT_EQ(sm.status().sys_version, "v6.7");
    EXPECT_EQ(sm.status().trading_day, "20260523");
    EXPECT_EQ(sm.status().login_time, "09:00:00");
    EXPECT_EQ(sm.status().expected_subscribe_count, 5u);
    EXPECT_EQ(sm.status().subscribed_count, 0u);

    // 重连 (CTP 自动重连)
    auto notif = sm.on_front_connected();
    EXPECT_EQ(sm.state(), MdState::Connected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情连接成功");
    EXPECT_EQ(sm.status().sys_version, "v6.7");
    EXPECT_EQ(sm.status().trading_day, "20260523");
    EXPECT_EQ(sm.status().login_time, "09:00:00");
}

TEST_F(MdStateMachineTest, ReqLoginFromConnectedTransitionsToLoggingIn) {
    sm.on_connect();
    sm.on_front_connected();
    EXPECT_EQ(sm.state(), MdState::Connected);

    auto notif = sm.on_req_login();
    EXPECT_EQ(sm.state(), MdState::LoggingIn);
    EXPECT_FALSE(notif.has_value());
    EXPECT_EQ(sm.status().progress_desc, "用户登录...");
    EXPECT_EQ(sm.status().progress_current, 3);
}

TEST_F(MdStateMachineTest, ReqLoginFromNonConnectedRejected) {
    auto notif = sm.on_req_login();
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(MdStateMachineTest, LoginSuccessTransitionsToLoggedIn) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    auto notif = sm.on_login_success("v6.7", "20260523", "09:00:00");
    EXPECT_EQ(sm.state(), MdState::LoggedIn);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情登录成功");
    EXPECT_EQ((*notif)["level"].get<int>(), DZ_NOTIFY_INFO);
    EXPECT_FALSE((*notif)["popup"].get<bool>());
    EXPECT_EQ(sm.status().sys_version, "v6.7");
    EXPECT_EQ(sm.status().trading_day, "20260523");
    EXPECT_EQ(sm.status().login_time, "09:00:00");
    EXPECT_EQ(sm.status().progress_current, 4);
    EXPECT_EQ(sm.status().progress_desc, "已登录");
}

TEST_F(MdStateMachineTest, LoginRejectedTransitionsToLoginFailedWithReason) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_success("v6.7", "20260523", "09:00:00");
    // 先造出非空 login_time：断线重连后再登录失败，验证失败分支会清空它。
    sm.on_front_disconnected(0x1001);
    sm.on_front_connected();
    sm.on_req_login();
    ASSERT_FALSE(sm.status().login_time.empty());

    // 契约 md-status 失败语义: CTP 拒绝登录（ErrorID/ErrorMsg 进 last_error 与通知）
    auto notif = sm.on_login_failed(MdFailureCode::LoginRejected, "CTP:不合法的登录", 3);
    EXPECT_EQ(sm.state(), MdState::LoginFailed);
    EXPECT_TRUE(sm.status().login_time.empty());
    EXPECT_EQ(sm.status().progress_desc, "登录失败");
    EXPECT_EQ(sm.status().progress_current, 2);
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "login_rejected");
    EXPECT_EQ(sm.status().last_error->message, "CTP:不合法的登录");
    EXPECT_EQ(sm.status().last_error->error_id, 3);
    EXPECT_GT(sm.status().last_error->timestamp, 0);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情登录失败: CTP:不合法的登录");
    EXPECT_EQ((*notif)["level"].get<int>(), DZ_NOTIFY_ERROR);
    EXPECT_TRUE((*notif)["popup"].get<bool>());
}

TEST_F(MdStateMachineTest, LoginRejectedWithoutMessageFallsBackToErrorId) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();

    auto notif = sm.on_login_failed(MdFailureCode::LoginRejected, "", 7);
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->message, "错误码 7");
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情登录失败: 错误码 7");
}

TEST_F(MdStateMachineTest, LoginTimeoutRecordsReason) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();

    auto notif = sm.on_login_failed(MdFailureCode::LoginTimeout, "10 秒内未收到登录响应");
    EXPECT_EQ(sm.state(), MdState::LoginFailed);
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "login_timeout");
    EXPECT_EQ(sm.status().last_error->error_id, 0);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情登录超时: 10 秒内未收到登录响应");
}

TEST_F(MdStateMachineTest, LoginParseErrorTransitionsToLoginFailed) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_success("v6.7", "20260523", "09:00:00");
    // 解析失败同样必须清空过期登录时间。
    sm.on_front_disconnected(0x1001);
    sm.on_front_connected();
    sm.on_req_login();
    ASSERT_FALSE(sm.status().login_time.empty());

    auto notif = sm.on_login_parse_error();
    EXPECT_EQ(sm.state(), MdState::LoginFailed);
    EXPECT_TRUE(sm.status().login_time.empty());
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "login_parse_error");
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情登录失败: 登录响应解析失败");
    EXPECT_EQ((*notif)["level"].get<int>(), DZ_NOTIFY_ERROR);
    EXPECT_TRUE((*notif)["popup"].get<bool>());
}

TEST_F(MdStateMachineTest, LoginFailedAllowsDirectRetry) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    ASSERT_TRUE(
        sm.on_login_failed(MdFailureCode::LoginRejected, "CTP:不合法的登录", 3).has_value());
    EXPECT_EQ(sm.state(), MdState::LoginFailed);
    ASSERT_TRUE(sm.status().last_error.has_value());

    // 契约 md-status 失败语义: 失败态可直接重试登录（无需先登出），重试开始即清空 last_error
    auto notif = sm.on_req_login();
    EXPECT_EQ(sm.state(), MdState::LoggingIn);
    EXPECT_FALSE(notif.has_value());
    EXPECT_FALSE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().progress_desc, "用户登录...");
}

TEST_F(MdStateMachineTest, NewConnectAttemptClearsLastError) {
    sm.on_connect();
    ASSERT_TRUE(sm.on_connect_timeout().has_value());
    ASSERT_TRUE(sm.status().last_error.has_value());

    // 下一次连接尝试开始（Idle -> Connecting）清空最近一次失败
    sm.on_connect();
    EXPECT_FALSE(sm.status().last_error.has_value());
}

TEST_F(MdStateMachineTest, LoginSuccessClearsLastError) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    ASSERT_TRUE(
        sm.on_login_failed(MdFailureCode::LoginTimeout, "10 秒内未收到登录响应").has_value());
    ASSERT_TRUE(sm.status().last_error.has_value());

    sm.on_req_login();
    sm.on_login_success("v", "d", "t");
    EXPECT_FALSE(sm.status().last_error.has_value());
}

TEST_F(MdStateMachineTest, ConnectTimeoutRecordsReasonAndReturnsToIdle) {
    sm.on_connect();
    EXPECT_EQ(sm.state(), MdState::Connecting);

    auto notif = sm.on_connect_timeout();
    EXPECT_EQ(sm.state(), MdState::Idle);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["level"].get<int>(), DZ_NOTIFY_WARN);
    EXPECT_FALSE((*notif)["popup"].get<bool>());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情连接超时: 30 秒内未连接上前置");
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "connect_timeout");
    EXPECT_EQ(sm.status().last_error->error_id, 0);
    EXPECT_GT(sm.status().last_error->timestamp, 0);
    EXPECT_EQ(sm.status().progress_current, 0);
}

TEST_F(MdStateMachineTest, ConnectTimeoutFromNonConnectingIgnored) {
    // Idle 状态（未发起连接）收到超时回调: 不记录失败、不转移状态
    auto notif = sm.on_connect_timeout();
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_FALSE(notif.has_value());
    EXPECT_FALSE(sm.status().last_error.has_value());
}

TEST_F(MdStateMachineTest, LoginFromUnexpectedStateIgnored) {
    auto notif = sm.on_login_success("v", "d", "t");
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(MdStateMachineTest, ServerLogoutTransitionsToConnected) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_success("v", "d", "t");
    auto notif = sm.on_server_logout();
    EXPECT_EQ(sm.state(), MdState::Connected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情被服务器登出");
    EXPECT_EQ((*notif)["level"].get<int>(), DZ_NOTIFY_WARN);
    EXPECT_TRUE((*notif)["popup"].get<bool>());
}

TEST_F(MdStateMachineTest, ServerLogoutFromNonLoggedInRejected) {
    sm.on_connect();
    sm.on_front_connected();
    auto notif = sm.on_server_logout();
    EXPECT_EQ(sm.state(), MdState::Connected);  // state unchanged
    EXPECT_FALSE(notif.has_value());
}

// --- Progress Mapping ---

TEST_F(MdStateMachineTest, ProgressMappingIdle) {
    EXPECT_EQ(sm.status().progress_min, 0);
    EXPECT_EQ(sm.status().progress_max, 4);
    EXPECT_EQ(sm.status().progress_current, 0);
}

TEST_F(MdStateMachineTest, ProgressMappingConnecting) {
    sm.on_connect();
    EXPECT_EQ(sm.status().progress_desc, "前置连接...");
    EXPECT_EQ(sm.status().progress_max, 4);
    EXPECT_EQ(sm.status().progress_current, 1);
}

TEST_F(MdStateMachineTest, ProgressMappingConnected) {
    sm.on_connect();
    sm.on_front_connected();
    EXPECT_EQ(sm.status().progress_desc, "已连接");
    EXPECT_EQ(sm.status().progress_max, 4);
    EXPECT_EQ(sm.status().progress_current, 2);
}

TEST_F(MdStateMachineTest, ProgressMappingLoggedIn) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_success("v", "d", "t");
    EXPECT_EQ(sm.status().progress_desc, "已登录");
    EXPECT_EQ(sm.status().progress_max, 4);
    EXPECT_EQ(sm.status().progress_current, 4);
}

TEST_F(MdStateMachineTest, ProgressMappingReady) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_success("v", "d", "t");
    EXPECT_EQ(sm.status().progress_desc, "已登录");
    EXPECT_EQ(sm.status().progress_current, 4);
    EXPECT_EQ(sm.status().progress_max, 4);
}

TEST_F(MdStateMachineTest, ProgressMappingDisconnected) {
    sm.on_connect();
    sm.on_front_disconnected(0x1001);
    EXPECT_EQ(sm.status().progress_desc, "重连中...");
    EXPECT_EQ(sm.status().progress_max, 4);
    EXPECT_EQ(sm.status().progress_current, 1);
}

TEST_F(MdStateMachineTest, ProgressMappingLoginFailed) {
    // 契约 md-status 失败语义: 登录失败态数值映射与 Connected 相同（current=2），
    // 失败原因由 RTN_MD_STATUS.last_error 承载；desc 仅展示为「登录失败」
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_failed(MdFailureCode::LoginTimeout, "10 秒内未收到登录响应");
    EXPECT_EQ(sm.status().progress_desc, "登录失败");
    EXPECT_EQ(sm.status().progress_min, 0);
    EXPECT_EQ(sm.status().progress_max, 4);
    EXPECT_EQ(sm.status().progress_current, 2);
}

TEST(MdFailureCodeStrTest, MapsAllCodes) {
    EXPECT_EQ(md_failure_code_str(MdFailureCode::ConnectTimeout), "connect_timeout");
    EXPECT_EQ(md_failure_code_str(MdFailureCode::LoginRejected), "login_rejected");
    EXPECT_EQ(md_failure_code_str(MdFailureCode::LoginTimeout), "login_timeout");
    EXPECT_EQ(md_failure_code_str(MdFailureCode::LoginParseError), "login_parse_error");
}

// --- Set API version ---

TEST_F(MdStateMachineTest, SetApiVersion) {
    sm.set_api_version("6.7.0");
    EXPECT_EQ(sm.status().api_version, "6.7.0");
}

// --- Same state transition is no-op ---

TEST_F(MdStateMachineTest, SameStateTransitionIsNoop) {
    sm.on_connect();  // Idle 转为 Connecting
    EXPECT_EQ(sm.state(), MdState::Connecting);
    // 从 Connecting 调用 on_disconnect 转为 Idle
    sm.on_disconnect();
    EXPECT_EQ(sm.state(), MdState::Idle);
    // Idle 状态下 on_disconnect 为空操作 (返回 nullopt)
    auto notif = sm.on_disconnect();
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

// --- Utility functions ---

TEST(DisconnectReasonStrTest, KnownReasons) {
    EXPECT_EQ(disconnect_reason_str(0x1001), "network read failed");
    EXPECT_EQ(disconnect_reason_str(0x1002), "network write failed");
    EXPECT_EQ(disconnect_reason_str(0x2001), "heartbeat receive timeout");
    EXPECT_EQ(disconnect_reason_str(0x2002), "heartbeat send failed");
    EXPECT_EQ(disconnect_reason_str(0x2003), "invalid packet received");
}

TEST(DisconnectReasonStrTest, UnknownReason) {
    auto result = disconnect_reason_str(9999);
    EXPECT_TRUE(result.find("unknown") != std::string::npos);
}

TEST(TradingDayToDaysTest, ValidDate) {
    // 2026-05-23 距 1970-01-01 的精确天数；常量 1/差一天/算错日期都必须变红。
    EXPECT_EQ(trading_day_to_days("20260523"), 20596);
}

TEST(TradingDayToDaysTest, InvalidFormat) {
    EXPECT_THROW(trading_day_to_days(""), std::runtime_error);
    EXPECT_THROW(trading_day_to_days("abc"), std::runtime_error);
    EXPECT_THROW(trading_day_to_days("20261301"), std::runtime_error);  // 非法月份
    // 契约要求恰好 YYYYMMDD 8 位；9 字符不能按前 8 位静默解析。
    EXPECT_THROW(trading_day_to_days("202605231"), std::runtime_error);
}

TEST(TradingDayToDaysTest, NullPointer) {
    EXPECT_THROW(trading_day_to_days(nullptr), std::runtime_error);
}

// --- Full lifecycle test ---

TEST_F(MdStateMachineTest, FullLifecycle) {
    // 起始状态: Idle
    EXPECT_EQ(sm.state(), MdState::Idle);

    // 发起连接
    sm.on_connect();
    EXPECT_EQ(sm.state(), MdState::Connecting);

    // 前置已连接
    sm.on_front_connected();
    EXPECT_EQ(sm.state(), MdState::Connected);

    // 请求登录
    sm.on_req_login();
    EXPECT_EQ(sm.state(), MdState::LoggingIn);

    // 登录成功
    sm.on_login_success("v6.7", "20260523", "09:00:00");
    EXPECT_EQ(sm.state(), MdState::LoggedIn);
    EXPECT_EQ(sm.status().progress_desc, "已登录");
    EXPECT_EQ(sm.status().sys_version, "v6.7");
    EXPECT_EQ(sm.status().trading_day, "20260523");
    EXPECT_EQ(sm.status().login_time, "09:00:00");
    sm.set_subscription_stats(5, 5);

    // Disconnect：进入 Idle 必须清空元信息并把订阅计数归零。
    sm.on_disconnect();
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_EQ(sm.status().progress_desc, "未登录");
    EXPECT_TRUE(sm.status().sys_version.empty());
    EXPECT_TRUE(sm.status().trading_day.empty());
    EXPECT_TRUE(sm.status().login_time.empty());
    EXPECT_EQ(sm.status().subscribed_count, 0u);
}

// ============================================================================
// trading_day_to_days: 不可能日期 (格式合法但日期不存在)
// ============================================================================

TEST(TradingDayToDaysTest, ImpossibleDateThrows) {
    // 2 月 30 日: 格式合法 (8 位数字), 但日期不存在, 走 ymd.ok() 分支
    EXPECT_THROW(trading_day_to_days("20260230"), std::runtime_error);
    // 11 月 31 日
    EXPECT_THROW(trading_day_to_days("20261131"), std::runtime_error);
    // 4 月 31 日 (小月)
    EXPECT_THROW(trading_day_to_days("20260431"), std::runtime_error);
    // 闰年 2 月 29 日合法
    EXPECT_NO_THROW(trading_day_to_days("20240229"));
    // 非闰年 2 月 29 日非法
    EXPECT_THROW(trading_day_to_days("20250229"), std::runtime_error);
}

// ============================================================================
// 登录失败/解析错误的意外状态守卫
// 7x24 场景: 定时器超时与 CTP 回调可能竞态, 需验证守卫不崩溃
// ============================================================================

TEST_F(MdStateMachineTest, LoginFailedFromUnexpectedStateIgnored) {
    // 不经过 on_req_login, 直接调 on_login_failed
    auto notif = sm.on_login_failed(MdFailureCode::LoginRejected, "x", 1);
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_FALSE(notif.has_value());
    EXPECT_FALSE(sm.status().last_error.has_value());
}

TEST_F(MdStateMachineTest, LoginParseErrorFromUnexpectedStateIgnored) {
    auto notif = sm.on_login_parse_error();
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

// ============================================================================
// build_md_status_payload: 契约 md-status 字段集验证
// ============================================================================

TEST(BuildMdStatusPayloadTest, ContainsContractFieldsIncludingNullLastError) {
    MdStateMachine sm;
    sm.set_api_version("v6.7.2");
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_success("v6.7.2_20240105", "20260808", "08:45:32");
    sm.set_subscription_stats(5000, 5000);

    auto j = build_md_status_payload(sm.status());

    // 契约 md-status 的 6 个状态字段 + last_error（无失败时为 null）
    EXPECT_EQ(j["api_version"], "v6.7.2");
    EXPECT_EQ(j["sys_version"], "v6.7.2_20240105");
    EXPECT_EQ(j["trading_day"], "20260808");
    EXPECT_EQ(j["login_time"], "08:45:32");
    EXPECT_EQ(j["expected_subscribe_count"], 5000);
    EXPECT_EQ(j["subscribed_count"], 5000);
    ASSERT_TRUE(j.contains("last_error"));
    EXPECT_TRUE(j["last_error"].is_null());
    // 契约 md-status: 登录/进度状态由 RTN_PROGRESS（契约 progress）覆盖
    EXPECT_EQ(j.size(), 7);
    EXPECT_FALSE(j.contains("login_state"));

    // 不应包含 state / progress_* 字段
    EXPECT_FALSE(j.contains("state"));
    EXPECT_FALSE(j.contains("progress_desc"));
    EXPECT_FALSE(j.contains("progress_min"));
    EXPECT_FALSE(j.contains("progress_max"));
    EXPECT_FALSE(j.contains("progress_current"));
}

TEST(BuildMdStatusPayloadTest, LastErrorSerializedWhenPresent) {
    MdStateMachine sm;
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    auto before = static_cast<int64_t>(std::time(nullptr));
    sm.on_login_failed(MdFailureCode::LoginRejected, "CTP:不合法的登录", 3);
    auto after = static_cast<int64_t>(std::time(nullptr));

    auto j = build_md_status_payload(sm.status());

    ASSERT_TRUE(j["last_error"].is_object());
    const auto& e = j["last_error"];
    EXPECT_EQ(e["code"], "login_rejected");
    EXPECT_EQ(e["message"], "CTP:不合法的登录");
    EXPECT_EQ(e["error_id"], 3);
    EXPECT_GE(e["timestamp"].get<int64_t>(), before);
    EXPECT_LE(e["timestamp"].get<int64_t>(), after);
}

TEST(BuildMdStatusPayloadTest, EmptyStringsWhenNotLoggedIn) {
    MdStateMachine sm;
    auto j = build_md_status_payload(sm.status());
    EXPECT_EQ(j["sys_version"], "");
    EXPECT_EQ(j["trading_day"], "");
    EXPECT_EQ(j["login_time"], "");
    EXPECT_EQ(j["expected_subscribe_count"], 0);
    EXPECT_EQ(j["subscribed_count"], 0);
    EXPECT_TRUE(j["last_error"].is_null());
    EXPECT_EQ(j.size(), 7);
    EXPECT_FALSE(j.contains("login_state"));
}
