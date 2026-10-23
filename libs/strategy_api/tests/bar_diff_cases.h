/**
 * @file bar_diff_cases.h
 * @brief issue #92 差分验收的用例执行器（fast 抽样档与 slow 全量档共用）
 *
 * 分层方式对齐仓库既有先例（`date_test.cpp` fast 抽样 / `date_exhaustive_test.cpp` slow
 * 全量）：用例**逻辑**只在本头写一份，两个 target 分别用不同规模的参数调用同一组执行器。
 *
 * 每个执行器都做三件事：
 * 1. 把 tick 流按 `DiffDrop` 标签分发给 oracle（只喂 `None`）与 builder（喂全量）；
 * 2. 断言 builder 的三个丢弃计数与流的期望计数完全相等；
 * 3. 逐根逐字段比较两侧 bar 序列（失败信息定位首个不一致 bar 与字段）。
 *
 * 依赖 `legacy_bar_oracle.h`（旧版查表语义）、`bar_diff_streams.h`（确定性 tick 流）、
 * `bar_diff_compare.h`（逐字段比较器）、`bar_test_data.h`（sink / make_tick / 会话表）。
 */
#ifndef DZTRADER_STRATEGY_API_TESTS_BAR_DIFF_CASES_H_
#define DZTRADER_STRATEGY_API_TESTS_BAR_DIFF_CASES_H_

#include "bar_diff_compare.h"
#include "bar_diff_streams.h"
#include "bar_test_data.h"
#include "legacy_bar_oracle.h"

#include <dztrader/bar.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace dztrader::strategy_api::test {

/**
 * @brief 一条差分用例的规模参数
 *
 * fast 档用最小规模（单 seed、少交易日、少 tick）覆盖每条路径各一次；slow 档用多 seed /
 * 多日 / 高密度做全量覆盖。参数相同则行为完全相同（流生成器确定性）。
 */
struct DiffScale {
    uint32_t seed;               ///< 随机种子
    int32_t trading_days;        ///< 交易日数量
    std::size_t ticks_per_slot;  ///< 每小节（日线为每日）tick 数
};

/// @brief 断言 builder 的丢弃计数与 tick 流的期望计数完全一致
inline void expect_stats_match(const dztrader::bar::BarBuilderStats& stats,
                               const DiffStream& stream,
                               const std::string& scenario) {
    EXPECT_EQ(stats.out_of_session, stream.out_of_session)
        << "out_of_session mismatch | scenario=" << scenario;
    EXPECT_EQ(stats.out_of_order, stream.out_of_order)
        << "out_of_order mismatch | scenario=" << scenario;
    EXPECT_EQ(stats.bad_volume, stream.bad_volume) << "bad_volume mismatch | scenario=" << scenario;
}

/**
 * @brief 日内周期差分用例：干净流 × 指定周期
 * @param table 交易小节与竞价窗口表（必须可被旧版查表语义表达）
 * @param period_sec 周期秒数
 * @param scale 规模参数
 */
inline void run_intraday_clean_case(const dztrader::bar::TradingSessionTable& table,
                                    int32_t period_sec,
                                    const DiffScale& scale) {
    const std::string scenario = "intraday_clean seed=" + std::to_string(scale.seed) +
                                 " days=" + std::to_string(scale.trading_days) +
                                 " period_sec=" + std::to_string(period_sec);
    const DiffStream stream =
        make_random_clean_stream(scale.seed, table, scale.trading_days, scale.ticks_per_slot);
    // 干净流不得含任何应被守卫丢弃的 tick，否则 oracle 与 builder 的输入不一致
    ASSERT_EQ(stream.out_of_session, 0U)
        << "clean stream must be fully accepted | scenario=" << scenario;
    ASSERT_EQ(stream.out_of_order, 0U)
        << "clean stream must be fully accepted | scenario=" << scenario;
    ASSERT_EQ(stream.bad_volume, 0U)
        << "clean stream must be fully accepted | scenario=" << scenario;
    const std::vector<DzTick> ticks = stream.accepted_ticks();
    ASSERT_FALSE(ticks.empty()) << "clean stream is empty | scenario=" << scenario;

    LegacyBarOracle oracle(table, period_sec);
    RecordingSink sink;
    dztrader::bar::IntradayBarBuilder<RecordingSink> builder(table, period_sec, sink);
    for (const DzTick& tick : ticks) {
        oracle.on_tick(tick);
        builder.on_tick(tick);
    }
    expect_stats_match(builder.stats(), stream, scenario);
    expect_sequences_equal(emitted_bars(sink, builder), oracle.bars(), scenario);
}

/**
 * @brief 日内周期差分用例：守卫流（乱序 / 坏量 / 非交易时段 / 同秒重复）× 指定周期
 *
 * 守卫 tick 只喂 builder；oracle 只收 `None` 的 tick。三类计数同时断言 > 0（防生成器退化后
 * 守卫断言空转）且与 builder.stats() 精确相等。
 */
inline void run_intraday_guarded_case(const dztrader::bar::TradingSessionTable& table,
                                      int32_t period_sec,
                                      const DiffScale& scale) {
    const std::string scenario = "intraday_guarded seed=" + std::to_string(scale.seed) +
                                 " days=" + std::to_string(scale.trading_days) +
                                 " period_sec=" + std::to_string(period_sec);
    const DiffStream stream =
        make_random_guarded_stream(scale.seed, table, scale.trading_days, scale.ticks_per_slot);
    ASSERT_GT(stream.out_of_session, 0U)
        << "guarded stream lost out_of_session cases | scenario=" << scenario;
    ASSERT_GT(stream.out_of_order, 0U)
        << "guarded stream lost out_of_order cases | scenario=" << scenario;
    ASSERT_GT(stream.bad_volume, 0U)
        << "guarded stream lost bad_volume cases | scenario=" << scenario;

    LegacyBarOracle oracle(table, period_sec);
    RecordingSink sink;
    dztrader::bar::IntradayBarBuilder<RecordingSink> builder(table, period_sec, sink);
    for (const DiffTick& tick : stream.ticks) {
        if (tick.expect == DiffDrop::None) {
            oracle.on_tick(tick.tick);
        }
        builder.on_tick(tick.tick);
    }
    expect_stats_match(builder.stats(), stream, scenario);
    expect_sequences_equal(emitted_bars(sink, builder), oracle.bars(), scenario);
}

/**
 * @brief 日内周期差分用例：竞价流（Open 窗口归后一小节首根、逐笔覆盖 O=H=L=C）
 * @param table 含可表达 Open 竞价窗口的表
 * @param period_sec 周期秒数
 * @param scale 规模参数
 */
inline void run_intraday_auction_case(const dztrader::bar::TradingSessionTable& table,
                                      int32_t period_sec,
                                      const DiffScale& scale) {
    const std::string scenario =
        "intraday_auction sessions=" + std::to_string(table.session_count) +
        " auctions=" + std::to_string(table.auction_count) + " seed=" + std::to_string(scale.seed) +
        " period_sec=" + std::to_string(period_sec);
    const DiffStream stream =
        make_auction_clean_stream(scale.seed, table, scale.trading_days, scale.ticks_per_slot);
    ASSERT_EQ(stream.out_of_session, 0U)
        << "auction stream must be fully accepted | scenario=" << scenario;
    ASSERT_EQ(stream.out_of_order, 0U)
        << "auction stream must be fully accepted | scenario=" << scenario;
    ASSERT_EQ(stream.bad_volume, 0U)
        << "auction stream must be fully accepted | scenario=" << scenario;
    const std::vector<DzTick> ticks = stream.accepted_ticks();
    ASSERT_FALSE(ticks.empty()) << "auction stream is empty | scenario=" << scenario;

    LegacyBarOracle oracle(table, period_sec);
    RecordingSink sink;
    dztrader::bar::IntradayBarBuilder<RecordingSink> builder(table, period_sec, sink);
    for (const DzTick& tick : ticks) {
        oracle.on_tick(tick);
        builder.on_tick(tick);
    }
    expect_stats_match(builder.stats(), stream, scenario);
    expect_sequences_equal(emitted_bars(sink, builder), oracle.bars(), scenario);
}

/**
 * @brief 日内周期差分用例：边界流（小节 begin/begin+1s/end-1s/end、同秒重复、休息后恢复）
 * @param table 交易小节表
 * @param period_sec 周期秒数
 */
inline void run_intraday_edge_case(const dztrader::bar::TradingSessionTable& table,
                                   int32_t period_sec,
                                   int32_t trading_days) {
    const std::string scenario = "intraday_edge sessions=" + std::to_string(table.session_count) +
                                 " period_sec=" + std::to_string(period_sec);
    const DiffStream stream = make_edge_clean_stream(table, trading_days);
    ASSERT_FALSE(stream.ticks.empty()) << "edge stream is empty | scenario=" << scenario;

    LegacyBarOracle oracle(table, period_sec);
    RecordingSink sink;
    dztrader::bar::IntradayBarBuilder<RecordingSink> builder(table, period_sec, sink);
    for (const DiffTick& tick : stream.ticks) {
        if (tick.expect == DiffDrop::None) {
            oracle.on_tick(tick.tick);
        }
        builder.on_tick(tick.tick);
    }
    expect_stats_match(builder.stats(), stream, scenario);
    expect_sequences_equal(emitted_bars(sink, builder), oracle.bars(), scenario);
}

/**
 * @brief 日线差分用例：干净流（交易日切换 + 惰性收盘 + 当日累计字段映射）
 * @param scale 规模参数
 */
inline void run_daily_clean_case(const DiffScale& scale) {
    const std::string scenario = "daily_clean seed=" + std::to_string(scale.seed) +
                                 " days=" + std::to_string(scale.trading_days);
    const DiffStream stream =
        make_daily_random_stream(scale.seed, scale.trading_days, scale.ticks_per_slot);
    ASSERT_EQ(stream.out_of_session, 0U)
        << "clean daily stream must be fully accepted | scenario=" << scenario;
    ASSERT_EQ(stream.out_of_order, 0U)
        << "clean daily stream must be fully accepted | scenario=" << scenario;
    ASSERT_EQ(stream.bad_volume, 0U)
        << "clean daily stream must be fully accepted | scenario=" << scenario;
    const std::vector<DzTick> ticks = stream.accepted_ticks();
    ASSERT_FALSE(ticks.empty()) << "clean daily stream is empty | scenario=" << scenario;

    LegacyDailyBarOracle oracle;
    RecordingSink sink;
    dztrader::bar::DailyBarBuilder<RecordingSink> builder(sink);
    for (const DzTick& tick : ticks) {
        oracle.on_tick(tick);
        builder.on_tick(tick);
    }
    expect_stats_match(builder.stats(), stream, scenario);
    expect_sequences_equal(emitted_bars(sink, builder), oracle.bars(), scenario);
}

/**
 * @brief 日线差分用例：守卫流（time 越界 / tsec 或交易日倒退 / 累计量额倒退 / 同秒重复）
 * @param scale 规模参数
 */
inline void run_daily_guarded_case(const DiffScale& scale) {
    const std::string scenario = "daily_guarded seed=" + std::to_string(scale.seed) +
                                 " days=" + std::to_string(scale.trading_days);
    const DiffStream stream =
        make_daily_guarded_stream(scale.seed, scale.trading_days, scale.ticks_per_slot);
    ASSERT_GT(stream.out_of_session, 0U)
        << "guarded daily stream lost out_of_session cases | scenario=" << scenario;
    ASSERT_GT(stream.out_of_order, 0U)
        << "guarded daily stream lost out_of_order cases | scenario=" << scenario;
    ASSERT_GT(stream.bad_volume, 0U)
        << "guarded daily stream lost bad_volume cases | scenario=" << scenario;

    LegacyDailyBarOracle oracle;
    RecordingSink sink;
    dztrader::bar::DailyBarBuilder<RecordingSink> builder(sink);
    for (const DiffTick& tick : stream.ticks) {
        if (tick.expect == DiffDrop::None) {
            oracle.on_tick(tick.tick);
        }
        builder.on_tick(tick.tick);
    }
    expect_stats_match(builder.stats(), stream, scenario);
    expect_sequences_equal(emitted_bars(sink, builder), oracle.bars(), scenario);
}

/// @brief 人工混合场景流的一笔 tick（字面量表的一行；expect != None 的 tick 不喂 oracle）
struct HandTick {
    DzDate day;                        ///< 交易日
    DzTime clock;                      ///< 墙钟秒
    double price;                      ///< last_price
    DzVolume volume;                   ///< 当日累计成交量
    double turnover;                   ///< 当日累计成交额
    DzLargeVolume open_interest;       ///< 持仓量
    DiffDrop expect = DiffDrop::None;  ///< 期望归宿
};

/**
 * @brief 人工混合场景的 tick 表：一个完整交易日 + 次日夜盘，覆盖全部六类人工场景
 *
 * 场景覆盖（逐项可见，不依赖随机生成器）：
 * - **竞价**：20:59:10 / 20:59:40（夜盘开盘窗口）、09:29:20（早盘开盘窗口）；
 * - **休息**：10:15 早盘小节末端、跨 10:15–10:30 休息后 13:00 恢复、12:00 落在休息时段（丢弃）；
 * - **日切**：DAY1 夜盘 21:00 起、DAY2 夜盘 21:00 切日；
 * - **乱序**：DAY2 回退到 DAY2 的 22:00（晚于 23:00 之后到来，tsec 倒退，丢弃）；
 * - **坏量**：DAY2 累计成交量从上笔 20 降到 15（丢弃）；
 * - **同秒重复**：21:00 连续两笔、DAY2 21:00 连续两笔（同秒全部接受）。
 *
 * 返回值同时给出期望丢弃计数（与 builder.stats() 精确比对）。
 */
inline DiffStream make_handcrafted_stream() {
    constexpr DzDate DAY1 = TRADING_DAY;
    constexpr DzDate DAY2 = TRADING_DAY + 1;
    constexpr DzTime CLOCK_20_59_10 = 20 * 3600 + 59 * 60 + 10;
    constexpr DzTime CLOCK_20_59_40 = 20 * 3600 + 59 * 60 + 40;
    constexpr DzTime CLOCK_21_00_00 = 21 * 3600;
    constexpr DzTime CLOCK_21_04_59 = 21 * 3600 + 4 * 60 + 59;
    constexpr DzTime CLOCK_22_00_00 = 22 * 3600;
    constexpr DzTime CLOCK_23_00_00 = 23 * 3600;
    constexpr DzTime CLOCK_02_30_00 = 2 * 3600 + 30 * 60;
    constexpr DzTime CLOCK_09_29_20 = 9 * 3600 + 29 * 60 + 20;
    constexpr DzTime CLOCK_09_30_00 = 9 * 3600 + 30 * 60;
    constexpr DzTime CLOCK_10_00_00 = 10 * 3600;
    constexpr DzTime CLOCK_10_15_00 = 10 * 3600 + 15 * 60;
    constexpr DzTime CLOCK_12_00_00 = 12 * 3600;
    constexpr DzTime CLOCK_13_00_00 = 13 * 3600;
    constexpr DzTime CLOCK_15_00_00 = 15 * 3600;
    /// DAY2 的乱序 tick：比前一笔（23:00）更早的 22:00
    constexpr DzTime CLOCK_OOO_22_00_00 = 22 * 3600;

    const std::array<HandTick, 20> hand_ticks = {{
        // DAY1 夜盘开盘竞价（窗口 20:59:00–20:59:59）——竞价场景
        {DAY1, CLOCK_20_59_10, 100.0, 10, 1000.0, 5, DiffDrop::None},
        {DAY1, CLOCK_20_59_40, 101.0, 20, 2010.0, 6, DiffDrop::None},
        // DAY1 夜盘 21:00 起（同秒重复两笔）——同秒重复场景
        {DAY1, CLOCK_21_00_00, 102.0, 30, 3030.0, 7, DiffDrop::None},
        {DAY1, CLOCK_21_00_00, 103.0, 30, 3030.0, 7, DiffDrop::None},
        {DAY1, CLOCK_21_04_59, 104.0, 40, 4040.0, 8, DiffDrop::None},
        {DAY1, CLOCK_22_00_00, 105.0, 50, 5050.0, 9, DiffDrop::None},
        // 夜盘末端 02:30——小节末端 tick 归前一根
        {DAY1, CLOCK_02_30_00, 106.0, 60, 6060.0, 10, DiffDrop::None},
        // DAY1 早盘开盘竞价（窗口 09:29:00–09:29:59）——竞价场景
        {DAY1, CLOCK_09_29_20, 107.0, 70, 7070.0, 11, DiffDrop::None},
        {DAY1, CLOCK_09_30_00, 108.0, 80, 8080.0, 12, DiffDrop::None},
        {DAY1, CLOCK_10_00_00, 109.0, 90, 9090.0, 13, DiffDrop::None},
        // 10:15 早盘小节末端 + 10:30 休息后恢复（同属 10:00–11:00 的 1 小时桶）——休息场景
        {DAY1, CLOCK_10_15_00, 110.0, 100, 10099.0, 14, DiffDrop::None},
        // 12:00 落在 11:30–13:00 休息时段——休息场景（丢弃）
        {DAY1, CLOCK_12_00_00, 111.0, 110, 11110.0, 15, DiffDrop::OutOfSession},
        {DAY1, CLOCK_13_00_00, 112.0, 120, 12120.0, 16, DiffDrop::None},
        {DAY1, CLOCK_15_00_00, 113.0, 130, 13130.0, 17, DiffDrop::None},
        // DAY2 夜盘 21:00 切日（同秒重复两笔）——日切 + 同秒重复场景
        {DAY2, CLOCK_21_00_00, 200.0, 10, 2000.0, 20, DiffDrop::None},
        {DAY2, CLOCK_21_00_00, 201.0, 10, 2000.0, 20, DiffDrop::None},
        {DAY2, CLOCK_23_00_00, 202.0, 20, 4010.0, 21, DiffDrop::None},
        // tsec 倒退到 22:00——乱序场景（丢弃）
        {DAY2, CLOCK_OOO_22_00_00, 203.0, 20, 4010.0, 21, DiffDrop::OutOfOrder},
        // 累计成交量从 20 降到 15——坏量场景（丢弃）
        {DAY2, CLOCK_23_00_00, 204.0, 15, 5000.0, 22, DiffDrop::BadVolume},
        // 非交易时段 12:00（DAY2 无对应小节数据）——休息/非交易时段场景（丢弃）
        {DAY2, CLOCK_12_00_00, 205.0, 25, 6000.0, 23, DiffDrop::OutOfSession},
    }};

    DiffStream stream;
    stream.ticks.reserve(hand_ticks.size());
    for (const HandTick& item : hand_ticks) {
        DiffTick tick;
        tick.tick = make_tick(item.clock, item.price, item.volume, item.turnover,
                              item.open_interest, item.day);
        tick.expect = item.expect;
        if (tick.expect == DiffDrop::BadVolume) {
            ++stream.bad_volume;
        } else if (tick.expect == DiffDrop::OutOfSession) {
            ++stream.out_of_session;
        } else if (tick.expect == DiffDrop::OutOfOrder) {
            ++stream.out_of_order;
        }
        stream.ticks.push_back(tick);
    }
    return stream;
}

/**
 * @brief 人工混合场景差分用例：六类场景混合流 × 指定周期
 * @param table 含可表达 Open 竞价窗口的多小节表
 * @param period_sec 周期秒数
 */
inline void run_handcrafted_case(const dztrader::bar::TradingSessionTable& table,
                                 int32_t period_sec) {
    const DiffStream stream = make_handcrafted_stream();
    // 六类人工场景必须都在流里出现，否则用例退化为「只测干净路径」
    ASSERT_GT(stream.out_of_session, 0U) << "handcrafted stream lost out_of_session cases";
    ASSERT_GT(stream.out_of_order, 0U) << "handcrafted stream lost out_of_order cases";
    ASSERT_GT(stream.bad_volume, 0U) << "handcrafted stream lost bad_volume cases";

    const std::string scenario = "handcrafted period_sec=" + std::to_string(period_sec);
    LegacyBarOracle oracle(table, period_sec);
    RecordingSink sink;
    dztrader::bar::IntradayBarBuilder<RecordingSink> builder(table, period_sec, sink);
    for (const DiffTick& tick : stream.ticks) {
        if (tick.expect == DiffDrop::None) {
            oracle.on_tick(tick.tick);
        }
        builder.on_tick(tick.tick);
    }
    expect_stats_match(builder.stats(), stream, scenario);
    expect_sequences_equal(emitted_bars(sink, builder), oracle.bars(), scenario);
}

}  // namespace dztrader::strategy_api::test

#endif  // DZTRADER_STRATEGY_API_TESTS_BAR_DIFF_CASES_H_
