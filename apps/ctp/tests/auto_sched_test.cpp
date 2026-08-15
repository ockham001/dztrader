#include "common/trading_calendar.h"

#include <gtest/gtest.h>

using namespace dztrader::ctp;

// ============================================================
// is_trading_window
// ============================================================

TEST(IsTradingWindow, MondayBefore0600IsFalse) {
    EXPECT_FALSE(is_trading_window(1, "00:00"));
    EXPECT_FALSE(is_trading_window(1, "05:59"));
}

TEST(IsTradingWindow, MondayAt0600IsTrue) {
    EXPECT_TRUE(is_trading_window(1, "06:00"));
    EXPECT_TRUE(is_trading_window(1, "23:59"));
}

TEST(IsTradingWindow, TuesdayToFridayAlwaysTrue) {
    for (int w = 2; w <= 5; ++w) {
        EXPECT_TRUE(is_trading_window(w, "00:00"));
        EXPECT_TRUE(is_trading_window(w, "12:00"));
        EXPECT_TRUE(is_trading_window(w, "23:59"));
    }
}

TEST(IsTradingWindow, SaturdayBefore0600IsTrue) {
    EXPECT_TRUE(is_trading_window(6, "00:00"));
    EXPECT_TRUE(is_trading_window(6, "05:59"));
}

TEST(IsTradingWindow, SaturdayAt0600IsFalse) {
    EXPECT_FALSE(is_trading_window(6, "06:00"));
    EXPECT_FALSE(is_trading_window(6, "23:59"));
}

TEST(IsTradingWindow, SundayAlwaysFalse) {
    EXPECT_FALSE(is_trading_window(7, "00:00"));
    EXPECT_FALSE(is_trading_window(7, "12:00"));
    EXPECT_FALSE(is_trading_window(7, "23:59"));
}

// ============================================================
// AutoSchedConfig 概念 (模板可复用性验证)
// ============================================================

// 模拟 td 网关的 Config, 验证只需 enable_auto_login_logout + schedules 两字段即可复用
struct FakeTdConfig {
    bool enable_auto_login_logout = true;
    std::vector<Schedule> schedules;
    std::string td_specific_field;  // td 特有字段, 不影响 concept
};

// 编译期验证: AutoLoginSchedView 和 FakeTdConfig 都满足 AutoSchedConfig concept
static_assert(AutoSchedConfig<AutoLoginSchedView>, "AutoLoginSchedView should satisfy AutoSchedConfig");
static_assert(AutoSchedConfig<FakeTdConfig>, "FakeTdConfig should satisfy AutoSchedConfig");

TEST(AutoSchedConfigConcept, AcceptsAutoLoginSchedView) {
    AutoLoginSchedView cfg;
    cfg.enable_auto_login_logout = true;
    cfg.schedules.push_back({"09:00", "15:30"});
    EXPECT_TRUE(should_be_logged_in(cfg, "10:00"));
}

TEST(AutoSchedConfigConcept, AcceptsFakeTdConfig) {
    FakeTdConfig cfg;
    cfg.enable_auto_login_logout = true;
    cfg.schedules.push_back({"09:00", "15:30"});
    EXPECT_TRUE(should_be_logged_in(cfg, "10:00"));
    EXPECT_EQ(evaluate_sched_action(cfg, 2, "09:00", false), AutoSchedAction::Login);
}

// ============================================================
// should_be_logged_in
// ============================================================

namespace {

AutoLoginSchedView make_cfg_with_schedule(const std::string& login, const std::string& logout,
                                           bool enable = true) {
    AutoLoginSchedView cfg;
    cfg.enable_auto_login_logout = enable;
    cfg.schedules.push_back({login, logout});
    return cfg;
}

}  // namespace

TEST(ShouldBeLoggedIn, SameDaySchedule) {
    auto cfg = make_cfg_with_schedule("09:00", "15:30");
    EXPECT_FALSE(should_be_logged_in(cfg, "08:59"));
    EXPECT_TRUE(should_be_logged_in(cfg, "09:00"));
    EXPECT_TRUE(should_be_logged_in(cfg, "15:29"));
    EXPECT_FALSE(should_be_logged_in(cfg, "15:30"));
}

TEST(ShouldBeLoggedIn, CrossMidnightSchedule) {
    auto cfg = make_cfg_with_schedule("21:00", "02:30");
    EXPECT_FALSE(should_be_logged_in(cfg, "20:59"));
    EXPECT_TRUE(should_be_logged_in(cfg, "21:00"));
    EXPECT_TRUE(should_be_logged_in(cfg, "23:59"));
    EXPECT_TRUE(should_be_logged_in(cfg, "00:00"));
    EXPECT_TRUE(should_be_logged_in(cfg, "02:29"));
    EXPECT_FALSE(should_be_logged_in(cfg, "02:30"));
    EXPECT_FALSE(should_be_logged_in(cfg, "12:00"));  // 中午不在区间内
}

TEST(ShouldBeLoggedIn, MultipleSchedules) {
    AutoLoginSchedView cfg;
    cfg.enable_auto_login_logout = true;
    cfg.schedules.push_back({"09:00", "15:30"});
    cfg.schedules.push_back({"21:00", "02:30"});
    EXPECT_TRUE(should_be_logged_in(cfg, "10:00"));   // 第一个区间
    EXPECT_TRUE(should_be_logged_in(cfg, "22:00"));   // 第二个区间
    EXPECT_TRUE(should_be_logged_in(cfg, "01:00"));   // 第二个区间跨午夜
    EXPECT_FALSE(should_be_logged_in(cfg, "16:00"));  // 两区间之外
}

TEST(ShouldBeLoggedIn, DisabledReturnsFalse) {
    auto cfg = make_cfg_with_schedule("09:00", "15:30", false);
    EXPECT_FALSE(should_be_logged_in(cfg, "10:00"));
}

TEST(ShouldBeLoggedIn, EmptySchedulesReturnsFalse) {
    AutoLoginSchedView cfg;
    cfg.enable_auto_login_logout = true;
    EXPECT_FALSE(should_be_logged_in(cfg, "10:00"));
}

TEST(ShouldBeLoggedIn, LoginEqualsLogoutIsAlwaysFalse) {
    // login == logout: 区间为空, 永远 false
    auto cfg = make_cfg_with_schedule("09:00", "09:00");
    EXPECT_FALSE(should_be_logged_in(cfg, "09:00"));
    EXPECT_FALSE(should_be_logged_in(cfg, "10:00"));
}

// ============================================================
// evaluate_sched_action
// ============================================================

TEST(Evaluate, DisabledReturnsNone) {
    auto cfg = make_cfg_with_schedule("09:00", "15:30", false);
    // 周二 09:00, 未登录, 但 enable=false
    EXPECT_EQ(evaluate_sched_action(cfg, 2, "09:00", false), AutoSchedAction::None);
}

TEST(Evaluate, NonTradingWindowReturnsNone) {
    auto cfg = make_cfg_with_schedule("09:00", "15:30");
    // 周日 09:00, 未登录, 但非交易日窗口
    EXPECT_EQ(evaluate_sched_action(cfg, 7, "09:00", false), AutoSchedAction::None);
    // 周一 05:59 (窗口前)
    EXPECT_EQ(evaluate_sched_action(cfg, 1, "05:59", false), AutoSchedAction::None);
    // 周六 06:00 (窗口后)
    EXPECT_EQ(evaluate_sched_action(cfg, 6, "06:00", false), AutoSchedAction::None);
}

TEST(Evaluate, LoginTimeWhenNotLoggedInReturnsLogin) {
    auto cfg = make_cfg_with_schedule("09:00", "15:30");
    EXPECT_EQ(evaluate_sched_action(cfg, 2, "09:00", false), AutoSchedAction::Login);
}

TEST(Evaluate, LoginTimeWhenAlreadyLoggedInReturnsNone) {
    auto cfg = make_cfg_with_schedule("09:00", "15:30");
    EXPECT_EQ(evaluate_sched_action(cfg, 2, "09:00", true), AutoSchedAction::None);
}

TEST(Evaluate, LogoutTimeWhenLoggedInReturnsLogout) {
    auto cfg = make_cfg_with_schedule("09:00", "15:30");
    EXPECT_EQ(evaluate_sched_action(cfg, 2, "15:30", true), AutoSchedAction::Logout);
}

TEST(Evaluate, LogoutTimeWhenNotLoggedInReturnsNone) {
    auto cfg = make_cfg_with_schedule("09:00", "15:30");
    EXPECT_EQ(evaluate_sched_action(cfg, 2, "15:30", false), AutoSchedAction::None);
}

TEST(Evaluate, NonMatchingTimeReturnsNone) {
    auto cfg = make_cfg_with_schedule("09:00", "15:30");
    EXPECT_EQ(evaluate_sched_action(cfg, 2, "10:00", false), AutoSchedAction::None);
    EXPECT_EQ(evaluate_sched_action(cfg, 2, "10:00", true), AutoSchedAction::None);
}

TEST(Evaluate, LogoutPrioritizedOverLogin) {
    // 两 schedules: s1 logout 在 15:30, s2 login 在 15:30
    // 同一时刻匹配两者, 优先 logout
    AutoLoginSchedView cfg;
    cfg.enable_auto_login_logout = true;
    cfg.schedules.push_back({"09:00", "15:30"});
    cfg.schedules.push_back({"15:30", "23:00"});
    EXPECT_EQ(evaluate_sched_action(cfg, 2, "15:30", true), AutoSchedAction::Logout);
}

TEST(Evaluate, CrossMidnightLoginLogout) {
    auto cfg = make_cfg_with_schedule("21:00", "02:30");
    // 周二 21:00 login
    EXPECT_EQ(evaluate_sched_action(cfg, 2, "21:00", false), AutoSchedAction::Login);
    // 周三 02:30 logout (跨午夜, 但星期窗口: 周三全天允许)
    EXPECT_EQ(evaluate_sched_action(cfg, 3, "02:30", true), AutoSchedAction::Logout);
}
