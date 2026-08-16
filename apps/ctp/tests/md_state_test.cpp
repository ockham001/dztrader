#include <gtest/gtest.h>
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
    sm.on_connect();  // 现处于 Connecting
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

    // 断开连接
    sm.on_front_disconnected(0x1001);
    EXPECT_EQ(sm.state(), MdState::Disconnected);
    EXPECT_EQ(sm.status().progress_desc, "重连中...");

    // 重连 (CTP 自动重连)
    auto notif = sm.on_front_connected();
    EXPECT_EQ(sm.state(), MdState::Connected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情连接成功");
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

TEST_F(MdStateMachineTest, LoginFailedTransitionsToConnected) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    auto notif = sm.on_login_failed();
    EXPECT_EQ(sm.state(), MdState::Connected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情登录失败");
    EXPECT_EQ((*notif)["level"].get<int>(), DZ_NOTIFY_ERROR);
    EXPECT_TRUE((*notif)["popup"].get<bool>());
}

TEST_F(MdStateMachineTest, LoginParseErrorTransitionsToConnected) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    auto notif = sm.on_login_parse_error();
    EXPECT_EQ(sm.state(), MdState::Connected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ((*notif)["message"].get<std::string>(), "行情登录失败");
    EXPECT_EQ((*notif)["level"].get<int>(), DZ_NOTIFY_ERROR);
    EXPECT_TRUE((*notif)["popup"].get<bool>());
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
    EXPECT_NO_THROW(trading_day_to_days("20260523"));
    auto days = trading_day_to_days("20260523");
    EXPECT_GT(days, 0);
}

TEST(TradingDayToDaysTest, InvalidFormat) {
    EXPECT_THROW(trading_day_to_days(""), std::runtime_error);
    EXPECT_THROW(trading_day_to_days("abc"), std::runtime_error);
    EXPECT_THROW(trading_day_to_days("20261301"), std::runtime_error);  // 非法月份
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

    // Disconnect
    sm.on_disconnect();
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_EQ(sm.status().progress_desc, "未登录");
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
    auto notif = sm.on_login_failed();
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(MdStateMachineTest, LoginParseErrorFromUnexpectedStateIgnored) {
    auto notif = sm.on_login_parse_error();
    EXPECT_EQ(sm.state(), MdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

// ============================================================================
// build_md_status_payload: 契约 md-status 字段集验证
// ============================================================================

TEST(BuildMdStatusPayloadTest, ContainsExactlySixContractFields) {
    MdStateMachine sm;
    sm.set_api_version("v6.7.2");
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_success("v6.7.2_20240105", "20260808", "08:45:32");
    sm.set_subscription_stats(5000, 5000);

    auto j = build_md_status_payload(sm.status());

    // 契约 md-status 的 6 个字段
    EXPECT_EQ(j["api_version"], "v6.7.2");
    EXPECT_EQ(j["sys_version"], "v6.7.2_20240105");
    EXPECT_EQ(j["trading_day"], "20260808");
    EXPECT_EQ(j["login_time"], "08:45:32");
    EXPECT_EQ(j["expected_subscribe_count"], 5000);
    EXPECT_EQ(j["subscribed_count"], 5000);
    // 契约 md-status: payload 仅 6 字段——登录/进度状态由 RTN_PROGRESS（契约 progress）覆盖
    EXPECT_EQ(j.size(), 6);
    EXPECT_FALSE(j.contains("login_state"));

    // 不应包含 state / progress_* 字段
    EXPECT_FALSE(j.contains("state"));
    EXPECT_FALSE(j.contains("progress_desc"));
    EXPECT_FALSE(j.contains("progress_min"));
    EXPECT_FALSE(j.contains("progress_max"));
    EXPECT_FALSE(j.contains("progress_current"));
}

TEST(BuildMdStatusPayloadTest, EmptyStringsWhenNotLoggedIn) {
    MdStateMachine sm;
    auto j = build_md_status_payload(sm.status());
    EXPECT_EQ(j["sys_version"], "");
    EXPECT_EQ(j["trading_day"], "");
    EXPECT_EQ(j["login_time"], "");
    EXPECT_EQ(j["expected_subscribe_count"], 0);
    EXPECT_EQ(j["subscribed_count"], 0);
    EXPECT_EQ(j.size(), 6);
    EXPECT_FALSE(j.contains("login_state"));
}
