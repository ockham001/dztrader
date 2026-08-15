#include <gtest/gtest.h>
#include "jwt.h"

using dztrader::webui::jwt_sign;
using dztrader::webui::jwt_verify;

TEST(JwtTest, SignAndVerifyRoundTrip) {
    const std::string token = jwt_sign("admin", 3600, "my_secret");
    EXPECT_FALSE(token.empty());
    std::string sub;
    ASSERT_TRUE(jwt_verify(token, "my_secret", sub));
    EXPECT_EQ(sub, "admin");
}

TEST(JwtTest, VerifyFailsWithWrongSecret) {
    const std::string token = jwt_sign("admin", 3600, "correct_secret");
    std::string sub;
    EXPECT_FALSE(jwt_verify(token, "wrong_secret", sub));
}

TEST(JwtTest, VerifyFailsWithExpiredToken) {
    const std::string token = jwt_sign("admin", 0, "secret");  // ttl=0 立即过期
    std::string sub;
    EXPECT_FALSE(jwt_verify(token, "secret", sub));
}

TEST(JwtTest, VerifyFailsWithMalformedToken) {
    std::string sub;
    EXPECT_FALSE(jwt_verify("not.a.jwt", "secret", sub));
    EXPECT_FALSE(jwt_verify("", "secret", sub));
}
