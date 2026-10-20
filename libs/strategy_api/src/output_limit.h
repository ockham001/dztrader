/**
 * @file output_limit.h
 * @brief dz_output_ui payload 上限计算与写路径（内部实现，不暴露给 C API 头文件）
 *
 * 变长帧总大小（DzFrameHeader + DzExtInstFrameHeader + padded payload）
 * 不得超过单页大小（shm Writer::open_frame 硬约束）。
 */
#ifndef DZTRADER_STRATEGY_API_OUTPUT_LIMIT_H_
#define DZTRADER_STRATEGY_API_OUTPUT_LIMIT_H_

#include <algorithm>
#include <cstdint>

#include <dztrader/struct.h>

struct DzContext;  ///< api.h 不透明句柄（实现见 strategy_context.h）

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

/**
 * @brief dz_output_ui 写路径（页容量显式传入）
 *
 * 生产入口 dz_output_ui 传入 event_writer.page_size()。独立成函数是因为
 * 真实通道页容量下限为 1MB（shm ChannelMeta::validate_config），
 * cap == 0 的 DZ_EC_BUFFER_TOO_SMALL 分支经公开入口不可达，只能在注入
 * 页容量时验证。
 *
 * @param ctx 策略上下文（仅用 strategy_id 与 event_writer）
 * @param data NUL 结尾字符串；超页上限时截断到 output_ui_max_payload
 * @param page_size 事件通道单页字节数
 * @return true 已写出并通知订阅者；false 已设置 LastError
 */
bool output_ui_write(DzContext* ctx, const char* data, uint64_t page_size);

}  // namespace dztrader

#endif /* DZTRADER_STRATEGY_API_OUTPUT_LIMIT_H_ */
