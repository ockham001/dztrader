/**
 * @file output_limit.h
 * @brief dz_output_ui payload 上限计算（内部实现，不暴露给 C API 头文件）
 *
 * 变长帧总大小（DzFrameHeader + DzExtInstFrameHeader + padded payload）
 * 不得超过单页大小（shm Writer::open_frame 硬约束）。
 */
#ifndef DZTRADER_STRATEGY_API_OUTPUT_LIMIT_H_
#define DZTRADER_STRATEGY_API_OUTPUT_LIMIT_H_

#include <algorithm>
#include <cstdint>

#include <dztrader/struct.h>

namespace dztrader {

/// dz_output_ui payload 硬上限（与既有 1MB 截断语义一致）
inline constexpr uint64_t kOutputUiHardCap = 1024 * 1024;

/// 帧头开销: DzFrameHeader(8) + DzExtInstFrameHeader(72)
inline constexpr uint64_t kOutputUiFrameOverhead =
    sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader);

/**
 * @brief 给定页大小时 dz_output_ui 的最大可写 payload 字节数
 *
 * 返回值已向下取整到 8 的倍数, 保证 padded(payload) + 帧头开销 <= 页大小。
 * page_size 不足以容纳空帧头时返回 0。
 */
inline uint64_t output_ui_max_payload(uint64_t page_size) noexcept {
    if (page_size <= kOutputUiFrameOverhead) {
        return 0;
    }
    return std::min(kOutputUiHardCap, (page_size - kOutputUiFrameOverhead) & ~uint64_t{7});
}

}  // namespace dztrader

#endif /* DZTRADER_STRATEGY_API_OUTPUT_LIMIT_H_ */
