#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

namespace {

/// @brief 交易日 D 的次日
constexpr DzDate NEXT_TRADING_DAY = TRADING_DAY + 1;
/// @brief 交易日 D 的再次日
constexpr DzDate DAY_AFTER_NEXT = TRADING_DAY + 2;

/// @brief 14:59:00 的墙钟秒（日盘 09:00–15:00 的最后一个非末端分钟桶）
constexpr DzTime CLOCK_14_59 = 14 * 3600 + 59 * 60;
/// @brief 23:59:59 的墙钟秒（交易日内秒 21599）
constexpr DzTime CLOCK_23_59_59 = 23 * 3600 + 59 * 60 + 59;

/// @brief 断言一根 bar 的身份三元组（交易日 + 标准桶起点 + 周期）
void expect_bar_identity(const Bar& bar,
                         DzDate trading_day,
                         int32_t begin_sec,
                         int32_t period_sec) {
    EXPECT_EQ(bar.trading_day, trading_day);
    EXPECT_EQ(bar.begin_sec, begin_sec);
    EXPECT_EQ(bar.period_sec, period_sec);
}

}  // namespace

/// @brief 交易日切换：日末根收盘、跨日累计基准归零、日期倒退丢弃、无 tick 交易日不产 bar
///
/// 断言对象一律是可观察行为（sink 事件序列、current_bar()、stats()），不读实现内部状态。
/// 交易日 D = TRADING_DAY；跨日 tick 显式给出 `date`。
class IntradayBarTradingDayTest : public ::testing::Test {
protected:
    TradingSessionTable table_ = day_session_table();
    RecordingSink sink_;
    IntradayBarBuilder<RecordingSink> builder_{table_, 60, sink_};
};

/// @brief 跨日：先以 is_final=true 收旧日末根，再以 is_final=false 开新日首根
TEST_F(IntradayBarTradingDayTest, CrossDayClosesPreviousDayLastBarThenOpensNewDayFirstBar) {
    builder_.on_tick(make_tick(CLOCK_14_59, 100.0, 100, 10000.0, 7));  // 交易日 D 14:59
    builder_.on_tick(make_tick(15 * 3600, 101.0, 100, 10000.0, 8));    // 交易日 D 15:00（末端）
    builder_.on_tick(make_tick(9 * 3600, 99.0, 5, 500.0, 9, NEXT_TRADING_DAY));  // 交易日 D+1 09:00

    ASSERT_EQ(sink_.events.size(), 6U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[2].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[3].kind, EventKind::Close);  // 跨日先收旧日末根
    EXPECT_EQ(sink_.events[4].kind, EventKind::Open);   // 再开新日首根
    EXPECT_EQ(sink_.events[5].kind, EventKind::Update);

    // 旧日末根：交易日/桶/真实最后 tick 时间都是旧日的，内容未被新日 tick 污染
    const Bar& closed = sink_.events[3].bar;
    EXPECT_TRUE(closed.is_final);
    expect_bar_identity(closed, TRADING_DAY, TSEC_15_00 - 60, 60);
    EXPECT_EQ(closed.last_tick_sec, TSEC_15_00);  // 15:00:00 小节末端归前一根
    EXPECT_DOUBLE_EQ(closed.open, 100.0);
    EXPECT_DOUBLE_EQ(closed.high, 101.0);
    EXPECT_DOUBLE_EQ(closed.low, 100.0);
    EXPECT_DOUBLE_EQ(closed.close, 101.0);
    EXPECT_EQ(closed.volume, 100);
    EXPECT_DOUBLE_EQ(closed.turnover, 10000.0);
    EXPECT_EQ(closed.open_interest, 8);
    EXPECT_EQ(closed.cum_volume, 100);

    // 新日首根：先 open 后 update，两者携带同一笔 tick 的数据（O=H=L=C=last_price）
    for (std::size_t i = 4; i < sink_.events.size(); ++i) {
        const Bar& opened = sink_.events[i].bar;
        EXPECT_FALSE(opened.is_final);
        expect_bar_identity(opened, NEXT_TRADING_DAY, TSEC_09_00, 60);
        EXPECT_EQ(opened.last_tick_sec, TSEC_09_00);
        EXPECT_DOUBLE_EQ(opened.open, 99.0);
        EXPECT_DOUBLE_EQ(opened.high, 99.0);
        EXPECT_DOUBLE_EQ(opened.low, 99.0);
        EXPECT_DOUBLE_EQ(opened.close, 99.0);
        EXPECT_EQ(opened.volume, 5);
        EXPECT_DOUBLE_EQ(opened.turnover, 500.0);
        EXPECT_EQ(opened.open_interest, 9);
        EXPECT_EQ(opened.cum_volume, 5);
        EXPECT_DOUBLE_EQ(opened.cum_turnover, 500.0);
    }

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& current = builder_.current_bar();
    expect_bar_identity(current, NEXT_TRADING_DAY, TSEC_09_00, 60);
    EXPECT_FALSE(current.is_final);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
}

/// @brief 新交易日第一笔 tick 的量额 delta 以 0 为基准，绝不跨日累加
TEST_F(IntradayBarTradingDayTest, NewTradingDayFirstTickDeltaStartsFromZero) {
    builder_.on_tick(make_tick(9 * 3600, 100.0, 100, 10000.0, 7));  // 交易日 D 累计 100
    builder_.on_tick(make_tick(9 * 3600, 101.0, 7, 700.0, 8, NEXT_TRADING_DAY));  // 新日累计从 7 起
    builder_.on_tick(make_tick(9 * 3600 + 30, 102.0, 12, 1200.0, 9, NEXT_TRADING_DAY));

    // 新日首根：12 - 0 = 12（若沿用旧日基准会得到 12 - 100 = -88）
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, NEXT_TRADING_DAY);
    EXPECT_EQ(builder_.current_bar().volume, 12);
    EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, 1200.0);
    EXPECT_EQ(builder_.current_bar().cum_volume, 12);
    EXPECT_DOUBLE_EQ(builder_.current_bar().cum_turnover, 1200.0);

    const Bar* closed = last_event_bar(sink_, EventKind::Close);
    ASSERT_NE(closed, nullptr);
    EXPECT_EQ(closed->trading_day, TRADING_DAY);
    EXPECT_EQ(closed->volume, 100);  // 旧日末根保留旧日差分
    EXPECT_DOUBLE_EQ(closed->turnover, 10000.0);
    EXPECT_GE(builder_.current_bar().volume, 0);  // 绝不发布负量

    // 再次跨日：D+2 首笔累计从 3 起
    builder_.on_tick(make_tick(9 * 3600, 98.0, 3, 300.0, 10, DAY_AFTER_NEXT));
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, DAY_AFTER_NEXT);
    EXPECT_EQ(builder_.current_bar().volume, 3);
    EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, 300.0);
}

/// @brief 跨日落在同一分钟内（begin_sec 相同）：仍必须先收旧根再开新根，量额按日重置
TEST_F(IntradayBarTradingDayTest, CrossDayInSameBucketStillClosesAndReopens) {
    builder_.on_tick(make_tick(9 * 3600 + 30, 100.0, 50, 5000.0, 7));
    builder_.on_tick(make_tick(9 * 3600 + 30, 101.0, 4, 404.0, 8, NEXT_TRADING_DAY));

    ASSERT_EQ(count_events(sink_, EventKind::Close), 1);
    ASSERT_EQ(count_events(sink_, EventKind::Open), 2);
    const Bar* closed = last_event_bar(sink_, EventKind::Close);
    ASSERT_NE(closed, nullptr);
    EXPECT_TRUE(closed->is_final);
    expect_bar_identity(*closed, TRADING_DAY, TSEC_09_00, 60);
    EXPECT_EQ(closed->volume, 50);
    EXPECT_DOUBLE_EQ(closed->turnover, 5000.0);

    ASSERT_TRUE(builder_.has_current_bar());
    // 同一桶起点、交易日已前进：两根 bar 靠 trading_day 区分
    expect_bar_identity(builder_.current_bar(), NEXT_TRADING_DAY, TSEC_09_00, 60);
    EXPECT_EQ(builder_.current_bar().volume, 4);
    EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, 404.0);
    EXPECT_FALSE(builder_.current_bar().is_final);
}

/// @brief 交易日倒退（跨日之后）：丢弃、out_of_order 递增，当前 bar 与事件序列不变
TEST_F(IntradayBarTradingDayTest, RegressedTradingDayAfterCrossDayIsDroppedAndCounted) {
    builder_.on_tick(make_tick(CLOCK_14_59, 100.0, 100, 10000.0, 7));  // 交易日 D
    builder_.on_tick(make_tick(9 * 3600, 99.0, 5, 500.0, 9, NEXT_TRADING_DAY));
    ASSERT_TRUE(builder_.has_current_bar());
    const Bar reference = builder_.current_bar();
    const std::size_t events_before = sink_.events.size();

    // 旧交易日的更晚时刻：交易日倒退优先于 tsec 判定，必须丢弃
    builder_.on_tick(make_tick(15 * 3600, 200.0, 500, 50000.0, 77, TRADING_DAY));
    EXPECT_EQ(builder_.stats().out_of_order, 1U);
    EXPECT_EQ(builder_.stats().bad_volume, 0U);
    EXPECT_EQ(builder_.stats().out_of_session, 0U);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);
    EXPECT_EQ(sink_.events.size(), events_before);
}

/// @brief 首根之前就出现交易日倒退：不建立 bar 且 out_of_order 递增
TEST_F(IntradayBarTradingDayTest, RegressedTradingDayBeforeFirstBarIsDroppedAndCounted) {
    builder_.on_tick(make_tick(CLOCK_14_59, 100.0, 100, 10000.0, 7));

    builder_.on_tick(make_tick(CLOCK_14_59, 90.0, 1, 90.0, 1, TRADING_DAY - 1));
    EXPECT_EQ(builder_.stats().out_of_order, 1U);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY);
    EXPECT_DOUBLE_EQ(builder_.current_bar().close, 100.0);
}

/// @brief 跳过若干交易日后：只为有 tick 的交易日生成 bar，中间无空 bar
TEST_F(IntradayBarTradingDayTest, SkippedTradingDaysProduceNoEmptyBars) {
    constexpr DzDate AFTER_HOLIDAY = TRADING_DAY + 5;

    builder_.on_tick(make_tick(CLOCK_14_59, 100.0, 100, 10000.0, 7));  // 交易日 D
    builder_.on_tick(make_tick(9 * 3600, 101.0, 7, 707.0, 8, AFTER_HOLIDAY));

    EXPECT_EQ(count_events(sink_, EventKind::Open), 2);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 1);
    for (const BarEvent& event : sink_.events) {
        ASSERT_TRUE(event.bar.trading_day == TRADING_DAY || event.bar.trading_day == AFTER_HOLIDAY)
            << "empty trading day must not produce a bar, trading_day="
            << static_cast<int64_t>(event.bar.trading_day);
    }
    const Bar* closed = last_event_bar(sink_, EventKind::Close);
    ASSERT_NE(closed, nullptr);
    EXPECT_EQ(closed->trading_day, TRADING_DAY);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, AFTER_HOLIDAY);
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_00);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
}

/// @brief 整日无场内 tick（只有非交易时段 tick）：该交易日不生成任何 bar
TEST_F(IntradayBarTradingDayTest, TradingDayWithoutInSessionTickProducesNoBar) {
    builder_.on_tick(make_tick(15 * 3600, 100.0, 100, 10000.0, 7));  // 交易日 D 末根
    const Bar reference = builder_.current_bar();
    const std::size_t events_before = sink_.events.size();

    // 交易日 D+1 只有 08:00 的 tick（日盘 09:00–15:00 之外）：丢弃并计数
    builder_.on_tick(make_tick(8 * 3600, 200.0, 500, 50000.0, 77, NEXT_TRADING_DAY));
    EXPECT_EQ(builder_.stats().out_of_session, 1U);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
    EXPECT_EQ(sink_.events.size(), events_before);  // 非交易时段 tick 不触发收盘
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);  // 旧日末根仍是当前根

    // 交易日 D+2 第一笔场内 tick：直接收掉 D 的末根，D+1 从未生成 bar
    builder_.on_tick(make_tick(9 * 3600, 101.0, 7, 707.0, 8, DAY_AFTER_NEXT));
    EXPECT_EQ(count_events(sink_, EventKind::Close), 1);
    EXPECT_EQ(count_events(sink_, EventKind::Open), 2);
    const Bar* closed = last_event_bar(sink_, EventKind::Close);
    ASSERT_NE(closed, nullptr);
    EXPECT_EQ(closed->trading_day, TRADING_DAY);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, DAY_AFTER_NEXT);
}

/// @brief 有效跨日不得触碰任何丢弃计数
TEST_F(IntradayBarTradingDayTest, ValidCrossDayLeavesAllDiscardCountersUntouched) {
    const DzDate days[] = {TRADING_DAY, NEXT_TRADING_DAY, DAY_AFTER_NEXT, TRADING_DAY + 6};
    for (const DzDate day : days) {
        builder_.on_tick(make_tick(9 * 3600, 100.0, 10, 1000.0, 7, day));
        builder_.on_tick(make_tick(CLOCK_14_59, 102.0, 20, 2040.0, 9, day));
    }

    const BarBuilderStats stats = builder_.stats();
    EXPECT_EQ(stats.out_of_session, 0U);
    EXPECT_EQ(stats.out_of_order, 0U);
    EXPECT_EQ(stats.bad_volume, 0U);
    // 每个交易日两个不同桶（09:00、14:59）：每日 open 两次，其中 09:00 根在 14:59 换桶时收
    EXPECT_EQ(count_events(sink_, EventKind::Open), 8);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 7);
}

/// @brief 新交易日第一笔 tick 价格非法：只跳过，不切交易日（旧日末根保持形成中）
TEST_F(IntradayBarTradingDayTest, InvalidPriceOnNewTradingDayDoesNotSwitchTradingDay) {
    builder_.on_tick(make_tick(CLOCK_14_59, 100.0, 100, 10000.0, 7));
    const Bar reference = builder_.current_bar();
    const std::size_t events_before = sink_.events.size();

    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    builder_.on_tick(make_tick(9 * 3600, kNaN, 5, 500.0, 9, NEXT_TRADING_DAY));
    EXPECT_EQ(sink_.events.size(), events_before);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);

    // 随后合法的新日 tick 才完成跨日
    builder_.on_tick(make_tick(9 * 3600, 99.0, 5, 500.0, 9, NEXT_TRADING_DAY));
    EXPECT_EQ(count_events(sink_, EventKind::Close), 1);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, NEXT_TRADING_DAY);
}

/// @brief 新交易日第一笔 tick 累计量为负：按坏量丢弃，不切交易日、不产生负量 bar
TEST_F(IntradayBarTradingDayTest, BadVolumeOnNewTradingDayDoesNotSwitchTradingDay) {
    builder_.on_tick(make_tick(CLOCK_14_59, 100.0, 100, 10000.0, 7));
    const Bar reference = builder_.current_bar();

    builder_.on_tick(make_tick(9 * 3600, 99.0, -5, 500.0, 9, NEXT_TRADING_DAY));
    EXPECT_EQ(builder_.stats().bad_volume, 1U);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);

    builder_.on_tick(make_tick(9 * 3600, 99.0, 5, 500.0, 9, NEXT_TRADING_DAY));
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, NEXT_TRADING_DAY);
    EXPECT_EQ(builder_.current_bar().volume, 5);
    EXPECT_GE(builder_.current_bar().volume, 0);
}

/// @brief 夜盘 + 日盘同一交易日：跨午夜不切交易日（date 不变），只有桶切换
TEST(IntradayBarTradingDayNightTest, MidnightInsideNightSessionKeepsSameTradingDay) {
    const TradingSessionTable table = commodity_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    // 一个交易日的夜盘（21:00–次日 02:30）与日盘（09:30–11:30、13:00–15:00）
    const DzTime clocks[] = {
        21 * 3600,           // 21:00      → 桶 10800
        CLOCK_23_59_59,      // 23:59:59   → 桶 21540
        0,                   // 次日 00:00 → 桶 21600（同一交易日）
        30 * 60,             // 次日 00:30 → 桶 23400
        2 * 3600 + 30 * 60,  // 次日 02:30 夜盘末端 → 桶 30540
        9 * 3600 + 30 * 60,  // 09:30 早盘 → 桶 55800
        15 * 3600,           // 15:00 收盘末端 → 桶 75540
    };
    int32_t cumulative = 0;
    for (const DzTime clock : clocks) {
        cumulative += 10;
        builder.on_tick(make_tick(clock, 100.0, cumulative, static_cast<double>(cumulative), 7));
    }

    const std::vector<int32_t> expected_begin_sec = {10800, 21540, 21600, 23400,
                                                     30540, 55800, 75540};
    ASSERT_EQ(count_events(sink, EventKind::Open), 7);
    ASSERT_EQ(count_events(sink, EventKind::Close), 6);
    EXPECT_EQ(builder.stats().out_of_order, 0U);
    EXPECT_EQ(builder.stats().out_of_session, 0U);

    std::size_t index = 0;
    for (const BarEvent& event : sink.events) {
        EXPECT_EQ(event.bar.trading_day, TRADING_DAY);  // 全在同一交易日，跨午夜不切日
        if (event.kind == EventKind::Open) {
            ASSERT_LT(index, expected_begin_sec.size());
            EXPECT_EQ(event.bar.begin_sec, expected_begin_sec[index]);
            ++index;
        }
    }
    EXPECT_EQ(index, expected_begin_sec.size());

    // 同一交易日内量额基准沿用上一笔（每根 delta=10），跨午夜不重置
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().begin_sec, 75540);
    EXPECT_EQ(builder.current_bar().volume, 10);
    EXPECT_EQ(builder.current_bar().cum_volume, 70);
}

/// @brief 同一交易日跨午夜后 tsec 前进：旧桶不因「墙钟日翻页」被收盘
TEST(IntradayBarTradingDayNightTest, MidnightBoundaryTickOpensNewBucketWithinSameTradingDay) {
    const TradingSessionTable table = commodity_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    builder.on_tick(make_tick(CLOCK_23_59_59, 100.0, 10, 1000.0, 7));  // 桶 21540
    builder.on_tick(make_tick(0, 101.0, 12, 1212.0, 8));               // 桶 21600（跨午夜）

    ASSERT_EQ(count_events(sink, EventKind::Close), 1);
    const Bar* closed = last_event_bar(sink, EventKind::Close);
    ASSERT_NE(closed, nullptr);
    EXPECT_EQ(closed->trading_day, TRADING_DAY);
    EXPECT_EQ(closed->begin_sec, 21540);
    EXPECT_EQ(closed->last_tick_sec, 21599);
    EXPECT_TRUE(closed->is_final);
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().trading_day, TRADING_DAY);  // 交易日不变
    EXPECT_EQ(builder.current_bar().begin_sec, 21600);
    EXPECT_EQ(builder.current_bar().volume, 2);  // 12 - 10（同日沿用上一笔）
    EXPECT_EQ(builder.stats().out_of_order, 0U);
}

/// @brief 跨日进入夜盘后同日 tsec 倒退仍按乱序丢弃（基准是新交易日已接受的 tick）
TEST(IntradayBarTradingDayNightTest, SameDayRegressionAfterCrossDayNightOpenIsDropped) {
    const TradingSessionTable table = commodity_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    builder.on_tick(make_tick(15 * 3600, 100.0, 100, 10000.0, 7));  // 交易日 D 日盘末端
    builder.on_tick(make_tick(21 * 3600 + 30 * 60, 101.0, 5, 505.0, 8, NEXT_TRADING_DAY));
    ASSERT_TRUE(builder.has_current_bar());
    const Bar reference = builder.current_bar();
    const std::size_t events_before = sink.events.size();

    // 交易日 D+1 更早的夜盘时刻（21:00，tsec 10800 < 已接受的 12600）：乱序丢弃
    builder.on_tick(make_tick(21 * 3600, 200.0, 500, 50000.0, 77, NEXT_TRADING_DAY));
    EXPECT_EQ(builder.stats().out_of_order, 1U);
    EXPECT_EQ(builder.stats().bad_volume, 0U);
    ASSERT_TRUE(builder.has_current_bar());
    expect_bar_equal(builder.current_bar(), reference);
    EXPECT_EQ(sink.events.size(), events_before);
}

/// @brief 日盘末根 → 次日夜盘首根：跨日收盘/开盘，桶起点可在 tsec 轴上后退
TEST(IntradayBarTradingDayNightTest, DaySessionEndToNextTradingDayNightSession) {
    const TradingSessionTable table = commodity_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    builder.on_tick(make_tick(13 * 3600, 100.0, 100, 10000.0, 7));  // 交易日 D 13:00
    builder.on_tick(make_tick(15 * 3600, 102.0, 110, 11220.0, 8));  // 交易日 D 15:00（末端）
    ASSERT_TRUE(builder.has_current_bar());
    ASSERT_EQ(builder.current_bar().begin_sec, TSEC_15_00 - 60);

    // 次日（交易日 D+1）夜盘 21:00：tsec 10800 远小于旧日末桶，但交易日更大 -> 切日
    // 此前 13:00 → 15:00 已换桶收过一次，故此处是第 2 次 close
    builder.on_tick(make_tick(21 * 3600, 99.0, 5, 495.0, 9, NEXT_TRADING_DAY));
    ASSERT_EQ(count_events(sink, EventKind::Close), 2);
    const Bar* closed = last_event_bar(sink, EventKind::Close);
    ASSERT_NE(closed, nullptr);
    EXPECT_TRUE(closed->is_final);
    EXPECT_EQ(closed->trading_day, TRADING_DAY);
    EXPECT_EQ(closed->begin_sec, TSEC_15_00 - 60);
    EXPECT_EQ(closed->volume, 10);  // 14:59 桶：110 − 100（13:00 根收于 100）

    ASSERT_TRUE(builder.has_current_bar());
    // tsec 轴上后退：新日首根 begin_sec < 旧日末根 begin_sec，靠 trading_day 区分
    EXPECT_EQ(builder.current_bar().trading_day, NEXT_TRADING_DAY);
    EXPECT_EQ(builder.current_bar().begin_sec, TSEC_21_00);
    EXPECT_LT(builder.current_bar().begin_sec, closed->begin_sec);
    EXPECT_EQ(builder.current_bar().volume, 5);  // 跨日基准归零
    EXPECT_EQ(builder.stats().out_of_order, 0U);

    // 同一交易日 D+1 的日盘 09:30：同日内换桶，量额基准沿用夜盘最后一笔（5）
    builder.on_tick(make_tick(9 * 3600 + 30 * 60, 101.0, 9, 909.0, 10, NEXT_TRADING_DAY));
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().trading_day, NEXT_TRADING_DAY);
    EXPECT_EQ(builder.current_bar().begin_sec, TSEC_09_30);
    EXPECT_EQ(builder.current_bar().volume, 4);               // 9 - 5
    EXPECT_DOUBLE_EQ(builder.current_bar().turnover, 414.0);  // 909 - 495
    EXPECT_EQ(count_events(sink, EventKind::Close), 3);  // 13:00→15:00 换桶、跨日、21:00→09:30 换桶
}

/// @brief 夜盘末根 → 次日夜盘首根：交易日变大即可切日，tsec 变小不算乱序
TEST(IntradayBarTradingDayNightTest, NightSessionEndToNextTradingDayNightSession) {
    const TradingSessionTable table = commodity_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    builder.on_tick(make_tick(2 * 3600 + 30 * 60, 100.0, 60, 6000.0, 7));  // 交易日 D 夜盘末端
    ASSERT_TRUE(builder.has_current_bar());
    ASSERT_EQ(builder.current_bar().begin_sec, TSEC_02_30 - 60);

    builder.on_tick(make_tick(21 * 3600, 101.0, 3, 303.0, 8, NEXT_TRADING_DAY));  // 交易日 D+1 夜盘
    EXPECT_EQ(builder.stats().out_of_order, 0U);  // tsec 30600 → 10800 后退，但交易日前进
    ASSERT_EQ(count_events(sink, EventKind::Close), 1);
    const Bar* closed = last_event_bar(sink, EventKind::Close);
    ASSERT_NE(closed, nullptr);
    EXPECT_EQ(closed->trading_day, TRADING_DAY);
    EXPECT_EQ(closed->begin_sec, TSEC_02_30 - 60);
    EXPECT_EQ(closed->last_tick_sec, TSEC_02_30);
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().trading_day, NEXT_TRADING_DAY);
    EXPECT_EQ(builder.current_bar().begin_sec, TSEC_21_00);
    EXPECT_EQ(builder.current_bar().volume, 3);
}

/// @brief 连续多个交易日：每日首笔都以 0 为基准，互不污染，且全程无负量额
TEST_F(IntradayBarTradingDayTest, ConsecutiveTradingDaysResetBasisIndependently) {
    struct DayCase {
        DzDate day;                  ///< 交易日
        DzVolume cumulative_volume;  ///< 当日首笔累计量（每日都从更小的值重新开始）
        double cumulative_turnover;  ///< 当日首笔累计额
    };
    const DayCase cases[] = {
        {TRADING_DAY, 100, 10000.0},
        {NEXT_TRADING_DAY, 20, 2000.0},
        {DAY_AFTER_NEXT, 5, 505.0},
    };
    for (const DayCase& day_case : cases) {
        SCOPED_TRACE(::testing::Message() << "day=" << static_cast<int64_t>(day_case.day));
        builder_.on_tick(make_tick(9 * 3600, 100.0, day_case.cumulative_volume,
                                   day_case.cumulative_turnover, 7, day_case.day));
        ASSERT_TRUE(builder_.has_current_bar());
        EXPECT_EQ(builder_.current_bar().trading_day, day_case.day);
        EXPECT_EQ(builder_.current_bar().volume, day_case.cumulative_volume);
        EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, day_case.cumulative_turnover);
        EXPECT_GE(builder_.current_bar().volume, 0);
        EXPECT_GE(builder_.current_bar().turnover, 0.0);
    }
    EXPECT_EQ(count_events(sink_, EventKind::Open), 3);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 2);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
    EXPECT_EQ(builder_.stats().bad_volume, 0U);
}

/// @brief 1 小时周期跨日：旧日末根按整点网格收盘，新日首根开在次日自己的桶上
TEST(IntradayBarTradingDayHourTest, HourlyPeriodClosesPreviousDayBucketThenOpensNextDayBucket) {
    const TradingSessionTable table = day_break_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 3600, sink);

    builder.on_tick(make_tick(14 * 3600, 100.0, 100, 10000.0, 7));  // 交易日 D 14:00 → 14:00 桶
    builder.on_tick(
        make_tick(15 * 3600, 101.0, 110, 11110.0, 8));  // 交易日 D 15:00 末端归 14:00 桶
    ASSERT_TRUE(builder.has_current_bar());
    ASSERT_EQ(builder.current_bar().begin_sec, TSEC_15_00 - 3600);

    builder.on_tick(make_tick(10 * 3600, 99.0, 4, 396.0, 9, NEXT_TRADING_DAY));  // 交易日 D+1 10:00
    ASSERT_EQ(count_events(sink, EventKind::Close), 1);
    const Bar* closed = last_event_bar(sink, EventKind::Close);
    ASSERT_NE(closed, nullptr);
    EXPECT_TRUE(closed->is_final);
    EXPECT_EQ(closed->trading_day, TRADING_DAY);
    EXPECT_EQ(closed->begin_sec, TSEC_15_00 - 3600);
    EXPECT_EQ(closed->period_sec, 3600);
    EXPECT_EQ(closed->volume, 110);
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().trading_day, NEXT_TRADING_DAY);
    EXPECT_EQ(builder.current_bar().begin_sec, TSEC_10_00);
    EXPECT_EQ(builder.current_bar().period_sec, 3600);
    EXPECT_EQ(builder.current_bar().volume, 4);
    EXPECT_EQ(builder.stats().out_of_order, 0U);
}
