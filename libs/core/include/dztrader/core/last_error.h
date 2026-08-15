/**
 * @file last_error.h
 * @brief 线程局部错误存储（C 接口 dz_errcode/dz_errstr/dz_errmsg 的 C++ 封装）
 *
 * 所有路径（性能/非性能）均可使用。
 * dz_errmsg() 格式规则见 coding-style.md。
 */
#ifndef DZTRADER_CORE_LAST_ERROR_H_
#define DZTRADER_CORE_LAST_ERROR_H_

#include <array>
#include <cstring>
#include <dztrader/error.h>
#include <format>
#include <string_view>

namespace dztrader {

class LastError {
public:
    /// 仅提供静态方法，禁止实例化
    LastError() = delete;

    /** @brief 设置错误码 + 上下文（格式化失败时降级为原始 fmt，超长截断） */
    template <typename... Args> static void set(DzErrorCode code, std::string_view fmt, const Args&... args) noexcept
    {
        code_ = code;
        if constexpr (sizeof...(Args) == 0) {
            store_msg(fmt);
        }
        else {
            try_format(fmt, args...);
        }
    }

    /** @brief 获取最近错误码，0 表示无错误 */
    [[nodiscard]] static DzErrorCode code() noexcept { return code_; }

    /** @brief 错误码→描述字符串，始终非 NULL */
    [[nodiscard]] static const char* str(DzErrorCode code) noexcept;

    /** @brief 最近错误的上下文消息，无错误返回 "" */
    [[nodiscard]] static const char* msg() noexcept { return msg_.data(); }

    /** @brief 清除错误状态 */
    static void clear() noexcept
    {
        code_ = DZ_EC_OK;
        msg_[0] = '\0';
    }

private:
    /** @brief 写入 msg_ 缓冲区，超长截断 */
    static void store_msg(std::string_view src) noexcept
    {
        const auto copy_len = src.size() < msg_.size() - 1 ? src.size() : msg_.size() - 1;
        if (copy_len > 0) {
            std::memcpy(msg_.data(), src.data(), copy_len);
        }
        msg_[copy_len] = '\0';
    }

    /** @brief 格式化并写入缓冲区，失败时降级为原始 fmt */
    template <typename... Args> static void try_format(std::string_view fmt, const Args&... args) noexcept
    {
        try {
            store_msg(std::vformat(fmt, std::make_format_args(args...)));
        }
        catch (...) {
            store_msg(fmt);
        }
    }

    static thread_local DzErrorCode code_;            // NOLINT
    static thread_local std::array<char, 2048> msg_;  // NOLINT
};

}  // namespace dztrader

#endif /* DZTRADER_CORE_LAST_ERROR_H_ */