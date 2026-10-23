/**
 * @file bar.h
 * @brief tick→bar 合成组件 — 综合头文件（ADR 0031）
 *
 * 一个实例 = 单合约 + 单周期（日线无周期），纯 tick 驱动、不做 IO/线程/定时器：
 * - dztrader/bar/types.h：公开数据模型（Bar / TradingTime / 交易小节与竞价表）
 * - dztrader/bar/intraday_bar_builder.h：日内容周期合成器
 * - dztrader/bar/daily_bar_builder.h：日线合成器
 *
 * @code
 *   #include <dztrader/bar.h>
 *
 *   // 交易小节：09:00–15:00（交易日内秒坐标）
 *   dztrader::bar::TradingSessionTable table{};
 *   table.session_count = 1;
 *   table.sessions[0] = {54000, 75600};
 *
 *   dztrader::bar::NullBarSink sink;
 *   dztrader::bar::IntradayBarBuilder<> builder(table, 60, sink);
 *   builder.on_tick(tick);
 *   if (builder.has_current_bar()) {
 *       const dztrader::bar::Bar& bar = builder.current_bar();
 *   }
 *
 *   dztrader::bar::DailyBarBuilder<> daily_builder(sink);
 *   daily_builder.on_tick(tick);
 * @endcode
 */
#ifndef DZTRADER_BAR_H_
#define DZTRADER_BAR_H_

#include <dztrader/bar/daily_bar_builder.h>
#include <dztrader/bar/intraday_bar_builder.h>
#include <dztrader/bar/types.h>

#endif  // DZTRADER_BAR_H_
