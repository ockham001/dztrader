#include <gtest/gtest.h>
#include "repository.h"
#include "db_init.h"
#include <algorithm>

namespace dztrader::webui {
namespace {

class RepositoryTest : public ::testing::Test {
protected:
    Repository repo_{":memory:"};
};

TEST_F(RepositoryTest, CreateAndGetUser) {
    const int64_t id =
        repo_.create_user("testuser", "Test User", "test@test.com", "password123", "user");
    ASSERT_GT(id, 0);

    auto user = repo_.get_user_by_username("testuser");
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(user->username, "testuser");
    EXPECT_EQ(user->display_name, "Test User");
    EXPECT_EQ(user->role, "user");
    EXPECT_EQ(user->status, "offline");
}

TEST_F(RepositoryTest, PasswordHashAndVerify) {
    const std::string hash = hash_password("mypassword");
    EXPECT_TRUE(verify_password("mypassword", hash));
    EXPECT_FALSE(verify_password("wrongpassword", hash));
}

TEST_F(RepositoryTest, PasswordHashPbkdf2Format) {
    std::string hash = hash_password("test");
    EXPECT_EQ(hash.substr(0, 14), "pbkdf2_sha256$");
    // Format: pbkdf2_sha256$iterations$salt_hex$hash_hex → 3 '$' separators
    EXPECT_EQ(std::count(hash.begin(), hash.end(), '$'), 3);
}

TEST_F(RepositoryTest, PasswordHashRejectsLegacyFormat) {
    // Legacy single-pass SHA-256 format (salt_hex$hash_hex) must be rejected
    const std::string legacy =
        "0123456789abcdef0123456789abcdef$"
        "0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef";
    EXPECT_FALSE(verify_password("anything", legacy));
}

TEST_F(RepositoryTest, PasswordHashUniqueSalts) {
    const std::string h1 = hash_password("same");
    const std::string h2 = hash_password("same");
    EXPECT_NE(h1, h2);  // different salts → different hashes
    EXPECT_TRUE(verify_password("same", h1));
    EXPECT_TRUE(verify_password("same", h2));
}

TEST_F(RepositoryTest, PasswordHashRejectsMalformed) {
    EXPECT_FALSE(verify_password("x", ""));
    EXPECT_FALSE(verify_password("x", "pbkdf2_sha256$"));
    EXPECT_FALSE(verify_password("x", "pbkdf2_sha256$abc$salt$hash"));
    EXPECT_FALSE(verify_password("x", "pbkdf2_sha256$100000$short$hash"));
}

TEST_F(RepositoryTest, ListUsersWithFilter) {
    repo_.create_user("admin1", "Admin One", "a1@test.com", "pass", "admin");
    repo_.create_user("user1", "User One", "u1@test.com", "pass", "user");
    repo_.create_user("user2", "User Two", "u2@test.com", "pass", "user");

    auto result = repo_.list_users("", "user", "", 1, 10);
    EXPECT_EQ(result.total, 2);

    auto admin_result = repo_.list_users("", "admin", "", 1, 10);
    EXPECT_EQ(admin_result.total, 1);
}

TEST_F(RepositoryTest, UpdateUserStatus) {
    const int64_t id = repo_.create_user("statususer", "Status User", "", "pass", "user");
    EXPECT_TRUE(repo_.update_user_status(id, "disabled"));
    auto user = repo_.get_user_by_id(id);
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(user->status, "disabled");
}

TEST_F(RepositoryTest, FailedLoginIncrement) {
    repo_.create_user("loginuser", "Login User", "", "pass", "user");
    repo_.increment_failed_login("loginuser");
    repo_.increment_failed_login("loginuser");
    auto user = repo_.get_user_by_username("loginuser");
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(user->failed_login_count, 2);

    repo_.reset_failed_login("loginuser");
    user = repo_.get_user_by_username("loginuser");
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(user->failed_login_count, 0);
}

TEST_F(RepositoryTest, SecurityConfigDefaults) {
    auto config = repo_.get_security_config();
    EXPECT_TRUE(config.login_lockout_enabled);
    EXPECT_EQ(config.access_mode, "auto");
    EXPECT_EQ(config.max_failed_attempts, 5);
}

TEST_F(RepositoryTest, IpBlacklist) {
    repo_.add_to_blacklist("192.168.1.100", "test reason", "manual");
    EXPECT_TRUE(repo_.is_ip_blacklisted("192.168.1.100"));
    EXPECT_FALSE(repo_.is_ip_blacklisted("192.168.1.101"));

    auto list = repo_.list_blacklist();
    EXPECT_EQ(list.size(), 1);
    EXPECT_EQ(list[0].ip, "192.168.1.100");
}

TEST_F(RepositoryTest, MarketSourceCRUD) {
    const int64_t id = repo_.create_market_source("CTP", "ctp_futures", "CTP期货");
    ASSERT_GT(id, 0);

    auto src = repo_.get_market_source(id);
    ASSERT_TRUE(src.has_value());
    EXPECT_EQ(src->source_type, "CTP");
    EXPECT_EQ(src->source_name, "ctp_futures");

    EXPECT_TRUE(repo_.update_market_source(id, "CTP期货更新"));
    src = repo_.get_market_source(id);
    ASSERT_TRUE(src.has_value());
    EXPECT_EQ(src->display_name, "CTP期货更新");

    auto list = repo_.list_market_sources();
    EXPECT_EQ(list.size(), 1);

    EXPECT_TRUE(repo_.delete_market_source(id));
    auto deleted = repo_.get_market_source(id);
    EXPECT_FALSE(deleted.has_value());
}

TEST_F(RepositoryTest, MarketSourceIsAddedLifecycle) {
    const int64_t id = repo_.create_market_source("ctp", "dzmd_ctp", "CTP");
    // remove 标记
    EXPECT_TRUE(repo_.set_market_source_added(id, false));
    auto s = repo_.get_market_source(id);
    ASSERT_TRUE(s.has_value());
    EXPECT_FALSE(s->is_added);
    // 再次添加：复用行并复位 is_added=1（ON CONFLICT DO UPDATE）
    const int64_t id2 = repo_.create_market_source("ctp", "dzmd_ctp", "CTP 重启");
    EXPECT_EQ(id2, id);
    auto s2 = repo_.get_market_source(id2);
    ASSERT_TRUE(s2.has_value());
    EXPECT_TRUE(s2->is_added);
    EXPECT_EQ(s2->display_name, "CTP");  // 复用行保留现有 display_name（再添加不覆盖——契约 rest §2.3 修订）
}

TEST_F(RepositoryTest, LoginHistory) {
    repo_.add_login_history("admin", "192.168.1.1", "Chrome/Windows", true);
    repo_.add_login_history("admin", "192.168.1.1", "Chrome/Windows", false);
    repo_.add_login_history("user1", "10.0.0.1", "Safari/macOS", true);

    auto result = repo_.get_login_history(1, 10);
    EXPECT_EQ(result.total, 3);
}

TEST_F(RepositoryTest, Permissions) {
    const int64_t uid = repo_.create_user("permuser", "Perm User", "", "pass", "user");
    repo_.set_user_permission(uid, "account", "CTP-主力账户", true);
    repo_.set_user_permission(uid, "strategy", "MACD_V3", true);
    repo_.set_user_permission(uid, "strategy", "Grid_BTC", false);

    auto perms = repo_.get_user_permissions(uid);
    EXPECT_EQ(perms.size(), 3);
}

}  // anonymous namespace
}  // namespace dztrader::webui
