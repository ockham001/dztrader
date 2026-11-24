#include <dztrader/core/utf8.h>

#include <gtest/gtest.h>

#include <string>

namespace dztrader {
namespace {

TEST(Utf8NfcTest, EmptyStaysEmpty) {
    const auto out = utf8_nfc("");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->empty());
}

TEST(Utf8NfcTest, AsciiUnchanged) {
    const auto out = utf8_nfc("account_001");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "account_001");
}

/// 汉字在 NFC 下是恒等映射（无分解/合成形式），中文账户名不受规范化影响。
TEST(Utf8NfcTest, ChineseUnchanged) {
    const auto out = utf8_nfc("主账户");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "主账户");
}

/// 核心语义：预组合 "é"(U+00E9) 与 分解形式 "e"+U+0301 规范化后字节相同，
/// 唯一性判定才能把「看起来一样」的名字判为冲突（ADR 0056 决策 4）。
TEST(Utf8NfcTest, ComposesDecomposedLatinSequence) {
    const std::string decomposed = "e\xCC\x81";  // e + U+0301 COMBINING ACUTE ACCENT
    const std::string composed = "\xC3\xA9";     // U+00E9 LATIN SMALL LETTER E WITH ACUTE
    const auto out = utf8_nfc(decomposed);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, composed);
}

TEST(Utf8NfcTest, IsIdempotent) {
    const auto once = utf8_nfc("e\xCC\x81");
    ASSERT_TRUE(once.has_value());
    const auto twice = utf8_nfc(*once);
    ASSERT_TRUE(twice.has_value());
    EXPECT_EQ(*twice, *once);
}

TEST(Utf8NfcTest, InvalidUtf8ReturnsNullopt) {
    EXPECT_EQ(utf8_nfc("\xFF"), std::nullopt);                  // 非法起始字节
    EXPECT_EQ(utf8_nfc("\xC3"), std::nullopt);                  // 截断的多字节序列
    EXPECT_EQ(utf8_nfc("e\xCC"), std::nullopt);                 // 组合符缺续字节
    EXPECT_EQ(utf8_nfc(std::string("a\0b", 3)), std::nullopt);  // 内嵌 NUL
}

}  // namespace
}  // namespace dztrader
