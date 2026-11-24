#include <dztrader/core/utf8.h>

#include <cstdlib>
#include <limits>

#include <utf8proc.h>

namespace dztrader {

std::optional<std::string> utf8_nfc(std::string_view text) {
    if (text.empty()) {
        return std::string{};
    }
    if (text.find('\0') != std::string_view::npos) {
        return std::nullopt;  // utf8proc 按 NUL 结尾处理输入，内嵌 NUL 无法安全规范化
    }
    if (text.size() > static_cast<size_t>(std::numeric_limits<utf8proc_ssize_t>::max())) {
        return std::nullopt;  // utf8proc 的长度参数为有符号；实际名字远小于此上限
    }
    const std::string input(text);  // 确保 NUL 结尾（string_view 的 data() 不保证）
    utf8proc_uint8_t* normalized =
        utf8proc_NFC(reinterpret_cast<const utf8proc_uint8_t*>(input.c_str()));
    if (normalized == nullptr) {
        return std::nullopt;  // 非法 UTF-8
    }
    std::string result(reinterpret_cast<const char*>(normalized));
    std::free(normalized);
    return result;
}

}  // namespace dztrader
