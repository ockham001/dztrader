#include "td/td_status_publish.h"

#include <gtest/gtest.h>

#include <unordered_map>
#include <vector>

using namespace dztrader::ctp;

// ============================================================================
// td_status_publish_key：哪些字段变化需要重推状态帧（契约 td-status §3 触发场景 1）
// ============================================================================

namespace {

TdStatus make_status(TdState state,
                     int progress_current,
                     const std::string& date,
                     const std::string& login_time,
                     const std::string& desc = "") {
    TdStatus status;
    status.state = state;
    status.progress_current = progress_current;
    status.date = date;
    status.login_time = login_time;
    status.progress_desc = desc;
    return status;
}

}  // namespace

TEST(TdStatusPublishKeyTest, StateTransitionChangesKey) {
    // 登录链每一档状态翻转都必须触发重推（否则 #409 的登录按钮/pending 不随结果变化）
    const auto idle = td_status_publish_key(make_status(TdState::Idle, 0, "", ""));
    const auto connecting = td_status_publish_key(make_status(TdState::Connecting, 1, "", ""));
    const auto authenticating =
        td_status_publish_key(make_status(TdState::Authenticating, 3, "", ""));
    const auto ready =
        td_status_publish_key(make_status(TdState::Ready, 10, "20261122", "2026-11-22 08:45:32"));
    EXPECT_FALSE(idle == connecting);
    EXPECT_FALSE(connecting == authenticating);
    EXPECT_FALSE(authenticating == ready);
    EXPECT_FALSE(ready == idle);
}

TEST(TdStatusPublishKeyTest, SameStateSameFieldsDoesNotChangeKey) {
    // 同一状态重复回报（SPI 重发/定时扫描）不得制造重复状态帧
    const auto a = td_status_publish_key(make_status(TdState::Ready, 10, "20261122", "08:45:32"));
    const auto b = td_status_publish_key(make_status(TdState::Ready, 10, "20261122", "08:45:32"));
    EXPECT_TRUE(a == b);
}

TEST(TdStatusPublishKeyTest, DateAndLoginTimeChangesChangeKey) {
    // 契约 td-status §3 触发场景 1：登录信息填充/清空、交易日变化都是状态字段变化
    const auto no_login = td_status_publish_key(make_status(TdState::Ready, 10, "", ""));
    const auto logged_in =
        td_status_publish_key(make_status(TdState::Ready, 10, "20261122", "2026-11-22 08:45:32"));
    const auto next_day =
        td_status_publish_key(make_status(TdState::Ready, 10, "20261123", "2026-11-22 08:45:32"));
    EXPECT_FALSE(no_login == logged_in);
    EXPECT_FALSE(logged_in == next_day);
    // 登出/失败回滚会清空 date 与 login_time（契约 td-connection：current 回到 min）
    EXPECT_FALSE(logged_in == no_login);
}

TEST(TdStatusPublishKeyTest, ProgressChangeAloneChangesKey) {
    const auto at_9 = td_status_publish_key(make_status(TdState::LoadingInstruments, 9, "", ""));
    const auto at_10 = td_status_publish_key(make_status(TdState::Ready, 10, "", ""));
    EXPECT_TRUE(at_9 == at_9);
    EXPECT_FALSE(at_9 == at_10);
}

TEST(TdStatusPublishKeyTest, ExtFieldsDoNotChangeKey) {
    // ext.sys_version / api_version 不是状态字段：不得让登录响应填充 ext 时多推一帧
    // （同一批状态字段变化里状态帧已经会带出 ext）。
    auto a = make_status(TdState::Ready, 10, "20261122", "08:45:32");
    auto b = a;
    b.ext.sys_version = "v6.7.2";
    b.ext.api_version = "v6.7.2";
    EXPECT_TRUE(td_status_publish_key(a) == td_status_publish_key(b));
}

// ============================================================================
// publish_changed_accounts：主循环 sweep 的判据
// ============================================================================

TEST(PublishChangedAccountsTest, NewSessionsArePublished) {
    const std::vector<TdSessionStatusSnapshot> sessions{
        {"account_001", td_status_publish_key(make_status(TdState::Connecting, 1, "", ""))},
    };
    const std::unordered_map<std::string, TdStatusPublishKey> published;
    EXPECT_EQ(publish_changed_accounts(sessions, published),
              std::vector<std::string>{"account_001"});
}

TEST(PublishChangedAccountsTest, UnchangedSessionsAreSkipped) {
    const auto key = td_status_publish_key(make_status(TdState::Ready, 10, "20261122", "08:45:32"));
    const std::vector<TdSessionStatusSnapshot> sessions{{"account_001", key}};
    const std::unordered_map<std::string, TdStatusPublishKey> published{{"account_001", key}};
    EXPECT_TRUE(publish_changed_accounts(sessions, published).empty());
}

TEST(PublishChangedAccountsTest, OnlyChangedSessionsArePublished) {
    const auto steady =
        td_status_publish_key(make_status(TdState::Ready, 10, "20261122", "08:45:32"));
    const std::vector<TdSessionStatusSnapshot> sessions{
        {"account_001", steady},
        {"account_002", td_status_publish_key(make_status(TdState::Disconnected, 1, "", ""))},
    };
    const std::unordered_map<std::string, TdStatusPublishKey> published{{"account_001", steady}};
    EXPECT_EQ(publish_changed_accounts(sessions, published),
              std::vector<std::string>{"account_002"});
}

TEST(PublishChangedAccountsTest, LogoutRollbackChangesArePublished) {
    // 登出/失败回滚：current 从 10 回到 0/1 也必须推（前端据此清 pending 并恢复登录按钮）
    const std::vector<TdSessionStatusSnapshot> sessions{
        {"account_001", td_status_publish_key(make_status(TdState::Idle, 0, "", ""))},
    };
    const std::unordered_map<std::string, TdStatusPublishKey> published{
        {"account_001",
         td_status_publish_key(make_status(TdState::Ready, 10, "20261122", "08:45:32"))},
    };
    EXPECT_EQ(publish_changed_accounts(sessions, published),
              std::vector<std::string>{"account_001"});
}
