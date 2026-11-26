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
    EXPECT_EQ(sm.status().ext.sys_version, "v1.0");
    EXPECT_EQ(sm.status().date, "20260726");
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

// --- 认证失败: 停在失败阶段 (ADR 0059 决策 1) ---

TEST_F(TdStateMachineTest, AuthenticateFailedStopsAtAuthFailed) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    ASSERT_EQ(sm.status().progress_current, 3);
    auto notif = sm.on_authenticate_failed(TdFailureCode::AuthRejected, "CTP:用户认证失败", 3);
    // 停在失败阶段: 与 Authenticating 同格 (3/10), 不回退到 Connected (2)。
    EXPECT_EQ(sm.state(), TdState::AuthFailed);
    EXPECT_EQ(sm.status().progress_current, 3);
    EXPECT_EQ(sm.status().progress_desc, "认证失败");
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notif->popup);
    // 段内原因写入 last_error (契约 td-status §2.5)
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "auth_rejected");
    EXPECT_EQ(sm.status().last_error->message, "CTP:用户认证失败");
    EXPECT_EQ(sm.status().last_error->error_id, 3);
    EXPECT_GT(sm.status().last_error->timestamp, 0);
}

TEST_F(TdStateMachineTest, AuthTimeoutWritesSameFailurePathAsRejection) {
    // ADR 0059 决策 5: 超时与柜台拒绝同失败态、同 last_error 写入, 只靠 code 区分。
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    auto notif = sm.on_authenticate_failed(TdFailureCode::AuthTimeout, "", 0);
    EXPECT_EQ(sm.state(), TdState::AuthFailed);
    EXPECT_EQ(sm.status().progress_current, 3);
    ASSERT_TRUE(notif.has_value());
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "auth_timeout");
    EXPECT_EQ(sm.status().last_error->message, "");  // 空串 = 无详情, 前端按 code 兜底
    EXPECT_EQ(sm.status().last_error->error_id, 0);
}

TEST_F(TdStateMachineTest, AuthenticateFailedOutsideAuthenticatingIsSilentAndDoesNotTransfer) {
    // 非预期状态下收到认证失败: 不转移、不写 last_error、不推通知 (保持既有语义)。
    ASSERT_EQ(sm.state(), TdState::Idle);
    auto notif = sm.on_authenticate_failed(TdFailureCode::AuthRejected, "late", 3);
    EXPECT_FALSE(notif.has_value());
    EXPECT_EQ(sm.state(), TdState::Idle);
    EXPECT_FALSE(sm.status().last_error.has_value());
}

// --- 登录失败: 停在失败阶段 (ADR 0059 决策 1, #495) ---

TEST_F(TdStateMachineTest, LoginRejectedStopsAtLoginFailed) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    ASSERT_EQ(sm.status().progress_current, 5);
    auto notif = sm.on_login_failed(TdFailureCode::LoginRejected, "CTP:user not found", 3);
    // 停在失败阶段: 与 LoggingIn 同格 (5/10), 不回退到 Authenticated (4)。
    EXPECT_EQ(sm.state(), TdState::LoginFailed);
    EXPECT_EQ(sm.status().progress_current, 5);
    EXPECT_EQ(sm.status().progress_desc, "登录失败");
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notif->popup);
    // 段内原因写入 last_error (契约 td-status §2.5)
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "login_rejected");
    EXPECT_EQ(sm.status().last_error->message, "CTP:user not found");
    EXPECT_EQ(sm.status().last_error->error_id, 3);
    EXPECT_GT(sm.status().last_error->timestamp, 0);
}

TEST_F(TdStateMachineTest, LoginFailedWithoutAuthPathAlsoStopsAtLoginFailed) {
    // 无 auth_code 时链从 Connected 直接进 LoggingIn: 失败同样停在 5/10, 不回 Connected (2)。
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_failed(TdFailureCode::LoginParseError, "交易日无效（原始值 \"00000000\"）", 0);
    EXPECT_EQ(sm.state(), TdState::LoginFailed);
    EXPECT_EQ(sm.status().progress_current, 5);
    EXPECT_EQ(sm.status().progress_desc, "登录失败");
}

TEST_F(TdStateMachineTest, LoginTimeoutWritesSameFailurePathAsRejection) {
    // ADR 0059 决策 5: 超时与柜台拒绝同失败态、同 last_error 写入, 只靠 code 区分。
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    auto notif = sm.on_login_failed(TdFailureCode::LoginTimeout, "", 0);
    EXPECT_EQ(sm.state(), TdState::LoginFailed);
    EXPECT_EQ(sm.status().progress_current, 5);
    ASSERT_TRUE(notif.has_value());
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "login_timeout");
    EXPECT_EQ(sm.status().last_error->message, "");  // 空串 = 无详情, 前端按 code 兜底
    EXPECT_EQ(sm.status().last_error->error_id, 0);
}

TEST_F(TdStateMachineTest, LoginParseErrorStopsAtLoginFailed) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_failed(TdFailureCode::LoginParseError, "登录响应缺少登录数据", 0);
    EXPECT_EQ(sm.state(), TdState::LoginFailed);
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "login_parse_error");
}

TEST_F(TdStateMachineTest, DayLoadFailedStopsAtLoginFailed) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_failed(TdFailureCode::DayLoadFailed, "当日订单/成交基准装载失败", 0);
    EXPECT_EQ(sm.state(), TdState::LoginFailed);
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "day_load_failed");
}

TEST_F(TdStateMachineTest, LoginFailedClearsLoginTime) {
    // 失败后不得残留上一次的登录时间 (UI 会误显示「已登录于 …」)。
    // 必须先把 login_time 置成非空再验清空 —— 否则断言恒真 (它本来就只在成功登录时写入)。
    // 走法: 先完整登录成功一次留下 login_time, 再断线重连重新发起登录并失败。
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("v", "20260726", "09:00:00");
    ASSERT_EQ(sm.status().login_time, "09:00:00") << "premise: a successful login set it";

    sm.on_front_disconnected(0x1001);  // -> Disconnected (保留 date/login_time)
    ASSERT_EQ(sm.status().login_time, "09:00:00") << "premise: disconnect keeps it";
    sm.on_front_connected();  // -> Connected
    sm.on_req_login();        // -> LoggingIn
    sm.on_login_failed(TdFailureCode::LoginRejected, "CTP:user not found", 3);

    EXPECT_EQ(sm.state(), TdState::LoginFailed);
    EXPECT_TRUE(sm.status().login_time.empty()) << "a stale login time must be cleared on failure";
}

TEST_F(TdStateMachineTest, LoginFailedOutsideLoggingInIsSilentAndDoesNotTransfer) {
    // 非预期状态下收到登录失败: 不转移、不写 last_error、不推通知 (保持既有语义)。
    ASSERT_EQ(sm.state(), TdState::Idle);
    auto notif = sm.on_login_failed(TdFailureCode::LoginRejected, "late", 3);
    EXPECT_FALSE(notif.has_value());
    EXPECT_EQ(sm.state(), TdState::Idle);
    EXPECT_FALSE(sm.status().last_error.has_value());
}

TEST_F(TdStateMachineTest, LoginFailedRejectsInPlaceRetry) {
    // ADR 0051 决策 4: 失败态不提供原地重试 —— on_req_login 只在 Connected/Authenticated 有效。
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    sm.on_login_failed(TdFailureCode::LoginRejected, "CTP:user not found", 3);
    ASSERT_EQ(sm.state(), TdState::LoginFailed);
    // on_req_login 无论成功与否都返回 nullopt, 所以「返回值是 nullopt」证明不了任何事 ——
    // 只有状态不变才说明转移被拒 (ADR 0051 决策 4: 失败态不提供原地重试)。
    (void)sm.on_req_login();
    EXPECT_EQ(sm.state(), TdState::LoginFailed);
}

TEST_F(TdStateMachineTest, LoginFailedDoesNotRollBackProgress) {
    // 核心回归: 失败态的 current 必须等于它停住那一段的数值, 不得回退到上一段。
    // 期望值取自契约表 (td-status §2.2: LoginFailed = 5, desc = 登录失败), 不取实现自身的
    // 中间值 —— 否则 progress 表改错时断言会跟着错 (docs/unit-test-standard.md §1.3)。
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    ASSERT_EQ(sm.status().progress_current, 5) << "premise: LoggingIn is 5/10";
    sm.on_login_failed(TdFailureCode::LoginTimeout, "", 0);
    EXPECT_EQ(sm.status().progress_current, 5)
        << "failure must stay at the failed stage, not fall back to Authenticated (4)";
    EXPECT_EQ(sm.status().progress_desc, "登录失败");
    EXPECT_LT(sm.status().progress_current, sm.status().progress_max)
        << "failure is not an endpoint";
    EXPECT_GT(sm.status().progress_current, sm.status().progress_min)
        << "failure is not an endpoint";
}

// --- 合约加载失败: 停在失败阶段 (#496 / ADR 0059 决策 1) ---

TEST_F(TdStateMachineTest, InstrumentsLoadFailedStopsAtFailedStage) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("v", "d", "t");
    sm.on_req_settlement_confirm();
    sm.on_settlement_confirmed();
    ASSERT_EQ(sm.status().progress_current, 8) << "premise: LoadingInstruments is 8/10";
    auto notif =
        sm.on_instruments_load_failed(TdFailureCode::InstrumentsRejected, "未到合约加载时段", 7);
    // 停在失败阶段: 与 LoadingInstruments 同格 (8/10), 不回退到 LoggedIn (6)。
    EXPECT_EQ(sm.state(), TdState::InstrumentsLoadFailed);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notif->popup);
    // 段内原因写入 last_error (契约 td-status §2.5)
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "instruments_rejected");
    EXPECT_EQ(sm.status().last_error->message, "未到合约加载时段");
    EXPECT_EQ(sm.status().last_error->error_id, 7);
    EXPECT_GT(sm.status().last_error->timestamp, 0);
    EXPECT_EQ(sm.status().progress_current, 8);
    EXPECT_EQ(sm.status().progress_desc, "合约加载失败");
    // 失败态不占端点 (ADR 0059 决策 1)
    EXPECT_LT(sm.status().progress_current, sm.status().progress_max);
    EXPECT_GT(sm.status().progress_current, sm.status().progress_min);
}

TEST_F(TdStateMachineTest, InstrumentsLoadTimeoutStopsAtFailedStage) {
    // 超时与柜台拒绝同等对待 (ADR 0059 决策 5): 同一失败态、同一 last_error 写入,
    // 只靠 code 区分。
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("v", "d", "t");
    sm.on_req_settlement_confirm();
    sm.on_settlement_confirmed();
    sm.on_instruments_load_failed(TdFailureCode::InstrumentsTimeout, "", 0);
    EXPECT_EQ(sm.state(), TdState::InstrumentsLoadFailed);
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "instruments_timeout");
    EXPECT_EQ(sm.status().last_error->error_id, 0) << "a timeout has no CTP error id";
    EXPECT_EQ(sm.status().progress_current, 8);
    EXPECT_EQ(sm.status().progress_desc, "合约加载失败");
}

TEST_F(TdStateMachineTest, InstrumentsLoadFailedFromNonLoadingRejected) {
    sm.on_connect();
    sm.on_front_connected();
    auto notif =
        sm.on_instruments_load_failed(TdFailureCode::InstrumentsRejected, "err", 1);  // Connected
    EXPECT_EQ(sm.state(), TdState::Connected);
    EXPECT_FALSE(notif.has_value());
    EXPECT_FALSE(sm.status().last_error.has_value()) << "a rejected transfer must not record";
}

// --- 结算单确认失败: 停在失败阶段 (#496 / ADR 0059 决策 1) ---

TEST_F(TdStateMachineTest, SettlementConfirmFailedStopsAtFailedStage) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("v", "20260726", "09:00:00");
    sm.on_req_settlement_confirm();
    ASSERT_EQ(sm.status().progress_current, 7) << "premise: Confirming is 7/10";
    auto notif =
        sm.on_settlement_confirm_failed(TdFailureCode::SettlementRejected, "结算单未确认", 3);
    // 停在失败阶段: 与 Confirming 同格 (7/10), 不回退到 LoggedIn (6)。
    EXPECT_EQ(sm.state(), TdState::SettlementConfirmFailed);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notif->popup);
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "settlement_rejected");
    EXPECT_EQ(sm.status().last_error->message, "结算单未确认");
    EXPECT_EQ(sm.status().last_error->error_id, 3);
    EXPECT_EQ(sm.status().progress_current, 7);
    EXPECT_EQ(sm.status().progress_desc, "结算单确认失败");
    EXPECT_LT(sm.status().progress_current, sm.status().progress_max);
    EXPECT_GT(sm.status().progress_current, sm.status().progress_min);
    // 失败态不移除登录信息 (会话仍有效, 只是链没走完)
    EXPECT_EQ(sm.status().date, "20260726");
    EXPECT_EQ(sm.status().login_time, "09:00:00");
}

TEST_F(TdStateMachineTest, SettlementConfirmTimeoutStopsAtFailedStage) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("v", "20260726", "09:00:00");
    sm.on_req_settlement_confirm();
    sm.on_settlement_confirm_failed(TdFailureCode::SettlementTimeout, "", 0);
    EXPECT_EQ(sm.state(), TdState::SettlementConfirmFailed);
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "settlement_timeout");
    EXPECT_EQ(sm.status().progress_current, 7);
    EXPECT_EQ(sm.status().progress_desc, "结算单确认失败");
}

TEST_F(TdStateMachineTest, SettlementConfirmFailedFromNonConfirmingRejected) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("v", "20260726", "09:00:00");
    // LoggedIn (非 Confirming) 拒绝
    auto notif = sm.on_settlement_confirm_failed(TdFailureCode::SettlementRejected, "err");
    EXPECT_EQ(sm.state(), TdState::LoggedIn);
    EXPECT_FALSE(notif.has_value());
    EXPECT_EQ(sm.status().progress_current, 6);
    EXPECT_FALSE(sm.status().last_error.has_value());
    // Idle 同样拒绝
    TdStateMachine idle_sm;
    EXPECT_FALSE(
        idle_sm.on_settlement_confirm_failed(TdFailureCode::SettlementRejected, "err").has_value());
    EXPECT_EQ(idle_sm.state(), TdState::Idle);
}

// --- 连接超时: 回 Idle, 不进失败态 (ADR 0059 决策 7 的例外) ---

TEST_F(TdStateMachineTest, ConnectTimeoutRollsBackToIdle) {
    sm.on_connect();
    ASSERT_EQ(sm.state(), TdState::Connecting);
    auto notif = sm.on_connect_timeout();
    EXPECT_EQ(sm.state(), TdState::Idle);
    ASSERT_TRUE(notif.has_value());
    EXPECT_EQ(notif->level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notif->popup);
    EXPECT_EQ(notif->message, "交易连接超时");
    EXPECT_EQ(sm.status().progress_current, 0);
    EXPECT_EQ(sm.status().progress_desc, "未启动");
}

TEST_F(TdStateMachineTest, ConnectTimeoutWritesConnectTimeoutCode) {
    // ADR 0059 决策 7 / #495 AC: 连接超时**不加失败态** (仍是 Idle = Offline 三态),
    // 只写 last_error.code = connect_timeout —— 原因只能从这里读。
    sm.on_connect();
    sm.on_connect_timeout();
    ASSERT_EQ(sm.state(), TdState::Idle) << "connect timeout must stay at the offline endpoint";
    EXPECT_EQ(account_state_of(sm.state()), DZ_ACCOUNT_OFFLINE)
        << "a failure stage here would report 'logging in' for a front that never connected";
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "connect_timeout");
    EXPECT_EQ(sm.status().last_error->message, kConnectTimeoutReason);
    EXPECT_EQ(sm.status().last_error->error_id, 0);
    EXPECT_GT(sm.status().last_error->timestamp, 0);
}

TEST_F(TdStateMachineTest, ConnectTimeoutFromNonConnectingRejected) {
    // Idle 拒绝
    EXPECT_FALSE(sm.on_connect_timeout().has_value());
    EXPECT_EQ(sm.state(), TdState::Idle);
    EXPECT_FALSE(sm.status().last_error.has_value()) << "a rejected transition writes nothing";

    // Connected 拒绝 (已连上, 超时定时器不应回退状态)
    sm.on_connect();
    sm.on_front_connected();
    EXPECT_FALSE(sm.on_connect_timeout().has_value());
    EXPECT_EQ(sm.state(), TdState::Connected);
    EXPECT_EQ(sm.status().progress_current, 2);
    EXPECT_FALSE(sm.status().last_error.has_value());
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
    sm.on_connect();               // Idle -> Connecting
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
    EXPECT_TRUE(sm.status().ext.sys_version.empty());
    EXPECT_TRUE(sm.status().date.empty());
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
    // 重连后走无认证路径登录: 失败同样停在 5/10「登录失败」——失败位置与「认证过没有」
    // 无关 (ADR 0059 决策 1), 唯一的失败态就是它自己那一段。
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_login();
    auto login_failed = sm.on_login_failed(TdFailureCode::LoginRejected, "CTP:user not found", 3);
    EXPECT_EQ(sm.state(), TdState::LoginFailed);
    ASSERT_TRUE(login_failed.has_value());
    EXPECT_EQ(login_failed->level, DZ_NOTIFY_ERROR);
    EXPECT_EQ(sm.status().progress_current, 5);
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

// --- last_error 清空时机 (契约 td-status §2.5) ---

namespace {

/// 把状态机推进到 AuthFailed 并返回 (夹具: on_connect 会清 last_error, 故先转再写)。
void drive_to_auth_failed(TdStateMachine& sm, int error_id = 3) {
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_failed(TdFailureCode::AuthRejected, "CTP:用户认证失败", error_id);
}

}  // namespace

TEST_F(TdStateMachineTest, LastErrorClearedOnNextLoginAttempt) {
    drive_to_auth_failed(sm);
    ASSERT_TRUE(sm.status().last_error.has_value());

    // 先登出 (进 Idle): last_error 不清空 —— 保留供回看 (ADR 0059 决策 6)
    sm.on_disconnect();
    EXPECT_EQ(sm.state(), TdState::Idle);
    ASSERT_TRUE(sm.status().last_error.has_value()) << "logout must keep the last failure";
    EXPECT_EQ(sm.status().last_error->code, "auth_rejected");

    // 下一次登录尝试被受理 = on_connect: 清空
    sm.on_connect();
    EXPECT_FALSE(sm.status().last_error.has_value());
}

TEST_F(TdStateMachineTest, LastErrorClearedOnReady) {
    drive_to_auth_failed(sm);
    ASSERT_TRUE(sm.status().last_error.has_value());

    // 重来一遍走到 Ready: 转 Ready 时清空
    sm.on_disconnect();
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("v1.0", "20260726", "09:00:00");
    sm.on_req_settlement_confirm();
    sm.on_settlement_confirmed();
    sm.on_instruments_loaded();
    ASSERT_EQ(sm.state(), TdState::Ready);
    EXPECT_FALSE(sm.status().last_error.has_value());
}

TEST_F(TdStateMachineTest, LastErrorKeptAcrossDisconnect) {
    // 断线 (Disconnected) 不是「新尝试受理」也不是「就绪」: last_error 保留,
    // 用户仍能看到上次失败的原因。
    drive_to_auth_failed(sm);
    sm.on_front_disconnected(0x1001);
    EXPECT_EQ(sm.state(), TdState::Disconnected);
    ASSERT_TRUE(sm.status().last_error.has_value());
    EXPECT_EQ(sm.status().last_error->code, "auth_rejected");
}

TEST_F(TdStateMachineTest, AuthFailedDoesNotRollBackProgress) {
    // 核心回归: 失败态的 current 必须等于它停住那一段的数值, 不得回退到上一段。
    sm.on_connect();
    sm.on_front_connected();
    const int at_connected = sm.status().progress_current;
    sm.on_req_authenticate();
    const int at_authenticating = sm.status().progress_current;
    sm.on_authenticate_failed(TdFailureCode::AuthTimeout, "", 0);
    EXPECT_EQ(sm.status().progress_current, at_authenticating);
    EXPECT_NE(at_authenticating, at_connected)
        << "test premise: auth stage must differ from connected";
    EXPECT_LT(sm.status().progress_current, sm.status().progress_max)
        << "failure is not an endpoint";
    EXPECT_GT(sm.status().progress_current, sm.status().progress_min)
        << "failure is not an endpoint";
}

// --- TdState -> DzAccountState 三态聚合映射 (契约 account-status) ---

TEST(AccountStateMapping, TripleStateAggregation) {
    using namespace dztrader::ctp;
    EXPECT_EQ(account_state_of(TdState::Idle), DZ_ACCOUNT_OFFLINE);
    EXPECT_EQ(account_state_of(TdState::Disconnected), DZ_ACCOUNT_OFFLINE);
    EXPECT_EQ(account_state_of(TdState::Connecting), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::Connected), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::Authenticating), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::AuthFailed), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::Authenticated), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::LoggingIn), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::LoginFailed), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::LoggedIn), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::Confirming), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::SettlementConfirmFailed), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::LoadingInstruments), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::InstrumentsLoadFailed), DZ_ACCOUNT_LOGGING_IN);
    EXPECT_EQ(account_state_of(TdState::Ready), DZ_ACCOUNT_READY);
}

// --- TdStatus JSON 形状（契约 td-status §2.1：通用层 + ext 分层）---

TEST(TdStatusJsonTest, PayloadMatchesContractShape) {
    using namespace dztrader::ctp;
    TdStateMachine sm;
    sm.set_api_version("v6.7.2");
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("v6.7.2_20240105", "20260808", "2026-08-08 08:45:32");

    nlohmann::json j = sm.status();

    // 通用层（契约 td-status §2.1）：date / login_time / state / progress_* 在顶层
    EXPECT_EQ(j["date"], "20260808");
    EXPECT_EQ(j["login_time"], "2026-08-08 08:45:32");
    EXPECT_EQ(j["state"], "LoggedIn");
    EXPECT_EQ(j["progress_max"], 10);

    // 柜台特有字段进 ext（§2.3），不再在顶层
    ASSERT_TRUE(j.contains("ext"));
    EXPECT_EQ(j["ext"]["api_version"], "v6.7.2");
    EXPECT_EQ(j["ext"]["sys_version"], "v6.7.2_20240105");
    EXPECT_FALSE(j.contains("api_version"));
    EXPECT_FALSE(j.contains("sys_version"));
}

TEST(TdStatusJsonTest, IdleClearsSessionFieldsInsideExt) {
    using namespace dztrader::ctp;
    TdStateMachine sm;
    sm.set_api_version("v6.7.2");
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_success();
    sm.on_req_login();
    sm.on_login_success("sys_ver", "20260808", "2026-08-08 08:45:32");
    sm.on_disconnect();

    EXPECT_TRUE(sm.status().date.empty());
    EXPECT_TRUE(sm.status().login_time.empty());
    EXPECT_TRUE(sm.status().ext.sys_version.empty());
    // api_version 是进程生命周期常量，Idle 时保留
    EXPECT_EQ(sm.status().ext.api_version, "v6.7.2");
}

// ============================================================================
// RTN_TD_STATUS payload: 未知枚举计数进 ext（契约 td-status §2.3，issue #367）
// ============================================================================

TEST(TdStatusPayloadTest, AddsUnknownEnumCountersUnderExt) {
    using namespace dztrader::ctp;
    TdStatus status;
    status.account_id = "acc1";
    status.state = TdState::Ready;
    status.ext.api_version = "v6.7.2";

    UnknownEnumCounts counts;
    counts.order_status = 3;
    counts.direction = 1;
    counts.price_type = 0;
    counts.position_effect = 2;
    EXPECT_EQ(counts.total(), 6u);

    const nlohmann::json j = td_status_payload(status, counts);
    // 按种类分开计数：合并成一个总数会让运维无法判断该补映射还是该查数据源。
    EXPECT_EQ(j["ext"]["unknown_order_status"], 3);
    EXPECT_EQ(j["ext"]["unknown_direction"], 1);
    EXPECT_EQ(j["ext"]["unknown_price_type"], 0);
    EXPECT_EQ(j["ext"]["unknown_position_effect"], 2);
    // 既有字段不被覆盖
    EXPECT_EQ(j["ext"]["api_version"], "v6.7.2");
    EXPECT_EQ(j["state"], "Ready");
    EXPECT_EQ(j["account_id"], "acc1");
}

TEST(TdStatusPayloadTest, ZeroCountsStillEmitted) {
    using namespace dztrader::ctp;
    // 计数为 0 也必须出现（前端与运维靠字段是否存在判断网关版本，缺失 = 老网关）。
    const nlohmann::json j = td_status_payload(TdStatus{}, UnknownEnumCounts{});
    ASSERT_TRUE(j.contains("ext"));
    EXPECT_EQ(j["ext"]["unknown_order_status"], 0);
    EXPECT_EQ(j["ext"]["unknown_direction"], 0);
    EXPECT_EQ(j["ext"]["unknown_price_type"], 0);
    EXPECT_EQ(j["ext"]["unknown_position_effect"], 0);
}

// ============================================================================
// RTN_TD_STATUS payload: last_error（契约 td-status §2.5）
// ============================================================================

TEST(TdStatusPayloadTest, LastErrorIsNullWhenNoFailure) {
    using namespace dztrader::ctp;
    // 无未清空失败时必须是显式 null（不是缺键、不是空对象）：前端用「非 null + timestamp
    // 变化」作登录 pending 清除信号，缺键与 null 必须同义。
    const nlohmann::json j = td_status_payload(TdStatus{}, UnknownEnumCounts{});
    ASSERT_TRUE(j.contains("last_error"));
    EXPECT_TRUE(j["last_error"].is_null());
}

TEST(TdStatusPayloadTest, LastErrorCarriesCodeMessageErrorIdAndTimestamp) {
    using namespace dztrader::ctp;
    TdStatus status;
    status.account_id = "acc1";
    status.state = TdState::AuthFailed;
    status.last_error = TdLastError{"auth_rejected", "CTP:用户认证失败", 3, 1'700'000'000};

    const nlohmann::json j = td_status_payload(status, UnknownEnumCounts{});
    ASSERT_TRUE(j["last_error"].is_object());
    EXPECT_EQ(j["last_error"]["code"], "auth_rejected");
    EXPECT_EQ(j["last_error"]["message"], "CTP:用户认证失败");
    EXPECT_EQ(j["last_error"]["error_id"], 3);
    EXPECT_EQ(j["last_error"]["timestamp"], 1'700'000'000);
}

TEST(TdStatusJsonTest, LastErrorIsSerializedWithStatus) {
    using namespace dztrader::ctp;
    // NLOHMANN 宏路径也要能带出 last_error（Payload 构造里 json = status 走这条）。
    TdStateMachine sm;
    sm.on_connect();
    sm.on_front_connected();
    sm.on_req_authenticate();
    sm.on_authenticate_failed(TdFailureCode::AuthTimeout, "10 秒内未收到响应", 0);

    const nlohmann::json j = sm.status();
    ASSERT_TRUE(j["last_error"].is_object());
    EXPECT_EQ(j["last_error"]["code"], "auth_timeout");
    EXPECT_EQ(j["last_error"]["error_id"], 0);
}

TEST(TdFailureCodeTest, CodeStringsMatchContract) {
    using namespace dztrader::ctp;
    // 线上码表（契约 td-status §2.5）：字符串即协议, 改名是破坏性变更。
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::ConnectTimeout)), "connect_timeout");
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::AuthRejected)), "auth_rejected");
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::AuthTimeout)), "auth_timeout");
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::LoginRejected)), "login_rejected");
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::LoginTimeout)), "login_timeout");
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::LoginParseError)),
              "login_parse_error");
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::DayLoadFailed)), "day_load_failed");
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::LoginSendFailed)),
              "login_send_failed");
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::SettlementRejected)),
              "settlement_rejected");
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::SettlementTimeout)),
              "settlement_timeout");
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::InstrumentsRejected)),
              "instruments_rejected");
    EXPECT_EQ(std::string(td_failure_code_str(TdFailureCode::InstrumentsTimeout)),
              "instruments_timeout");
}

TEST(TdFailureCodeTest, EveryCodeIsCoveredByTheContractTable) {
    // 契约 td-status §2.5 码表是 12 项；枚举漏一个码或改名, 上面那条逐项断言会红,
    // 但「新增枚举值却忘了补契约表」只有计数能钉住 —— 逐项断言不会因为多了一个值而失败。
    int reachable = 0;
    for (const auto code : magic_enum::enum_values<TdFailureCode>()) {
        EXPECT_NE(std::string(td_failure_code_str(code)), "unknown")
            << "every enum value must have a wire string: " << magic_enum::enum_name(code);
        ++reachable;
    }
    EXPECT_EQ(reachable, 12) << "contract td-status §2.5 lists 12 codes; keep them in sync";
}
