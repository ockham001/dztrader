/**
 * @file dztrader.h
 * @brief dztrader 策略接口 — 综合头文件（策略便捷入口）
 *
 * 包含所有策略接口头文件：纯 C 接口 + C++ 模板策略引擎。
 *
 * C 接口使用示例：
 * @code
 *   #include <dztrader.h>
 *   DzContext* ctx = dz_init();
 *   if (ctx == NULL) { return; } // dz_errcode()/dz_errmsg() 报告原因
 *   dz_wait(ctx);
 *   const void* frame = dz_next_event(ctx);
 *   const DzFrameHeader* hdr = (const DzFrameHeader*)frame;
 *   dz_release(ctx);
 * @endcode
 *
 * C++ 引擎使用示例（见 dztrader/strategy_engine.h）：
 * @code
 *   struct MyStrategy : dztrader::StrategyBase {
 *       void on_start(DzContext* ctx) override { /* ... * }
 *       void on_tick(const DzTick& tick) override { /* ... * }
 *   };
 *   int main() {
 *       MyStrategy s;
 *       return dztrader::run_strategy(s);
 *   }
 * @endcode
 */
#ifndef DZTRADER_H_
#define DZTRADER_H_

#include <dztrader/api.h>
#include <dztrader/strategy_base.h>
#include <dztrader/strategy_engine.h>

#endif /* DZTRADER_H_ */
