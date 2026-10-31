/**
 * @file session_guard_test.cpp
 * @brief 会话有效性判定（契约 rest §1「会话吊销」/「强制改密」）
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "jwt.h"
#include "repository.h"
#include "session_guard.h"

namespace dztrader::webui {
namespace {

class SessionGuardTest : public ::testing::Test {
protected:
    std::shared_ptr<Repository> repo_;
    int64_t uid_ = 0;

    void SetUp() override {
        repo_ = std::make_shared<Repository>(":memory:");
        uid_ = repo_->create_user("alice", "Alice", "a@t.com", "AlicePass123!", "user");
    }
};

TEST_F(SessionGuardTest, ValidWhenVersionMatchesAndNoForcedChange) {
    const auto user = repo_->get_user_by_id(uid_);
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(check_session(*repo_, "alice", user->token_version), SessionState::Valid);
}

TEST_F(SessionGuardTest, UnknownUserIsInvalid) {
    EXPECT_EQ(check_session(*repo_, "ghost", 0), SessionState::InvalidToken);
}

TEST_F(SessionGuardTest, StaleVersionIsInvalid) {
    // 改密 / 重置 / 禁用后 token_version 自增，旧 token 的 ver 不再相等
    ASSERT_TRUE(repo_->revoke_user_tokens(uid_));
    const auto user = repo_->get_user_by_id(uid_);
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(check_session(*repo_, "alice", user->token_version - 1), SessionState::InvalidToken);
    EXPECT_EQ(check_session(*repo_, "alice", user->token_version), SessionState::Valid);
}

TEST_F(SessionGuardTest, DisabledUserIsInvalidEvenWithMatchingVersion) {
    const auto user = repo_->get_user_by_id(uid_);
    ASSERT_TRUE(user.has_value());
    repo_->update_user_status(uid_, "disabled");
    // 版本号未变也必须拒绝（禁用是即时语义，不依赖额外吊销）
    EXPECT_EQ(check_session(*repo_, "alice", user->token_version), SessionState::InvalidToken);
}

TEST_F(SessionGuardTest, MustChangePasswordIsItsOwnState) {
    const auto user = repo_->get_user_by_id(uid_);
    ASSERT_TRUE(user.has_value());
    ASSERT_TRUE(repo_->admin_reset_password(uid_, "ResetPass123!"));
    const auto after = repo_->get_user_by_id(uid_);
    ASSERT_TRUE(after.has_value());
    ASSERT_EQ(after->must_change_password, 1);
    EXPECT_EQ(check_session(*repo_, "alice", after->token_version),
              SessionState::PasswordChangeRequired);
}

TEST_F(SessionGuardTest, ChangeOwnPasswordClearsForcedChange) {
    ASSERT_TRUE(repo_->admin_reset_password(uid_, "ResetPass123!"));
    ASSERT_TRUE(repo_->change_own_password(uid_, "MyNewPass123!"));
    const auto after = repo_->get_user_by_id(uid_);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->must_change_password, 0);
    EXPECT_EQ(check_session(*repo_, "alice", after->token_version), SessionState::Valid);
}

// 端到端：签发 token → 管理员重置 → 旧 token 被拒（契约 rest §1「会话吊销」）
// 这是 issue「补测试」要求的直接断言：前面几条分别验版本自增与版本比对，
// 这里把两者串起来验「旧 token 真的不能再用」，避免只测半截。
TEST_F(SessionGuardTest, OldTokenRejectedAfterAdminReset) {
    const std::string secret = "test_secret";
    const auto before = repo_->get_user_by_username("alice");
    ASSERT_TRUE(before.has_value());

    // 1. 用当前 token_version 签发一个 token（等价于用户此刻登录拿到的那个）
    const std::string token = jwt_sign("alice", 3600, secret, before->token_version);
    std::string sub;
    int64_t ver = -1;
    ASSERT_TRUE(jwt_verify(token, secret, sub, &ver));
    ASSERT_EQ(check_session(*repo_, sub, ver), SessionState::Valid);

    // 2. 管理员重置口令 → token_version 自增 + 强制改密
    ASSERT_TRUE(repo_->admin_reset_password(uid_, "ResetPass123!"));

    // 3. 同一个 token 再走一次会话判定：签名仍有效，但会话必须已失效
    std::string sub2;
    int64_t ver2 = -1;
    ASSERT_TRUE(jwt_verify(token, secret, sub2, &ver2)) << "签名不该因重置而变化";
    // 版本已过时 + 强制改密；两者都不能让 token 继续可用（此处先撞版本不匹配）
    EXPECT_EQ(check_session(*repo_, sub2, ver2), SessionState::InvalidToken);
}

// 端到端：自助改密后**另一个**会话的旧 token 也被吊销（不只当前会话）
TEST_F(SessionGuardTest, OtherSessionsInvalidatedByPasswordChange) {
    const std::string secret = "test_secret";
    const auto before = repo_->get_user_by_username("alice");
    ASSERT_TRUE(before.has_value());
    const std::string other_device_token = jwt_sign("alice", 3600, secret, before->token_version);

    ASSERT_TRUE(repo_->change_own_password(uid_, "MyNewPass123!"));

    std::string sub;
    int64_t ver = -1;
    ASSERT_TRUE(jwt_verify(other_device_token, secret, sub, &ver));
    EXPECT_EQ(check_session(*repo_, sub, ver), SessionState::InvalidToken);
}

TEST_F(SessionGuardTest, PathWhitelistBeforePasswordChange) {
    // 契约 rest §1「强制改密」：只放行改密本身与「我是谁」
    EXPECT_TRUE(is_path_allowed_before_password_change("/api/auth/change-password"));
    EXPECT_TRUE(is_path_allowed_before_password_change("/api/user/me"));
    EXPECT_FALSE(is_path_allowed_before_password_change("/api/user"));
    EXPECT_FALSE(is_path_allowed_before_password_change("/api/security/config"));
    EXPECT_FALSE(is_path_allowed_before_password_change("/api/market-sources"));
    EXPECT_FALSE(is_path_allowed_before_password_change(""));
}

}  // namespace
}  // namespace dztrader::webui
