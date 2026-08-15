/**
 * @file exception.h
 * @brief 项目异常类，携带 DzErrorCode + 上下文
 *
 * 仅用于非性能关键路径（初始化、打开/关闭、配置等）。
 * .what() 格式与 dz_errmsg() 完全相同，仅存储位置不同。
 * 支持格式化参数（与 LastError::set 对称），格式化失败时降级为原始 fmt。
 * 在 C 接口边界 catch 后转为 LastError::set()。
 */
#ifndef DZTRADER_CORE_EXCEPTION_H_
#define DZTRADER_CORE_EXCEPTION_H_

#include <dztrader/error.h>

#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dztrader {

class Exception : public std::runtime_error {
public:
    /// 构造异常：code + 上下文（格式化失败时降级为原始 fmt）
    template <typename... Args>
    explicit Exception(DzErrorCode code, std::string_view fmt, Args&&... args)
        : std::runtime_error(format_msg(fmt, std::forward<Args>(args)...)),
          code_(code) {}

    [[nodiscard]] DzErrorCode code() const noexcept { return code_; }

private:
    DzErrorCode code_;

    static std::string format_msg(std::string_view fmt, auto&&... args) {
        if constexpr (sizeof...(args) == 0) {
            return std::string(fmt);
        } else {
            try {
                return std::vformat(fmt, std::make_format_args(args...));
            } catch (...) {
                return std::string(fmt);
            }
        }
    }
};

}  // namespace dztrader

#endif  // DZTRADER_CORE_EXCEPTION_H_