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
// last_error.timestamp 参与重推判定（契约 td-status §2.5 重推判据）
// ============================================================================

TEST(TdStatusPublishKeyTest, LastErrorTimestampChangeAloneChangesKey) {
    // 关键场景：同一失败态下「原因变了」（先柜台拒绝、重试后超时）——state 与进度都不变，
    // 若 last_error 不进键，新原因永远到不了前端（卡片一直显示旧原因）。
    auto rejected = make_status(TdState::AuthFailed, 3, "", "", "认证失败");
    rejected.last_error = TdLastError{"auth_rejected", "CTP:用户认证失败", 3, 1'700'000'000};
    auto timed_out = rejected;
    timed_out.last_error = TdLastError{"auth_timeout", "10 秒内未收到响应", 0, 1'700'000'100};

    const auto a = td_status_publish_key(rejected);
    const auto b = td_status_publish_key(timed_out);
    EXPECT_EQ(a.state, b.state);
    EXPECT_EQ(a.progress_current, b.progress_current);
    EXPECT_FALSE(a == b) << "same state with a new failure reason must still be republished";
}

TEST(TdStatusPublishKeyTest, LastErrorClearedChangesKey) {
    // 清空 last_error（下一次尝试被受理 / 转 Ready）也必须触发重推：前端据此撤下
    // 「上次失败」展示，等下一帧推送才生效会残留旧原因。
    // 关键：**除 last_error 外所有字段都相同**——否则 state/进度变化本身就足以让断言通过，
    // 这条用例就证明不了 last_error 进了键（docs/unit-test-standard.md §1.1）。
    auto failed = make_status(TdState::AuthFailed, 3, "", "", "认证失败");
    failed.last_error = TdLastError{"auth_rejected", "CTP:用户认证失败", 3, 1'700'000'000};
    auto cleared = failed;
    cleared.last_error.reset();  // 只清 last_error

    const auto with_error = td_status_publish_key(failed);
    const auto without_error = td_status_publish_key(cleared);
    EXPECT_EQ(with_error.state, without_error.state) << "premise: state must be identical";
    EXPECT_EQ(with_error.progress_current, without_error.progress_current) << "premise";
    EXPECT_FALSE(with_error == without_error) << "clearing last_error alone must republish";
    EXPECT_TRUE(without_error.last_error_identity.empty());
}

TEST(TdStatusPublishKeyTest, ReasonChangeWithinSameSecondStillChangesKey) {
    // 时间戳只有秒级精度: 同一秒内「先柜台拒绝、重试后超时」时 timestamp 相同,
    // 若键只比 timestamp, 新原因既不重推、前端也不清 pending —— 正是本判据要覆盖的场景。
    auto rejected = make_status(TdState::AuthFailed, 3, "", "", "认证失败");
    rejected.last_error = TdLastError{"auth_rejected", "CTP:用户认证失败", 3, 1'700'000'000};
    auto timed_out = rejected;
    timed_out.last_error = TdLastError{"auth_timeout", "", 0, 1'700'000'000};  // 同一秒

    const auto a = td_status_publish_key(rejected);
    const auto b = td_status_publish_key(timed_out);
    EXPECT_EQ(a.state, b.state);
    EXPECT_FALSE(a == b) << "a different failure code in the same second must republish";
}

TEST(TdStatusPublishKeyTest, SameLastErrorDoesNotChangeKey) {
    // 同一次失败被重复观察（主循环每轮比较）不得制造重复状态帧。
    auto failed = make_status(TdState::AuthFailed, 3, "", "", "认证失败");
    failed.last_error = TdLastError{"auth_rejected", "CTP:用户认证失败", 3, 1'700'000'000};
    auto same = failed;
    EXPECT_TRUE(td_status_publish_key(failed) == td_status_publish_key(same));
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
