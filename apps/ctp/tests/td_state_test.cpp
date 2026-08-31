#include <gtest/gtest.h>
#include <optional>
#include <string>

#include "td/td_state.h"

using namespace dztrader::ctp;

class TdStateMachineTest : public ::testing::Test {
protected:
    TdStateMachine sm;
};

// --- 初始状态 ---

TEST_F(TdStateMachineTest, InitialStateIsIdle) {
    EXPECT_EQ(sm.state(), TdState::Idle);
    EXPECT_EQ(sm.status().progress_max, 10);
    EXPECT_EQ(sm.status().progress_current, 0);
    EXPECT_EQ(sm.status().progress_desc, "未启动");
}

// --- 基本状态转移链路 ---

TEST_F(TdStateMachineTest, FullHappyPath) {
    // Idle -> Connecting
    auto notif = sm.on_connect();
    EXPECT_EQ(sm.state(), TdState::Connecting);
    EXPECT_FALSE(notif.has_value());
    EXPECT_EQ(sm.status().progress_current, 1);
    EXPECT_EQ(sm.status().progress_desc, "前置连接...");

    // Connecting -> Connected
    notif = sm.on_front_connected();
    EXPECT_EQ(sm.state(), TdState::Connected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->message, "交易连接成功");
    EXPECT_EQ(sm.status().progress_current, 2);
    EXPECT_EQ(sm.status().progress_desc, "已连接");

    // Connected -> Authenticating
    notif = sm.on_req_authenticate();
    EXPECT_EQ(sm.state(), TdState::Authenticating);
    EXPECT_FALSE(notif.has_value());
    EXPECT_EQ(sm.status().progress_current, 3);
    EXPECT_EQ(sm.status().progress_desc, "授权验证...");

    // Authenticating -> Authenticated
    notif = sm.on_authenticate_success();
    EXPECT_EQ(sm.state(), TdState::Authenticated);
    EXPECT_FALSE(notif.has_value());
    EXPECT_EQ(sm.status().progress_current, 4);
    EXPECT_EQ(sm.status().progress_desc, "已授权");

    // Authenticated -> LoggingIn
    notif = sm.on_req_login();
    EXPECT_EQ(sm.state(), TdState::LoggingIn);
    EXPECT_EQ(sm.status().progress_current, 5);
    EXPECT_EQ(sm.status().progress_desc, "用户登录...");

    // LoggingIn -> LoggedIn
    notif = sm.on_login_success("v1.0", "20260726", "09:00:00");
    EXPECT_EQ(sm.state(), TdState::LoggedIn);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->message, "交易登录成功");
    EXPECT_EQ(sm.status().sys_version, "v1.0");
    EXPECT_EQ(sm.status().trading_day, "20260726");
    EXPECT_EQ(sm.status().login_time, "09:00:00");
    EXPECT_EQ(sm.status().progress_current, 6);
    EXPECT_EQ(sm.status().progress_desc, "已登录");

    // LoggedIn -> Confirming
    notif = sm.on_req_settlement_confirm();
    EXPECT_EQ(sm.state(), TdState::Confirming);
    EXPECT_EQ(sm.status().progress_current, 7);
    EXPECT_EQ(sm.status().progress_desc, "结算单确认...");

    // Confirming -> LoadingInstruments
    notif = sm.on_settlement_confirmed();
    EXPECT_EQ(sm.state(), TdState::LoadingInstruments);
    EXPECT_EQ(sm.status().progress_current, 8);
    EXPECT_EQ(sm.status().progress_desc, "加载合约...");

    // LoadingInstruments -> Ready
    notif = sm.on_instruments_loaded();
    EXPECT_EQ(sm.state(), TdState::Ready);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->message, "交易就绪");
    EXPECT_EQ(sm.status().progress_current, 10);
    EXPECT_EQ(sm.status().progress_desc, "就绪");
}

// --- 无认证路径 (Connected -> LoggingIn) ---

TEST_F(TdStateMachineTest, LoginWithoutAuth) {
    sm.on_connect();
    sm.on_front_connected();
    // 无 auth_code 时直接 login
    auto notif = sm.on_req_login();
    EXPECT_EQ(sm.state(), TdState::LoggingIn);
    EXPECT_FALSE(notif.has_value());
}

// --- 认证失败回退 ---

TEST_F(TdStateMachineTest, AuthenticateFailedRollback) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    auto notif = sm.on_authenticate_failed();
    EXPECT_EQ(sm.state(), TdState::Connected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notif->popup);
    EXPECT_EQ(notif->message, "认证失败");
    EXPECT_EQ(sm.status().progress_current, 2);
}

// --- 登录失败回退 ---

TEST_F(TdStateMachineTest, LoginFailedRollback) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    auto notif = sm.on_login_failed();
    EXPECT_EQ(sm.state(), TdState::Authenticated);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notif->popup);
    EXPECT_EQ(notif->message, "交易登录失败");
    EXPECT_EQ(sm.status().progress_current, 4);
    EXPECT_EQ(sm.status().progress_desc, "已授权");
}

// --- 无认证路径登录失败回退到 Connected ---

TEST_F(TdStateMachineTest, LoginFailedWithoutAuthRollsBackToConnected) {
    sm.on_connect();
    sm.on_front_connected();
    // 无 auth_code, 直接 login
    sm.on_req_login();
    auto notif = sm.on_login_failed();
    EXPECT_EQ(sm.state(), TdState::Connected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notif->popup);
    EXPECT_EQ(sm.status().progress_current, 2);
    EXPECT_EQ(sm.status().progress_desc, "已连接");
}

// --- 合约加载失败回退 ---

TEST_F(TdStateMachineTest, InstrumentsLoadFailedRollback) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("v", "d", "t");
    sm.on_req_settlement_confirm();
    sm.on_settlement_confirmed();
    auto notif = sm.on_instruments_load_failed("未到合约加载时段");
    EXPECT_EQ(sm.state(), TdState::LoggedIn);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notif->popup);
    EXPECT_EQ(notif->message, "合约加载失败: 未到合约加载时段");
    EXPECT_EQ(sm.status().progress_current, 6);
    EXPECT_EQ(sm.status().progress_desc, "已登录");
}

// --- 断线 ---

TEST_F(TdStateMachineTest, FrontDisconnectedFromAnyState) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("v", "d", "t");
    auto notif = sm.on_front_disconnected(0x1001);
    EXPECT_EQ(sm.state(), TdState::Disconnected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->level, DZ_NOTIFY_WARN);
    EXPECT_EQ(sm.status().progress_current, 1);
    EXPECT_EQ(sm.status().progress_desc, "重连中...");
}

TEST_F(TdStateMachineTest, FrontDisconnectedFromIdleIgnored) {
    auto notif = sm.on_front_disconnected(0x1001);
    EXPECT_EQ(sm.state(), TdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(TdStateMachineTest, FrontDisconnectedFromDisconnectedIgnored) {
    sm.on_connect();
    sm.on_front_connected();
    auto first = sm.on_front_disconnected(0x1001);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(sm.state(), TdState::Disconnected);
    // 重复回调应被忽略
    auto repeat = sm.on_front_disconnected(0x1002);
    EXPECT_EQ(sm.state(), TdState::Disconnected);
    EXPECT_FALSE(repeat.has_value());
}

// --- 重连后从 Disconnected 恢复 ---

TEST_F(TdStateMachineTest, ReconnectFromDisconnected) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_front_disconnected(0x1001);
    EXPECT_EQ(sm.state(), TdState::Disconnected);
    auto notif = sm.on_front_connected();
    EXPECT_EQ(sm.state(), TdState::Connected);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->message, "交易连接成功");
}

// --- 非法转移被拒绝 ---

TEST_F(TdStateMachineTest, ConnectFromNonIdleRejected) {
    sm.on_connect();  // Idle -> Connecting
    auto notif = sm.on_connect();  // 被拒绝
    EXPECT_EQ(sm.state(), TdState::Connecting);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(TdStateMachineTest, LoginFromIdleRejected) {
    auto notif = sm.on_req_login();
    EXPECT_EQ(sm.state(), TdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(TdStateMachineTest, AuthenticateFromLoggingInRejected) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    auto notif = sm.on_req_authenticate();  // LoggingIn 状态拒绝
    EXPECT_EQ(sm.state(), TdState::LoggingIn);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(TdStateMachineTest, InstrumentsLoadedFromNonLoadingRejected) {
    sm.on_connect();
    sm.on_front_connected();
    auto notif = sm.on_instruments_loaded();  // Connected 状态拒绝
    EXPECT_EQ(sm.state(), TdState::Connected);
    EXPECT_FALSE(notif.has_value());
}

// --- disconnect 从任意状态 ---

TEST_F(TdStateMachineTest, DisconnectFromAnyState) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    auto notif = sm.on_disconnect();
    EXPECT_EQ(sm.state(), TdState::Idle);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->message, "交易已断开");
    // Idle 状态字段清空
    EXPECT_TRUE(sm.status().login_time.empty());
    EXPECT_TRUE(sm.status().sys_version.empty());
    EXPECT_TRUE(sm.status().trading_day.empty());
    EXPECT_EQ(sm.status().progress_current, 0);
}

TEST_F(TdStateMachineTest, DisconnectFromIdleNoOp) {
    auto notif = sm.on_disconnect();
    EXPECT_EQ(sm.state(), TdState::Idle);
    EXPECT_FALSE(notif.has_value());
}

TEST_F(TdStateMachineTest, DisconnectFromDisconnected) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_front_disconnected(0x1001);
    EXPECT_EQ(sm.state(), TdState::Disconnected);
    auto notif = sm.on_disconnect();
    EXPECT_EQ(sm.state(), TdState::Idle);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->message, "交易已断开");
    // was_authenticated_ 应已重置 (可通过再次走无认证路径登录失败验证)
}

// --- Ready 状态 progress_current=10 (max) ---

TEST_F(TdStateMachineTest, ReadyStateProgressIsMax) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("v", "d", "t");
    sm.on_req_settlement_confirm();
    sm.on_settlement_confirmed();
    sm.on_instruments_loaded();
    EXPECT_EQ(sm.status().progress_current, sm.status().progress_max);
    EXPECT_EQ(sm.status().progress_current, 10);
}

// --- TdState 11 态 -> DzAccountState 三态聚合映射 (契约 account-status) ---

TEST(AccountStateMapping, TripleStateAggregation) {
    using namespace dztrader::ctp;
    EXPECT_EQ(account_state_of(TdState::Idle), DZ_ACCOUNT_OFFLINE);
    EXPECT_EQ(account_state_of(TdState::Disconnected), DZ_ACCOUNT_OFFLINE);
    EXPECT_EQ(account_state_of(TdState::Connecting), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::Connected), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::Authenticating), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::Authenticated), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::LoggingIn), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::Confirming), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::LoadingInstruments), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::Ready), DZ_ACCOUNT_READY);
}
