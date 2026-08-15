#ifndef DZTRADER_CORE_STRING_UTIL_H
#define DZTRADER_CORE_STRING_UTIL_H
#include <cstring>
#include <dztrader/error.h>

namespace dztrader {

/**
 * @brief 安全地将字符串从源缓冲区复制到目标缓冲区
 * @param dest 目标缓冲区指针
 * @param dest_size 目标缓冲区大小（字节数）
 * @param src 源字符串指针
 * @param truncate 如果为 true，当源字符串过长时截断到目标缓冲区大小；如果为
 * false，则不执行复制
 * @return
 * DZ_EC_OK 表示成功复制；
 * DZ_EC_INVALID_PARAM 表示参数无效；
 * DZ_EC_BUFFER_TOO_SMALL 表示目标缓冲区大小不足，根据 truncate参数决定是否截断
 * @note 在调试模式下，未使用的缓冲区空间会被填充为 0xFE 以帮助检测缓冲区溢出
 */
inline DzErrorCode copy_string(char* dest,
                               size_t dest_size,
                               const char* src,
                               bool truncate) noexcept {
#if !defined(NDEBUG)
    static constexpr uint8_t DEBUG_INVALID_MEMORY_FILL_VALUE = 0xFE;
#endif

    if (dest == nullptr || dest_size == 0) {
        return DZ_EC_INVALID_PARAM;
    }
    if (src == nullptr) {
        *dest = '\0';
#if !defined(NDEBUG)
        if (dest_size > 1) {
            memset(dest + 1, DEBUG_INVALID_MEMORY_FILL_VALUE,
                   dest_size - 1);  // 标记为无效
        }
#endif
        return DZ_EC_INVALID_PARAM;
    }

    char* dest_ptr = dest;
    size_t available = dest_size;
    while ((*dest_ptr++ = *src++) != 0 && --available > 0) {
    }

    if (available == 0) {
        if (truncate) {
            dest[dest_size - 1] = '\0';
        } else {
            *dest = '\0';
#if !defined(NDEBUG)
            if (dest_size > 1) {
                memset(dest + 1, DEBUG_INVALID_MEMORY_FILL_VALUE,
                       dest_size - 1);  // 标记为无效
            }
#endif
        }

        return DZ_EC_BUFFER_TOO_SMALL;
    }

#if !defined(NDEBUG)
    if (available > 1) {
        memset(dest + 1 + dest_size - available, DEBUG_INVALID_MEMORY_FILL_VALUE,
               available - 1);  // 标记为无效
    }
#endif
    return DZ_EC_OK;
}

/**
 * @brief 安全地将字符串从源缓冲区复制到固定大小的目标数组
 * @tparam N 目标数组的大小
 * @param dest 目标数组引用
 * @param src 源字符串指针
 * @param truncate 如果为 true，当源字符串过长时截断到目标数组大小；如果为
 * false，则不执行复制
 * @return DZ_EC_OK 表示成功复制；DZ_EC_INVALID_PARAM
 * 表示参数无效；DZ_EC_BUFFER_TOO_SMALL 表示源字符串过长，根据 truncate
 * 参数决定是否截断
 * @note 这是针对数组引用的重载版本，提供了更好的类型安全性
 */
template <size_t N>
inline DzErrorCode copy_string(char (&dest)[N], const char* src, bool truncate) noexcept {
    static_assert(N > 0, "Buffer size must be at least 1");
    return copy_string(dest, N, src, truncate);
}

}  // namespace dztrader

#endif  // DZTRADER_CORE_STRING_UTIL_H