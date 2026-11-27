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

TEST(ToUtf8FromSystem, EmptyStringReturnsEmpty) { EXPECT_EQ(to_utf8_from_system(""), ""); }

TEST(ToUtf8FromSystem, AsciiUnchanged) {
    EXPECT_EQ(to_utf8_from_system("hello world"), "hello world");
}

TEST(ToUtf8FromSystem, InvalidBytesReturnUsableString) {
    // 0xFF 超出 GBK 首字节上限 (0xFE), 为无效字节。
    // 契约: 失败时返回原串或带替换字符的 UTF-8, 不返回空串。
    const std::string invalid = std::string("\xFF\xFE\x80", 3);
    const std::string result = to_utf8_from_system(invalid);
#ifndef _WIN32
    EXPECT_EQ(result, invalid);  // Linux 直返原串
#else
    EXPECT_FALSE(result.empty());
#endif
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
    const std::string mixed = std::string(
        "a\xD6\xD0"
        "b",
        4);
    const std::string expected = std::string(
        "a\xE4\xB8\xAD"
        "b",
        5);
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

namespace {
/// 测试侧独立的 UTF-8 严格校验（不复用被测实现，避免「用同一 bug 自证」）。
bool is_valid_utf8_for_test(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        unsigned int cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > s.size()) {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) {
            return false;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return false;
        }
        if (cp > 0x10FFFF) {
            return false;
        }
        i += len;
    }
    return true;
}
}  // namespace

TEST(ToUtf8FromGbk, EmptyStringReturnsEmpty) { EXPECT_EQ(to_utf8_from_gbk(""), ""); }

TEST(ToUtf8FromGbk, AsciiUnchanged) {
    // ASCII 与 GBK 兼容, 不变
    EXPECT_EQ(to_utf8_from_gbk("hello world"), "hello world");
}

TEST(ToUtf8FromGbk, InvalidBytesReturnUsableString) {
    // 无效 GBK 字节: 契约要求失败时返回原串或带替换字符的 UTF-8, 不返回空串。
    const std::string invalid = std::string("\xFF\xFE\x80", 3);
    EXPECT_FALSE(to_utf8_from_gbk(invalid).empty());
}

TEST(ToUtf8FromGbk, ConvertsGbkChineseToUtf8) {
    // 固定按 936 解码, 与系统 locale 无关（Linux 上 CTP 同样返回 GBK，见 #512）
    // "中文" 的 GBK 字节
    const std::string gbk = std::string("\xD6\xD0\xCE\xC4", 4);
    // "中文" 的 UTF-8 字节
    const std::string expected = std::string("\xE4\xB8\xAD\xE6\x96\x87", 6);
    EXPECT_EQ(to_utf8_from_gbk(gbk), expected);
}

TEST(ToUtf8FromGbk, ConvertsMixedAsciiGbk) {
    // "a中b" = 'a' + GBK("中") + 'b'
    const std::string mixed = std::string(
        "a\xD6\xD0"
        "b",
        4);
    const std::string expected = std::string(
        "a\xE4\xB8\xAD"
        "b",
        5);
    EXPECT_EQ(to_utf8_from_gbk(mixed), expected);
}

TEST(ToUtf8FromGbk, ConvertsMultipleChineseChars) {
    // "你好CTP" 全部字符
    // GBK: 你=C4 E3, 好=BAC3, C=T P=P
    const std::string gbk = std::string(
        "\xC4\xE3\xBA\xC3"
        "CTP",
        6);
    // UTF-8: 你=E4 BD A0, 好=E5 A5 BD, C=T P=P
    const std::string expected = std::string(
        "\xE4\xBD\xA0\xE5\xA5\xBD"
        "CTP",
        8);
    EXPECT_EQ(to_utf8_from_gbk(gbk), expected);
}

TEST(ToUtf8FromGbk, Utf8InputUnchanged) {
    // 已是合法 UTF-8 的输入原样返回（兼容声称/实际返回 UTF-8 的 SDK 版本）
    const std::string s = "utf8 字符串 测试";
    EXPECT_EQ(to_utf8_from_gbk(s), s);
}

#ifndef _WIN32
TEST(ToUtf8FromGbk, InvalidBytesProduceValidUtf8OnLinux) {
    // #512：Linux 下该函数的输出必须一定能被 JSON 序列化——非法字节净化成 U+FFFD，
    // 不能再把原始非法字节透传给帧编码（否则整帧丢失）。
    for (const std::string& invalid :
         {std::string("\xFF\xFE\x80", 3), std::string("CTP:\xB2\xBB", 6),
          std::string("\xC0\xAF", 2),         // 超长编码
          std::string("\xED\xA0\x80", 3)}) {  // 代理区
        const std::string result = to_utf8_from_gbk(invalid);
        EXPECT_TRUE(is_valid_utf8_for_test(result))
            << "input bytes=" << invalid.size() << " result is not valid UTF-8";
    }
}

TEST(ToUtf8FromGbk, GbkErrorMsgFromCpLoginRejected) {
    // 现场样本（#512）：CTP 6.7.13 Linux 错误密码时 ErrorMsg = "CTP:不合法的登录"（GBK）。
    // GBK: C T P : 不合法的登录
    const std::string gbk = std::string(
        "CTP:"
        "\xB2\xBB\xBA\xCF\xB7\xA8\xB5\xC4\xB5\xC7\xC2\xBC",
        17);
    const std::string expected = std::string(
        "CTP:"
        "\xE4\xB8\x8D\xE5\x90\x88\xE6\xB3\x95\xE7\x9A\x84\xE7\x99\xBB\xE5\xBD\x95",
        23);
    EXPECT_EQ(to_utf8_from_gbk(gbk), expected);
    EXPECT_TRUE(is_valid_utf8_for_test(to_utf8_from_gbk(gbk)));
}
#endif

#ifdef _WIN32
TEST(ToUtf8FromGbk, IndependentOfSystemLocale) {
    // 即使系统 locale 不是 936, 也必须按 GBK 解码
    // (测试无法切换系统 locale, 但函数显式用 936, 不依赖 CP_ACP)
    const std::string gbk = std::string("\xD6\xD0", 2);  // "中"
    const std::string expected = std::string("\xE4\xB8\xAD", 3);
    EXPECT_EQ(to_utf8_from_gbk(gbk), expected);
}
#endif
