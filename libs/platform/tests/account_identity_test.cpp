/**
 * @file account_identity_test.cpp
 * @brief 交易账户身份字段的平台层共享校验（ADR 0056 决策 4/6、契约 td-config §2.2 / §7）。
 *
 * 断言两侧（dzweb 平台入口 / td 网关落盘前自检）共用的**字节规则**与唯一性比较键：
 * `account_id` 字符集、`account_name` 的 trim / 64 字节 / 控制字符 / NFC / 大小写敏感。
 * 长度上限（31 / 12 字节）与「非空」属各层自有的分层约束，不在本文件断言。
 */

#include <dztrader/platform/account_identity.h>

#include <gtest/gtest.h>

#include <string>

namespace dztrader::platform {
namespace {

// ============================================================================
// account_id 字符集（契约 td-config §7 / ADR 0057）
// ============================================================================

TEST(AccountIdentityTest, AccountIdAcceptsAsciiVisibleCharset) {
    for (const char* id : {"00001", "A-z_09", "!~", "account_001"}) {
        EXPECT_TRUE(account_id_charset_valid(id)) << "id=" << id;
    }
}

TEST(AccountIdentityTest, AccountIdAcceptsBoundaryBytes) {
    EXPECT_TRUE(account_id_charset_valid("!")) << "0x21 is the lower bound (inclusive)";
    EXPECT_TRUE(account_id_charset_valid("~")) << "0x7E is the upper bound (inclusive)";
}

TEST(AccountIdentityTest, AccountIdRejectsSpace) {
    EXPECT_FALSE(account_id_charset_valid("000 01"));
    EXPECT_FALSE(account_id_charset_valid(" 00001"));
    EXPECT_FALSE(account_id_charset_valid("00001 "));
    EXPECT_FALSE(account_id_charset_valid(" "));
}

TEST(AccountIdentityTest, AccountIdRejectsColon) {
    // ADR 0056 取消了 `网关名:账户ID` 复合键：`:` 不再出现在账户 ID 里，避免旧形态回流。
    EXPECT_FALSE(account_id_charset_valid("00001:x"));
    EXPECT_FALSE(account_id_charset_valid(":"));
}

TEST(AccountIdentityTest, AccountIdRejectsPathSeparatorAndDot) {
    // ADR 0057：account_id 直接作 td 网关的 CTP 流文件目录名——`/`、`\` 可穿越目录；
    // `.` / `..` 与 Windows 尾点归一可造成目录碰撞，三者一律不合法。
    for (const char* id : {"000/01", "000\\01", "000.01", ".", "..", "00001.", ".00001"}) {
        EXPECT_FALSE(account_id_charset_valid(id)) << "id=" << id;
    }
}

TEST(AccountIdentityTest, AccountIdRejectsControlAndNonAscii) {
    EXPECT_FALSE(account_id_charset_valid("000\t01"));
    EXPECT_FALSE(account_id_charset_valid(std::string("000\0 01", 7)));
    EXPECT_FALSE(account_id_charset_valid("\x7F"));
    EXPECT_FALSE(account_id_charset_valid("账户"));
    EXPECT_FALSE(account_id_charset_valid(std::string("\xC2\x80", 2))) << "C1 control U+0080";
}

TEST(AccountIdentityTest, AccountIdEmptyIsCharsetValidButCallerChecksNonEmpty) {
    // 契约 §7 的「非空」是独立判定（两层各自校验）；字符集函数对空串恒为 true。
    EXPECT_TRUE(account_id_charset_valid(""));
}

// ============================================================================
// trim（契约 td-config §2.2「trim 后」）
// ============================================================================

TEST(AccountIdentityTest, TrimRemovesAsciiWhitespaceOnly) {
    EXPECT_EQ(trim_account_name("  主账户  "), "主账户");
    EXPECT_EQ(trim_account_name("\t\r\n name \f\v"), "name");
    EXPECT_EQ(trim_account_name("name"), "name");
    EXPECT_TRUE(trim_account_name("").empty());
    EXPECT_TRUE(trim_account_name(" \t\r\n\f\v ").empty());
    // 不是 Unicode 空白：全角空格不在 trim 集合里（契约口径只有这六个 ASCII 字符）。
    EXPECT_EQ(trim_account_name("\xE3\x80\x80name"), "\xE3\x80\x80name");
}

// ============================================================================
// account_name 硬约束与唯一性比较键（契约 td-config §2.2 / §7、ADR 0056 决策 4）
// ============================================================================

TEST(AccountIdentityTest, EmptyAndWhitespaceOnlyNameIsUnset) {
    for (const char* raw : {"", " ", "\t\r\n", "   \t  "}) {
        const auto result = validate_account_name(raw);
        EXPECT_TRUE(result.valid()) << "raw=" << raw;
        EXPECT_TRUE(result.unset()) << "raw=" << raw;
        EXPECT_TRUE(result.key.empty()) << "raw=" << raw;
    }
}

TEST(AccountIdentityTest, NameIsTrimmedIntoTheComparisonKey) {
    const auto result = validate_account_name("  我的主账户  ");
    ASSERT_TRUE(result.valid());
    EXPECT_FALSE(result.unset());
    EXPECT_EQ(result.key, "我的主账户");
}

TEST(AccountIdentityTest, NameMaxBytesIsCountedAfterTrim) {
    // 上限 64 字节；两侧空白不计入。
    const std::string max_ok = "  " + std::string(64, 'n') + "  ";
    const auto ok = validate_account_name(max_ok);
    ASSERT_TRUE(ok.valid()) << "64 bytes after trim is the inclusive upper bound";
    EXPECT_EQ(ok.key.size(), 64U);

    const auto too_long = validate_account_name(std::string(65, 'n'));
    EXPECT_EQ(too_long.error, AccountNameError::TooLong);
    EXPECT_TRUE(too_long.key.empty());
}

TEST(AccountIdentityTest, NameMaxBytesCountsUtf8BytesNotCharacters) {
    // 汉字 3 字节：21 字 = 63 字节（可），22 字 = 66 字节（超限）。
    std::string chinese_21;
    for (int i = 0; i < 21; ++i) {
        chinese_21 += "名";
    }
    EXPECT_TRUE(validate_account_name(chinese_21).valid());
    EXPECT_EQ(validate_account_name(chinese_21 + "名").error, AccountNameError::TooLong);
}

TEST(AccountIdentityTest, NameRejectsControlChars) {
    for (const char* raw : {"a\tb", "a\nb", "a\rb",
                            "a\x7F"
                            "b"}) {
        EXPECT_EQ(validate_account_name(raw).error, AccountNameError::ControlChar) << "raw=" << raw;
    }
}

TEST(AccountIdentityTest, NameRejectsC1ControlChars) {
    // U+0080–U+009F（UTF-8 里是 0xC2 + 0x80–0x9F 两字节）
    EXPECT_EQ(validate_account_name(std::string("a\xC2\x80"
                                                "b",
                                                4))
                  .error,
              AccountNameError::ControlChar);
}

TEST(AccountIdentityTest, NameRejectsInvalidUtf8) {
    EXPECT_EQ(validate_account_name("\xFF").error, AccountNameError::InvalidUtf8);
    // 内嵌 NUL 先被控制字符规则拦下（< 0x20），不会走到 UTF-8 校验——同样非法。
    EXPECT_EQ(validate_account_name(std::string("a\0b", 3)).error, AccountNameError::ControlChar);
}

TEST(AccountIdentityTest, NameKeyIsNfcNormalized) {
    // 预组合 U+00E9 与 分解形式 "e" + U+0301 渲染相同、字节不同；NFC 后同一比较键。
    const auto precomposed = validate_account_name("\xC3\xA9");
    const auto decomposed = validate_account_name("e\xCC\x81");
    ASSERT_TRUE(precomposed.valid());
    ASSERT_TRUE(decomposed.valid());
    EXPECT_EQ(precomposed.key, decomposed.key);
    EXPECT_EQ(precomposed.key, "\xC3\xA9");
}

TEST(AccountIdentityTest, NameComparisonIsCaseSensitive) {
    // 契约 §2.2：NFC 后字节精确比较，不折叠大小写。
    const auto upper = validate_account_name("Main");
    const auto lower = validate_account_name("main");
    ASSERT_TRUE(upper.valid());
    ASSERT_TRUE(lower.valid());
    EXPECT_NE(upper.key, lower.key);
}

TEST(AccountIdentityTest, ChineseNameIsNfcIdentity) {
    const auto result = validate_account_name("主账户");
    ASSERT_TRUE(result.valid());
    EXPECT_EQ(result.key, "主账户");
}

// ============================================================================
// 展平判定用的有效身份键（契约 td-config §2.2 / §5 / §7、ADR 0056 决策 4）
// ============================================================================

TEST(AccountIdentityTest, EffectiveKeyUsesNormalizedNameWhenSet) {
    EXPECT_EQ(effective_account_name_key("00001", "  我的主账户 "), "我的主账户");
    EXPECT_EQ(effective_account_name_key("00001", "e\xCC\x81"), "\xC3\xA9") << "NFC applied";
}

TEST(AccountIdentityTest, EffectiveKeyFallsBackToAccountIdWhenUnset) {
    // 留空 = 未设置：按 account_id 参与判定（"未设置"与"设置成和 id 一样"是两件事，但
    // 唯一性比较键相同——两者都会让界面显示同一个字符串）。
    EXPECT_EQ(effective_account_name_key("00001", ""), "00001");
    EXPECT_EQ(effective_account_name_key("00001", "   "), "00001");
}

TEST(AccountIdentityTest, EffectiveKeyFallsBackToAccountIdForInvalidName) {
    // 非法名只可能来自手改配置文件；与展示回落同口径按 id 参与判定（不产出空键）。
    EXPECT_EQ(effective_account_name_key("00001", "bad\tname"), "00001");
    EXPECT_EQ(effective_account_name_key("00001", std::string(65, 'n')), "00001");
}

}  // namespace
}  // namespace dztrader::platform
