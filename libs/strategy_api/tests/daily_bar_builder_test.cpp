/**
 * @file daily_bar_builder_test.cpp
 * @brief 日线 builder：首笔建根、同日更新、交易日切换与惰性收盘用例（#91）
 */

#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

namespace {

/// @brief 只实现 update 回调的 sink（缺失的 open/close 应为空操作）
class UpdateOnlySink {
public:
    void on_bar_update(const Bar& bar) {
        ++update_count;
        last_update = bar;
    }

    int update_count = 0;
    Bar last_update{};
};

/// @brief 不实现任何回调的 sink（缺失回调全部为空操作）
struct EmptySink {};

}  // namespace

/// @brief 日线首笔 tick 直接取当日累计字段建立形成中 bar（begin_sec=0、period_sec=0）
TEST(DailyBarBuilderTest, FirstTickFormsFormingDailyBar) {
    RecordingSink sink;
    DailyBarBuilder<RecordingSink> builder(sink);
    EXPECT_FALSE(builder.has_current_bar());

    builder.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));

    ASSERT_TRUE(builder.has_current_bar());
    const Bar& bar = builder.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY);
    EXPECT_EQ(bar.begin_sec, 0);   // 18:00 标记
    EXPECT_EQ(bar.period_sec, 0);  // 日线无周期
    EXPECT_EQ(bar.begin_clock(), 18 * 3600);
    EXPECT_EQ(bar.standard_end_sec(), 0);  // 日线以 trading_day 判断结束
    EXPECT_EQ(bar.last_tick_sec, TSEC_09_00);
    EXPECT_EQ(bar.last_tick_clock(), 9 * 3600);
    EXPECT_DOUBLE_EQ(bar.open, 100.0);
    EXPECT_DOUBLE_EQ(bar.high, 102.0);
    EXPECT_DOUBLE_EQ(bar.low, 99.0);
    EXPECT_DOUBLE_EQ(bar.close, 101.0);
    EXPECT_EQ(bar.volume, 1000);  // 日线起点基准恒为 0
    EXPECT_DOUBLE_EQ(bar.turnover, 101000.0);
    EXPECT_EQ(bar.open_interest, 7);
    EXPECT_EQ(bar.cum_volume, 1000);  // 与 bar 内量一致
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 101000.0);
    EXPECT_FALSE(bar.is_final);

    // 首笔：先 open（is_final=false）再 update，二者携带同一份 bar
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink.events[1].kind, EventKind::Update);
    for (const BarEvent& event : sink.events) {
        EXPECT_FALSE(event.bar.is_final);
        expect_bar_equal(event.bar, bar);
    }
}

/// @brief 同日后续 tick 直接用 tick 当日累计字段覆盖全部行情字段（不做 last_price 极值统计）
TEST(DailyBarBuilderTest, SameDayTicksUpdateFromCumulativeFields) {
    RecordingSink sink;
    DailyBarBuilder<RecordingSink> builder(sink);
    builder.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    builder.on_tick(make_daily_tick(10 * 3600, 100.5, 105.0, 97.5, 104.0, 1500, 156000.0, 9));

    // open 也取 tick 的当日累计字段（交易所同日内 open_price 不变）
    const Bar& bar = builder.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY);
    EXPECT_EQ(bar.begin_sec, 0);
    EXPECT_EQ(bar.period_sec, 0);
    EXPECT_EQ(bar.last_tick_sec, TSEC_10_00);
    EXPECT_DOUBLE_EQ(bar.open, 100.5);
    EXPECT_DOUBLE_EQ(bar.high, 105.0);
    EXPECT_DOUBLE_EQ(bar.low, 97.5);
    EXPECT_DOUBLE_EQ(bar.close, 104.0);
    EXPECT_EQ(bar.volume, 1500);  // 日线量即当日累计量（起点基准 0）
    EXPECT_DOUBLE_EQ(bar.turnover, 156000.0);
    EXPECT_EQ(bar.open_interest, 9);
    EXPECT_EQ(bar.cum_volume, 1500);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 156000.0);
    EXPECT_FALSE(bar.is_final);

    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink.events[1].kind, EventKind::Update);
    EXPECT_EQ(sink.events[2].kind, EventKind::Update);
    EXPECT_FALSE(sink.events[2].bar.is_final);
    expect_bar_equal(sink.events[2].bar, bar);
}

/// @brief 日线 high/low 取 tick 的当日累计极值，不因 last_price 未触及而缺失（与日内容口径不同）
TEST(DailyBarBuilderTest, ExtremesComeFromTickCumulativeFieldsNotLastPrice) {
    RecordingSink sink;
    DailyBarBuilder<RecordingSink> builder(sink);
    builder.on_tick(make_daily_tick(9 * 3600, 100.0, 110.0, 90.0, 101.0, 1000, 101000.0, 7));

    const Bar& bar = builder.current_bar();
    EXPECT_DOUBLE_EQ(bar.high, 110.0);
    EXPECT_DOUBLE_EQ(bar.low, 90.0);
    EXPECT_DOUBLE_EQ(bar.close, 101.0);
}

/// @brief 交易日的后续 tick 只 update：不重复 open、不 close，最后一根保持形成中（惰性收盘）
TEST(DailyBarBuilderTest, LazyCloseKeepsLastBarForming) {
    RecordingSink sink;
    DailyBarBuilder<RecordingSink> builder(sink);
    builder.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    builder.on_tick(make_daily_tick(13 * 3600, 100.0, 103.0, 98.0, 102.0, 1400, 141400.0, 8));

    EXPECT_EQ(count_events(sink, EventKind::Open), 1);
    EXPECT_EQ(count_events(sink, EventKind::Update), 2);
    EXPECT_EQ(count_events(sink, EventKind::Close), 0);
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_FALSE(builder.current_bar().is_final);
    EXPECT_EQ(builder.current_bar().last_tick_sec, TSEC_13_00);
}

/// @brief 交易日变大：先 close 旧日线（is_final=true），再 open 新日线（is_final=false），随后
/// update
TEST(DailyBarBuilderTest, TradingDayChangeClosesOldThenOpensNew) {
    RecordingSink sink;
    DailyBarBuilder<RecordingSink> builder(sink);
    builder.on_tick(make_daily_tick(15 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    builder.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 200, 10100.0, 3, TRADING_DAY + 1));

    ASSERT_EQ(sink.events.size(), 5U);
    EXPECT_EQ(sink.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink.events[1].kind, EventKind::Update);
    EXPECT_EQ(sink.events[2].kind, EventKind::Close);
    EXPECT_EQ(sink.events[3].kind, EventKind::Open);
    EXPECT_EQ(sink.events[4].kind, EventKind::Update);

    // 旧日线以 is_final=true 关闭，携带它自己的最终数据
    const Bar& closed = sink.events[2].bar;
    EXPECT_TRUE(closed.is_final);
    EXPECT_EQ(closed.trading_day, TRADING_DAY);
    EXPECT_EQ(closed.begin_sec, 0);
    EXPECT_EQ(closed.last_tick_sec, TSEC_15_00);
    EXPECT_DOUBLE_EQ(closed.open, 100.0);
    EXPECT_DOUBLE_EQ(closed.high, 102.0);
    EXPECT_DOUBLE_EQ(closed.low, 99.0);
    EXPECT_DOUBLE_EQ(closed.close, 101.0);
    EXPECT_EQ(closed.volume, 1000);
    EXPECT_DOUBLE_EQ(closed.turnover, 101000.0);
    EXPECT_EQ(closed.open_interest, 7);

    // 新日线量额以 0 为基准，直接取新交易日首笔 tick 的当日累计字段
    ASSERT_TRUE(builder.has_current_bar());
    const Bar& current = builder.current_bar();
    EXPECT_EQ(current.trading_day, TRADING_DAY + 1);
    EXPECT_EQ(current.begin_sec, 0);
    EXPECT_EQ(current.period_sec, 0);
    EXPECT_EQ(current.last_tick_sec, TSEC_09_00);
    EXPECT_DOUBLE_EQ(current.open, 50.0);
    EXPECT_DOUBLE_EQ(current.high, 51.0);
    EXPECT_DOUBLE_EQ(current.low, 49.0);
    EXPECT_DOUBLE_EQ(current.close, 50.5);
    EXPECT_EQ(current.volume, 200);
    EXPECT_DOUBLE_EQ(current.turnover, 10100.0);
    EXPECT_EQ(current.cum_volume, 200);
    EXPECT_FALSE(current.is_final);
    expect_bar_equal(sink.events[3].bar, current);
    expect_bar_equal(sink.events[4].bar, current);
}

/// @brief 没有 tick 的交易日不生成空日线（相邻日线的 trading_day 可跳跃）
TEST(DailyBarBuilderTest, SkippedTradingDaysProduceNoEmptyBars) {
    RecordingSink sink;
    DailyBarBuilder<RecordingSink> builder(sink);
    builder.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    builder.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 200, 10100.0, 3, TRADING_DAY + 3));

    ASSERT_EQ(sink.events.size(), 5U);
    EXPECT_EQ(count_events(sink, EventKind::Close), 1);
    EXPECT_EQ(count_events(sink, EventKind::Open), 2);
    for (const BarEvent& event : sink.events) {
        EXPECT_TRUE(event.bar.trading_day == TRADING_DAY ||
                    event.bar.trading_day == TRADING_DAY + 3)
            << "empty trading day must not produce a bar, trading_day=" << event.bar.trading_day;
    }
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().trading_day, TRADING_DAY + 3);
}

/// @brief 交易日变大时即便墙钟时间更早也开新日线（新交易日重新计时，不算乱序）
TEST(DailyBarBuilderTest, ForwardTradingDayWithEarlierClockStartsNewBar) {
    RecordingSink sink;
    DailyBarBuilder<RecordingSink> builder(sink);
    builder.on_tick(make_daily_tick(15 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    builder.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 200, 10100.0, 3, TRADING_DAY + 1));

    const BarBuilderStats stats = builder.stats();
    EXPECT_EQ(stats.out_of_session, 0U);
    EXPECT_EQ(stats.out_of_order, 0U);
    EXPECT_EQ(stats.bad_volume, 0U);
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().trading_day, TRADING_DAY + 1);
    EXPECT_EQ(builder.current_bar().last_tick_sec, TSEC_09_00);
}

/// @brief 同一秒的多笔 tick 全部接受（乱序判定是严格小于）
TEST(DailyBarBuilderTest, TicksWithinSameSecondAreAllAccepted) {
    RecordingSink sink;
    DailyBarBuilder<RecordingSink> builder(sink);
    builder.on_tick(make_daily_tick(9 * 3600, 100.0, 100.5, 99.5, 100.2, 1000, 100200.0, 7));
    builder.on_tick(make_daily_tick(9 * 3600, 100.0, 101.0, 99.5, 100.8, 1200, 120960.0, 8));
    builder.on_tick(make_daily_tick(9 * 3600, 100.0, 101.0, 99.0, 100.4, 1500, 150600.0, 9));

    const BarBuilderStats stats = builder.stats();
    EXPECT_EQ(stats.out_of_order, 0U);
    EXPECT_EQ(stats.bad_volume, 0U);
    EXPECT_EQ(count_events(sink, EventKind::Update), 3);
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().volume, 1500);
    EXPECT_EQ(builder.current_bar().last_tick_sec, TSEC_09_00);
}

/// @brief 缺失 open/close 回调的 sink 仍能驱动数据更新
TEST(DailyBarBuilderSinkTest, SinkWithMissingCallbacksStillCompilesAndUpdates) {
    UpdateOnlySink sink;
    DailyBarBuilder<UpdateOnlySink> builder(sink);
    builder.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    builder.on_tick(make_daily_tick(10 * 3600, 100.0, 103.0, 98.0, 102.0, 1200, 122400.0, 8));
    builder.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 200, 10100.0, 3, TRADING_DAY + 1));

    EXPECT_EQ(sink.update_count, 3);
    EXPECT_EQ(sink.last_update.trading_day, TRADING_DAY + 1);
    EXPECT_EQ(sink.last_update.volume, 200);
    EXPECT_FALSE(sink.last_update.is_final);
}

/// @brief 不实现任何回调的 sink 仍能正常合成日线
TEST(DailyBarBuilderSinkTest, SinkWithNoCallbacksStillCompiles) {
    EmptySink sink;
    DailyBarBuilder<EmptySink> builder(sink);
    builder.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    builder.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 200, 10100.0, 3, TRADING_DAY + 1));

    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().trading_day, TRADING_DAY + 1);
    EXPECT_EQ(builder.current_bar().volume, 200);
    EXPECT_FALSE(builder.current_bar().is_final);
}

/// @brief NullBarSink 可实例化，且默认模板参数与显式无回调 sink 行为一致
TEST(DailyBarBuilderSinkTest, NullBarSinkBehavesLikeSinkWithoutCallbacks) {
    NullBarSink null_sink;
    DailyBarBuilder<> default_builder(null_sink);
    EmptySink empty_sink;
    DailyBarBuilder<EmptySink> empty_builder(empty_sink);

    const DzTick ticks[] = {
        make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7),
        make_daily_tick(10 * 3600, 100.0, 103.0, 98.0, 102.0, 1200, 122400.0, 8),
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 200, 10100.0, 3, TRADING_DAY + 1),
    };
    for (const DzTick& tick : ticks) {
        default_builder.on_tick(tick);
        empty_builder.on_tick(tick);
        ASSERT_TRUE(default_builder.has_current_bar());
        ASSERT_TRUE(empty_builder.has_current_bar());
        expect_bar_equal(default_builder.current_bar(), empty_builder.current_bar());
    }

    // 默认 Sink 路径的数据也要有独立断言（避免只做两个 builder 的同义比较）
    const Bar& bar = default_builder.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY + 1);
    EXPECT_EQ(bar.begin_sec, 0);
    EXPECT_EQ(bar.period_sec, 0);
    EXPECT_EQ(bar.volume, 200);
    EXPECT_EQ(bar.cum_volume, 200);
    EXPECT_FALSE(bar.is_final);
}
