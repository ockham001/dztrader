/**
 * @file dztrader.h
 * @brief dztrader 策略接口 — 综合头文件（策略便捷入口）
 *
 * 包含所有策略接口头文件（纯 C 接口）。
 * C++ 封装（StrategyBase CRTP 模板等）后期按需添加。
 *
 * 使用示例（C）：
 * @code
 *   #include <dztrader.h>
 *   dz_init();
 *   dz_wait();
 *   const void* frame = dz_next_event();
 *   const DzFrameHeader* hdr = (const DzFrameHeader*)frame;
 * @endcode
 */
#ifndef DZTRADER_H_
#define DZTRADER_H_

#include <dztrader/api.h>

#endif /* DZTRADER_H_ */
