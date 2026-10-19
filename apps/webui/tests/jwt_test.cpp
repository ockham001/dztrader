#include <gtest/gtest.h>
#include "jwt.h"

#include <string>
#include <vector>

using dztrader::webui::jwt_sign;
using dztrader::webui::jwt_verify;

namespace {

/// 按 '.' 切分 JWT（header.payload.signature）
std::vector<std::string> split_token(const std::string& token) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t dot = token.find('.', start);
        if (dot == std::string::npos) {
            parts.push_back(token.substr(start));
            break;
        }
        parts.push_back(token.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

/// base64url({"alg":"none"})，无 padding
constexpr const char* kAlgNoneHeader = "eyJhbGciOiJub25lIn0";

/// 翻转 base64url 段的一个字符（仍属同一字母表，签名必然不再匹配）
void flip_char(std::string& part, size_t pos) { part[pos] = (part[pos] == 'A') ? 'B' : 'A'; }

}  // namespace

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

TEST(JwtTest, VerifyFailsWhenPayloadTampered) {
    const std::string token = jwt_sign("admin", 3600, "secret");
    auto parts = split_token(token);
    ASSERT_EQ(parts.size(), 3u);

    std::string tampered_payload = parts[1];
    ASSERT_FALSE(tampered_payload.empty());
    flip_char(tampered_payload, tampered_payload.size() - 1);
    const std::string tampered = parts[0] + "." + tampered_payload + "." + parts[2];

    std::string sub = "sentinel";
    EXPECT_FALSE(jwt_verify(tampered, "secret", sub));
    EXPECT_EQ(sub, "sentinel");  // 失败路径不得污染输出
}

TEST(JwtTest, VerifyFailsWhenSignatureTampered) {
    const std::string token = jwt_sign("admin", 3600, "secret");
    auto parts = split_token(token);
    ASSERT_EQ(parts.size(), 3u);

    std::string tampered_sig = parts[2];
    ASSERT_FALSE(tampered_sig.empty());
    flip_char(tampered_sig, 0);
    const std::string tampered = parts[0] + "." + parts[1] + "." + tampered_sig;

    std::string sub = "sentinel";
    EXPECT_FALSE(jwt_verify(tampered, "secret", sub));
    EXPECT_EQ(sub, "sentinel");
}

TEST(JwtTest, VerifyRejectsAlgNoneHeader) {
    const std::string token = jwt_sign("admin", 3600, "secret");
    auto parts = split_token(token);
    ASSERT_EQ(parts.size(), 3u);

    // alg=none + 空签名：实现不得信任 header 的 alg（始终按 HS256 校验）
    const std::string none_token = std::string(kAlgNoneHeader) + "." + parts[1] + ".";
    std::string sub = "sentinel";
    EXPECT_FALSE(jwt_verify(none_token, "secret", sub));
    EXPECT_EQ(sub, "sentinel");
}
