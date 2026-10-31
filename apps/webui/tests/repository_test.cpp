#include <gtest/gtest.h>
#include "repository.h"
#include "db_init.h"
#include <sqlite3.h>
#include <dztrader/core/this_process.h>
#include <algorithm>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>

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

TEST_F(RepositoryTest, CreateUserRejectsDuplicateUsername) {
    const int64_t id = repo_.create_user("dupuser", "First", "", "pass", "user");
    ASSERT_GT(id, 0);

    // UNIQUE(username) 冲突必须显式失败，不得静默返回旧 rowid（旧实现返回首次插入的 id）
    EXPECT_THROW(repo_.create_user("dupuser", "Second", "", "pass2", "admin"), std::runtime_error);

    // 原用户与用户总数均未被改动
    const auto users = repo_.list_users("", "", "", 1, 10);
    EXPECT_EQ(users.total, 1);
    auto user = repo_.get_user_by_username("dupuser");
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(user->id, id);
    EXPECT_EQ(user->display_name, "First");
    EXPECT_EQ(user->role, "user");
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
    const int64_t admin_id =
        repo_.create_user("admin_one", "Alice Admin", "a1@test.com", "pass", "admin");
    const int64_t user1 =
        repo_.create_user("user_one", "Bob O'Brien", "u1@test.com", "pass", "user");
    const int64_t user2 = repo_.create_user("user_two", "Carol 100%", "", "pass", "user");
    const int64_t user3 = repo_.create_user("user_three", "Dave", "", "pass", "user");
    ASSERT_TRUE(repo_.update_user_status(user2, "disabled"));

    // role 过滤：断言内容而非只有总数
    auto result = repo_.list_users("", "user", "", 1, 10);
    EXPECT_EQ(result.total, 3);
    ASSERT_EQ(result.users.size(), 3u);
    EXPECT_EQ(result.users[0].id, user1);
    EXPECT_EQ(result.users[1].id, user2);
    EXPECT_EQ(result.users[2].id, user3);

    auto admin_result = repo_.list_users("", "admin", "", 1, 10);
    EXPECT_EQ(admin_result.total, 1);
    ASSERT_EQ(admin_result.users.size(), 1u);
    EXPECT_EQ(admin_result.users[0].id, admin_id);

    // search 命中 username
    auto by_username = repo_.list_users("user_on", "", "", 1, 10);
    EXPECT_EQ(by_username.total, 1);
    ASSERT_EQ(by_username.users.size(), 1u);
    EXPECT_EQ(by_username.users[0].id, user1);

    // search 命中 display_name（含单引号，绑定参数而非字符串拼接）
    auto by_display = repo_.list_users("O'Brien", "", "", 1, 10);
    EXPECT_EQ(by_display.total, 1);
    ASSERT_EQ(by_display.users.size(), 1u);
    EXPECT_EQ(by_display.users[0].id, user1);

    // 注入样式输入只按字面量匹配，不得返回全量
    auto injected = repo_.list_users("' OR 1=1 --", "", "", 1, 10);
    EXPECT_EQ(injected.total, 0);
    EXPECT_TRUE(injected.users.empty());

    // search 含 %（LIKE 元字符/注入面）：输入照常绑定，不得报错或返回全量
    auto percent = repo_.list_users("100%", "", "", 1, 10);
    EXPECT_EQ(percent.total, 1);
    ASSERT_EQ(percent.users.size(), 1u);
    EXPECT_EQ(percent.users[0].id, user2);

    // status 过滤
    auto disabled = repo_.list_users("", "", "disabled", 1, 10);
    EXPECT_EQ(disabled.total, 1);
    ASSERT_EQ(disabled.users.size(), 1u);
    EXPECT_EQ(disabled.users[0].id, user2);
    EXPECT_EQ(disabled.users[0].status, "disabled");

    // page>1 切片：ORDER BY id, LIMIT/OFFSET
    auto page1 = repo_.list_users("", "", "", 1, 2);
    ASSERT_EQ(page1.total, 4);
    ASSERT_EQ(page1.users.size(), 2u);
    EXPECT_EQ(page1.users[0].id, admin_id);
    EXPECT_EQ(page1.users[1].id, user1);

    auto page2 = repo_.list_users("", "", "", 2, 2);
    ASSERT_EQ(page2.total, 4);
    ASSERT_EQ(page2.users.size(), 2u);
    EXPECT_EQ(page2.users[0].id, user2);
    EXPECT_EQ(page2.users[1].id, user3);

    auto page3 = repo_.list_users("", "", "", 3, 2);
    EXPECT_EQ(page3.total, 4);
    EXPECT_TRUE(page3.users.empty());
}

TEST_F(RepositoryTest, MutatorsRejectUnknownIdsWithoutSideEffects) {
    const int64_t uid = repo_.create_user("mutuser", "Mut User", "mut@test.com", "pass", "user");
    const int64_t src_id = repo_.create_market_source("CTP", "mut_src", "Mut Src");
    ASSERT_TRUE(repo_.add_to_blacklist("10.0.0.9", "reason", "manual"));

    const auto hash_before = repo_.get_password_hash("mutuser");
    ASSERT_TRUE(hash_before.has_value());

    constexpr int64_t kMissingId = 999999;
    EXPECT_FALSE(repo_.update_user(kMissingId, "Changed", "x@test.com", "admin"));
    EXPECT_FALSE(repo_.delete_user(kMissingId));
    EXPECT_FALSE(repo_.update_user_status(kMissingId, "disabled"));
    EXPECT_FALSE(repo_.admin_reset_password(kMissingId, "new-password"));
    EXPECT_FALSE(repo_.change_own_password(kMissingId, "new-password"));
    EXPECT_FALSE(repo_.revoke_user_tokens(kMissingId));
    EXPECT_FALSE(repo_.force_password_change_if_default(kMissingId, "pass"));
    EXPECT_FALSE(repo_.ack_default_password(kMissingId));
    EXPECT_FALSE(repo_.update_market_source(kMissingId, "Changed"));
    EXPECT_FALSE(repo_.delete_market_source(kMissingId));
    EXPECT_FALSE(repo_.set_market_source_added(kMissingId, false));
    EXPECT_FALSE(repo_.remove_from_blacklist(kMissingId));

    // 数据库状态未被改动
    auto user = repo_.get_user_by_id(uid);
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(user->display_name, "Mut User");
    EXPECT_EQ(user->email, "mut@test.com");
    EXPECT_EQ(user->role, "user");
    EXPECT_EQ(user->status, "offline");
    EXPECT_EQ(user->default_password_acknowledged, 0);
    EXPECT_EQ(repo_.get_password_hash("mutuser"), hash_before);

    auto src = repo_.get_market_source(src_id);
    ASSERT_TRUE(src.has_value());
    EXPECT_EQ(src->display_name, "Mut Src");
    EXPECT_TRUE(src->is_added);

    const auto blacklist = repo_.list_blacklist();
    ASSERT_EQ(blacklist.size(), 1u);
    EXPECT_EQ(blacklist[0].ip, "10.0.0.9");
}

// ---- #202 会话吊销与强制改密 ----

TEST_F(RepositoryTest, AdminResetPasswordBumpsVersionAndForcesChange) {
    const int64_t id = repo_.create_user("resetuser", "Reset User", "", "oldpassword123", "user");
    const auto before = repo_.get_user_by_id(id);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->token_version, 0);
    EXPECT_EQ(before->must_change_password, 0);

    ASSERT_TRUE(repo_.admin_reset_password(id, "ResetPass123!"));
    const auto after = repo_.get_user_by_id(id);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->token_version, before->token_version + 1);
    EXPECT_EQ(after->must_change_password, 1);
    // 新口令生效
    auto hash = repo_.get_password_hash("resetuser");
    ASSERT_TRUE(hash.has_value());
    EXPECT_TRUE(verify_password("ResetPass123!", *hash));
}

TEST_F(RepositoryTest, ChangeOwnPasswordBumpsVersionAndClearsForcedChange) {
    const int64_t id = repo_.create_user("ownuser", "Own User", "", "oldpassword123", "user");
    ASSERT_TRUE(repo_.admin_reset_password(id, "ResetPass123!"));
    const auto mid = repo_.get_user_by_id(id);
    ASSERT_TRUE(mid.has_value());
    ASSERT_EQ(mid->must_change_password, 1);

    ASSERT_TRUE(repo_.change_own_password(id, "MyNewPass123!"));
    const auto after = repo_.get_user_by_id(id);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->token_version, mid->token_version + 1);
    EXPECT_EQ(after->must_change_password, 0);
    auto hash = repo_.get_password_hash("ownuser");
    ASSERT_TRUE(hash.has_value());
    EXPECT_TRUE(verify_password("MyNewPass123!", *hash));
}

TEST_F(RepositoryTest, RevokeUserTokensOnlyBumpsVersion) {
    const int64_t id = repo_.create_user("revokeuser", "Revoke", "", "pw1234567890", "user");
    const auto hash_before = repo_.get_password_hash("revokeuser");
    ASSERT_TRUE(repo_.revoke_user_tokens(id));
    const auto after = repo_.get_user_by_id(id);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->token_version, 1);
    EXPECT_EQ(after->must_change_password, 0);
    // 吊销不动口令
    EXPECT_EQ(repo_.get_password_hash("revokeuser"), hash_before);
}

TEST_F(RepositoryTest, ForcePasswordChangeIfDefaultOnlyForDefaultPassword) {
    const int64_t id = repo_.create_user("defuser", "Def", "", "NotTheDefault123!", "user");
    // 口令不是给定默认值 → 不置位
    EXPECT_FALSE(repo_.force_password_change_if_default(id, "88888888"));
    const auto untouched = repo_.get_user_by_id(id);
    ASSERT_TRUE(untouched.has_value());
    EXPECT_EQ(untouched->must_change_password, 0);
    // 口令正是给定默认值 → 置位
    const int64_t def_id = repo_.create_user("defuser2", "Def2", "", "88888888", "user");
    EXPECT_TRUE(repo_.force_password_change_if_default(def_id, "88888888"));
    const auto flagged = repo_.get_user_by_id(def_id);
    ASSERT_TRUE(flagged.has_value());
    EXPECT_EQ(flagged->must_change_password, 1);
}

TEST_F(RepositoryTest, PasswordMatchesReflectsCurrentHash) {
    const int64_t id = repo_.create_user("matchuser", "Match", "", "88888888", "user");
    EXPECT_TRUE(repo_.password_matches("matchuser", "88888888"));
    ASSERT_TRUE(repo_.change_own_password(id, "NewStrongPass1"));
    // 改密后不再匹配系统默认口令——默认口令告警据此不再谎报
    EXPECT_FALSE(repo_.password_matches("matchuser", "88888888"));
    EXPECT_TRUE(repo_.password_matches("matchuser", "NewStrongPass1"));
    // 不存在的用户
    EXPECT_FALSE(repo_.password_matches("ghost", "whatever"));
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
    EXPECT_EQ(s2->display_name,
              "CTP");  // 复用行保留现有 display_name（再添加不覆盖——契约 rest §2.3 修订）
}

TEST_F(RepositoryTest, LoginHistory) {
    repo_.add_login_history("admin", "192.168.1.1", "Chrome/Windows", true, "UA-chrome", "");
    repo_.add_login_history("admin", "192.168.1.2", "Firefox/Linux", false, "UA-firefox",
                            "bad password");
    repo_.add_login_history("user1", "10.0.0.1", "Safari/macOS", true);

    auto result = repo_.get_login_history(1, 10);
    ASSERT_EQ(result.total, 3);
    ASSERT_EQ(result.entries.size(), 3u);

    // id DESC：最后写入的最先返回，字段逐项映射
    EXPECT_GT(result.entries[0].id, result.entries[1].id);
    EXPECT_GT(result.entries[1].id, result.entries[2].id);

    EXPECT_EQ(result.entries[0].username, "user1");
    EXPECT_EQ(result.entries[0].ip, "10.0.0.1");
    EXPECT_EQ(result.entries[0].device, "Safari/macOS");
    EXPECT_TRUE(result.entries[0].success);
    EXPECT_EQ(result.entries[0].user_agent, "");  // 未提供 → NULL → 空串
    EXPECT_EQ(result.entries[0].reason, "");

    EXPECT_EQ(result.entries[1].username, "admin");
    EXPECT_EQ(result.entries[1].ip, "192.168.1.2");
    EXPECT_EQ(result.entries[1].device, "Firefox/Linux");
    EXPECT_FALSE(result.entries[1].success);
    EXPECT_EQ(result.entries[1].user_agent, "UA-firefox");
    EXPECT_EQ(result.entries[1].reason, "bad password");

    EXPECT_EQ(result.entries[2].username, "admin");
    EXPECT_EQ(result.entries[2].ip, "192.168.1.1");
    EXPECT_TRUE(result.entries[2].success);
    EXPECT_EQ(result.entries[2].user_agent, "UA-chrome");

    // 分页
    auto page2 = repo_.get_login_history(2, 2);
    ASSERT_EQ(page2.total, 3);
    ASSERT_EQ(page2.entries.size(), 1u);
    EXPECT_EQ(page2.entries[0].ip, "192.168.1.1");

    // start/end 显式区间优先于 days
    auto in_range =
        repo_.get_login_history(1, 10, 1, "2000-01-01T00:00:00Z", "9999-12-31T23:59:59Z");
    EXPECT_EQ(in_range.total, 3);
    auto future = repo_.get_login_history(1, 10, 1, "9999-01-01T00:00:00Z", "");
    EXPECT_EQ(future.total, 0);
    auto past = repo_.get_login_history(1, 10, 1, "", "2000-01-01T00:00:00Z");
    EXPECT_EQ(past.total, 0);
}

TEST(RepositoryLoginHistoryFilterTest, DaysFilterExcludesOldEntries) {
    // 作用域退出即删除临时库文件（含 ASSERT 提前返回路径）
    struct TempDbFileGuard {
        std::filesystem::path path;
        ~TempDbFileGuard() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    };

    std::random_device rd;
    TempDbFileGuard db_guard{std::filesystem::temp_directory_path() /
                             ("dz_webui_repo_history_" +
                              std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
                              "_" + std::to_string(rd()))};
    const auto& db_path = db_guard.path;
    std::filesystem::remove_all(db_path);

    {
        Repository repo(db_path);

        // 直接写入一条 2000 年的旧记录（Repository 没有自定义时间入口）
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(db_path.string().c_str(), &raw), SQLITE_OK);
        char* err = nullptr;
        const int rc = sqlite3_exec(
            raw,
            "INSERT INTO login_history (username, ip, device, success, created_at, user_agent, "
            "reason) VALUES ('old', '1.2.3.4', 'old-device', 1, '2000-01-01T00:00:00Z', NULL, "
            "NULL)",
            nullptr, nullptr, &err);
        if (err != nullptr) {
            sqlite3_free(err);
        }
        sqlite3_close(raw);
        ASSERT_EQ(rc, SQLITE_OK);

        repo.add_login_history("now", "5.6.7.8", "new-device", true);

        const auto recent = repo.get_login_history(1, 10, /*days=*/1);
        EXPECT_EQ(recent.total, 1);
        ASSERT_EQ(recent.entries.size(), 1u);
        EXPECT_EQ(recent.entries[0].username, "now");

        const auto wide = repo.get_login_history(1, 10, /*days=*/100000);
        EXPECT_EQ(wide.total, 2);
        ASSERT_EQ(wide.entries.size(), 2u);
        EXPECT_EQ(wide.entries[0].username, "now");  // id DESC
        EXPECT_EQ(wide.entries[1].username, "old");
    }
}

}  // anonymous namespace
}  // namespace dztrader::webui
