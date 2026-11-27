#include <dztrader/core/encoding.h>

#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>

#include <cstddef>
#endif

namespace dztrader {

#ifdef _WIN32
/// 共享底层实现: 按指定代码页将多字节字符串转换为 UTF-8。
/// 失败时不抛异常, 返回原串 (宁乱码不崩溃)。
/// flags 传 0: 不验证无效字符, 无效字节被静默跳过或导致函数返回 0。
/// 转换成功时输出为合法 UTF-8; 失败时返回原串 (可能不是合法 UTF-8)。
static std::string mb_to_utf8(const std::string& s, UINT code_page) {
    if (s.empty()) {
        return s;
    }

    const int input_len = static_cast<int>(s.size());
    const char* input = s.c_str();

    // 第一步: 多字节 -> UTF-16
    int wide_len = MultiByteToWideChar(code_page, 0, input, input_len, nullptr, 0);
    if (wide_len <= 0) {
        return s;  // 转换失败, 返回原串避免崩溃
    }

    std::wstring wide(wide_len, L'\0');
    MultiByteToWideChar(code_page, 0, input, input_len, wide.data(), wide_len);

    // 第二步: UTF-16 -> UTF-8
    int utf8_len =
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) {
        return s;
    }

    std::string utf8(utf8_len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len, utf8.data(), utf8_len, nullptr,
                        nullptr);

    return utf8;
}
#else
namespace {

/// 严格校验 UTF-8（拒绝超长编码 / 代理区 / 超出 U+10FFFF）。
///
/// 为什么要先校验而不是直接 iconv：CTP 各平台/版本的 ErrorMsg 编码并不一致（Windows 与
/// Linux 6.7.13 实测都是 GBK，但注释/部分 SDK 版本声称 Linux 返回 UTF-8），直接按 GBK 解
/// 一个本来就是 UTF-8 的串会得到乱码。合法 UTF-8 原样返回是零成本且不会误伤。
bool is_valid_utf8(const std::string& s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        unsigned int cp = 0;
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
        if (i + len > n) {
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
            return false;  // 超长编码
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return false;  // UTF-16 代理区不是合法 code point
        }
        if (cp > 0x10FFFF) {
            return false;
        }
        i += len;
    }
    return true;
}

/// GBK -> UTF-8（iconv）。转换失败或残留未消费字节时返回空串，由调用方净化兜底。
std::string gbk_to_utf8(const std::string& s) {
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        return {};
    }
    // GBK 双字节 -> UTF-8 最多 3 字节，2 倍输入 + 余量足够。
    std::string out(s.size() * 2 + 8, '\0');
    char* in_ptr = const_cast<char*>(s.data());
    std::size_t in_left = s.size();
    char* out_ptr = out.data();
    std::size_t out_left = out.size();
    const std::size_t rc = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
    iconv_close(cd);
    if (rc == static_cast<std::size_t>(-1) || in_left != 0) {
        return {};
    }
    out.resize(out.size() - out_left);
    return out;
}

/// 把任意字节串净化成合法 UTF-8：合法序列保留，非法字节替换为 U+FFFD。
///
/// 这是最后的兜底（iconv 也失败时）：帧 JSON 序列化遇到非法 UTF-8 会**整帧失败**，
/// 宁可显示「???」也不能把整帧丢掉（#512 的现场就是整帧被丢弃，前端永远停在「登录中」）。
std::string sanitize_to_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        // 以当前位置为起点，取最短可行序列（最多 4 字节）做严格校验
        std::size_t len = 0;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (len != 0 && i + len <= n && is_valid_utf8(s.substr(i, len))) {
            out.append(s, i, len);
            i += len;
            continue;
        }
        out.append("\xEF\xBF\xBD");  // U+FFFD
        ++i;
    }
    return out;
}

}  // namespace
#endif

std::string to_utf8_from_system(const std::string& s) {
#ifdef _WIN32
    return mb_to_utf8(s, CP_ACP);
#else
    return s;  // Linux 默认 UTF-8 locale
#endif
}

std::string to_utf8_from_gbk(const std::string& s) {
#ifdef _WIN32
    // 显式用 936 (GBK), 与系统 locale 无关。
    // CTP SDK 在 Windows 上固定返回 GBK, 不能依赖 CP_ACP 恰好是 936。
    return mb_to_utf8(s, 936);
#else
    // Linux CTP SDK 的 ErrorMsg 也是 GBK（6.7.13 实测，见 #512），不能假设 UTF-8：
    // 透传会把非法 UTF-8 塞进窗口帧，nlohmann::json 序列化整帧失败（type=1030 的
    // RTN_TD_STATUS 被丢弃），失败原因到不了前端。
    if (is_valid_utf8(s)) {
        return s;  // 已是合法 UTF-8：兼容声称/实际返回 UTF-8 的 SDK 版本
    }
    std::string converted = gbk_to_utf8(s);
    if (!converted.empty() && is_valid_utf8(converted)) {
        return converted;
    }
    return sanitize_to_utf8(s);  // 最后兜底：输出一定是合法 UTF-8
#endif
}

}  // namespace dztrader
