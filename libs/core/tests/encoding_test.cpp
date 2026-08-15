#include <dztrader/core/encoding.h>

#include <string>

#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using dztrader::to_utf8_from_gbk;
using dztrader::to_utf8_from_system;

// ── to_utf8_from_system ──

TEST(ToUtf8FromSystem, EmptyStringReturnsEmpty) {
    EXPECT_EQ(to_utf8_from_system(""), "");
}

TEST(ToUtf8FromSystem, AsciiUnchanged) {
    EXPECT_EQ(to_utf8_from_system("hello world"), "hello world");
}

TEST(ToUtf8FromSystem, DoesNotCrashOnInvalidBytes) {
    // 0xFF 超出 GBK 首字节上限 (0xFE), 为无效字节
    const std::string invalid = std::string("\xFF\xFE\x80", 3);
    EXPECT_NO_THROW((void)to_utf8_from_system(invalid));
}

#ifdef _WIN32
TEST(ToUtf8FromSystem, ConvertsGbkOnChineseWindows) {
    // 仅在 CP_ACP==936 (GBK) 的中文 Windows 上验证转换正确性
    if (GetACP() != 936) {
        GTEST_SKIP() << "skipped: CP_ACP is not 936 (GBK)";
    }
    // "中文" 的 GBK 字节
    const std::string gbk = std::string("\xD6\xD0\xCE\xC4", 4);
    // "中文" 的 UTF-8 字节
    const std::string expected = std::string("\xE4\xB8\xAD\xE6\x96\x87", 6);
    EXPECT_EQ(to_utf8_from_system(gbk), expected);
}

TEST(ToUtf8FromSystem, ConvertsMixedAsciiGbkOnChineseWindows) {
    if (GetACP() != 936) {
        GTEST_SKIP() << "skipped: CP_ACP is not 936 (GBK)";
    }
    // "a中b" = 'a' + GBK("中") + 'b'
    const std::string mixed = std::string("a\xD6\xD0""b", 4);
    const std::string expected = std::string("a\xE4\xB8\xAD""b", 5);
    EXPECT_EQ(to_utf8_from_system(mixed), expected);
}
#else
TEST(ToUtf8FromSystem, ReturnsOriginalOnLinux) {
    // Linux 默认 locale 即 UTF-8, 函数直返
    const std::string s = "utf8 字符串 测试";
    EXPECT_EQ(to_utf8_from_system(s), s);
}
#endif

// ── to_utf8_from_gbk ──

TEST(ToUtf8FromGbk, EmptyStringReturnsEmpty) {
    EXPECT_EQ(to_utf8_from_gbk(""), "");
}

TEST(ToUtf8FromGbk, AsciiUnchanged) {
    // ASCII 与 GBK 兼容, 不变
    EXPECT_EQ(to_utf8_from_gbk("hello world"), "hello world");
}

TEST(ToUtf8FromGbk, DoesNotCrashOnInvalidBytes) {
    // 无效 GBK 字节, 必须不崩溃 (可乱码, 可返回原串)
    const std::string invalid = std::string("\xFF\xFE\x80", 3);
    EXPECT_NO_THROW((void)to_utf8_from_gbk(invalid));
}

#ifdef _WIN32
TEST(ToUtf8FromGbk, ConvertsGbkChineseToUtf8) {
    // 固定按 936 解码, 与系统 locale 无关
    // "中文" 的 GBK 字节
    const std::string gbk = std::string("\xD6\xD0\xCE\xC4", 4);
    // "中文" 的 UTF-8 字节
    const std::string expected = std::string("\xE4\xB8\xAD\xE6\x96\x87", 6);
    EXPECT_EQ(to_utf8_from_gbk(gbk), expected);
}

TEST(ToUtf8FromGbk, ConvertsMixedAsciiGbk) {
    // "a中b" = 'a' + GBK("中") + 'b'
    const std::string mixed = std::string("a\xD6\xD0""b", 4);
    const std::string expected = std::string("a\xE4\xB8\xAD""b", 5);
    EXPECT_EQ(to_utf8_from_gbk(mixed), expected);
}

TEST(ToUtf8FromGbk, ConvertsMultipleChineseChars) {
    // "你好CTP" 全部字符
    // GBK: 你=C4 E3, 好=BAC3, C=T P=P
    const std::string gbk = std::string("\xC4\xE3\xBA\xC3""CTP", 6);
    // UTF-8: 你=E4 BD A0, 好=E5 A5 BD, C=T P=P
    const std::string expected = std::string("\xE4\xBD\xA0\xE5\xA5\xBD""CTP", 8);
    EXPECT_EQ(to_utf8_from_gbk(gbk), expected);
}

TEST(ToUtf8FromGbk, IndependentOfSystemLocale) {
    // 即使系统 locale 不是 936, 也必须按 GBK 解码
    // (测试无法切换系统 locale, 但函数显式用 936, 不依赖 CP_ACP)
    const std::string gbk = std::string("\xD6\xD0", 2);  // "中"
    const std::string expected = std::string("\xE4\xB8\xAD", 3);
    EXPECT_EQ(to_utf8_from_gbk(gbk), expected);
}
#else
TEST(ToUtf8FromGbk, ReturnsOriginalOnLinux) {
    // Linux CTP SDK 已返回 UTF-8, 函数直返
    const std::string s = "utf8 字符串 测试";
    EXPECT_EQ(to_utf8_from_gbk(s), s);
}
#endif
