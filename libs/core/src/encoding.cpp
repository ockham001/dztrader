#include <dztrader/core/encoding.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len,
                                       nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) {
        return s;
    }

    std::string utf8(utf8_len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len,
                        utf8.data(), utf8_len, nullptr, nullptr);

    return utf8;
}
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
    return s;  // Linux CTP SDK 已返回 UTF-8
#endif
}

}  // namespace dztrader
