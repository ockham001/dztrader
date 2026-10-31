#include <gtest/gtest.h>

#include <string>

#include "ws_auth.h"

namespace dztrader::webui {
namespace {

// ---- /ws 连接鉴权（契约 webui-ws §1）：query token 优先，其次 Authorization: Bearer ----

TEST(WsAuthTest, AuthenticateAcceptsQueryTokenAndHeaderFallback) {
    const std::string secret = "test_secret";
    const std::string token = jwt_sign("alice", 3600, secret);

    auto via_query = ws_authenticate(token, "", secret);
    ASSERT_TRUE(via_query.has_value());
    EXPECT_EQ(*via_query, "alice");

    auto via_header = ws_authenticate("", "Bearer " + token, secret);
    ASSERT_TRUE(via_header.has_value());
    EXPECT_EQ(*via_header, "alice");

    // query 优先于 header（header 无效也不影响）
    auto both = ws_authenticate(token, "Bearer garbage", secret);
    ASSERT_TRUE(both.has_value());
    EXPECT_EQ(*both, "alice");
}

TEST(WsAuthTest, AuthenticateRejectsInvalidTokens) {
    const std::string secret = "test_secret";
    const std::string token = jwt_sign("alice", 3600, secret);

    EXPECT_FALSE(ws_authenticate("", "", secret).has_value());
    EXPECT_FALSE(ws_authenticate("garbage", "", secret).has_value());
    EXPECT_FALSE(ws_authenticate("", "garbage", secret).has_value());
    // 非 Bearer scheme 不提取
    EXPECT_FALSE(ws_authenticate("", "Basic " + token, secret).has_value());
    // 签名密钥不符
    EXPECT_FALSE(ws_authenticate("", "Bearer " + token, "wrong_secret").has_value());
}

// 契约 webui-ws §1：token 的 ver claim 需被提取出来供 DB 侧会话判定使用
TEST(WsAuthTest, AuthenticateSurfacesTokenVersion) {
    const std::string secret = "test_secret";
    const std::string token = jwt_sign("alice", 3600, secret, 5);

    int64_t version = -1;
    auto user = ws_authenticate(token, "", secret, &version);
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(*user, "alice");
    EXPECT_EQ(version, 5);
}

TEST(WsAuthTest, FailedAuthLeavesVersionUntouched) {
    const std::string secret = "test_secret";
    const std::string token = jwt_sign("alice", 3600, "other_secret", 5);

    int64_t version = 42;
    EXPECT_FALSE(ws_authenticate(token, "", secret, &version).has_value());
    EXPECT_EQ(version, 42);  // 失败路径不得污染输出
}

}  // namespace
}  // namespace dztrader::webui
