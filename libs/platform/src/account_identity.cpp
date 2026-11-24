#include <dztrader/platform/account_identity.h>

#include <dztrader/core/utf8.h>

namespace dztrader::platform {

std::string_view trim_account_name(std::string_view text) noexcept {
    const auto first = text.find_first_not_of(kAccountNameWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(kAccountNameWhitespace);
    return text.substr(first, last - first + 1);
}

bool account_id_charset_valid(std::string_view account_id) noexcept {
    // 契约 td-config §7：ASCII 可见字符 0x21–0x7E（禁空格 / 控制字符 / 非 ASCII），禁 `:`。
    for (const char c : account_id) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x21 || byte > 0x7E || c == ':') {
            return false;
        }
    }
    return true;
}

namespace {

/// 控制字符 = ASCII < 0x20（含 `\t\r\n`）、DEL 0x7F 与 C1 控制段 U+0080–U+009F。
/// C1 段在 UTF-8 里只能是 `0xC2` + `0x80`–`0x9F` 两字节序列。
bool has_control_chars(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x20 || byte == 0x7F) {
            return true;
        }
        if (byte == 0xC2 && i + 1 < text.size()) {
            const auto next = static_cast<unsigned char>(text[i + 1]);
            if (next >= 0x80 && next <= 0x9F) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

AccountNameKey validate_account_name(std::string_view account_name) {
    const std::string_view trimmed = trim_account_name(account_name);
    if (trimmed.empty()) {
        return {};  // 未设置：合法，key 为空串（调用方按 account_id 回落）
    }
    if (trimmed.size() > kAccountNameMaxBytes) {
        return {.error = AccountNameError::TooLong, .key = {}};
    }
    if (has_control_chars(trimmed)) {
        return {.error = AccountNameError::ControlChar, .key = {}};
    }
    auto normalized = dztrader::utf8_nfc(trimmed);
    if (!normalized.has_value()) {
        return {.error = AccountNameError::InvalidUtf8, .key = {}};
    }
    return {.error = AccountNameError::None, .key = std::move(*normalized)};
}

}  // namespace dztrader::platform
