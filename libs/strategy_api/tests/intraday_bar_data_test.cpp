#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

/// @brief 数据口径：累计量额差分、重复快照零增量、OI 取最后一笔、H/L 只用 last_price
class IntradayBarDataTest : public ::testing::Test {
protected:
    TradingSessionTable table_ = day_session_table();
    RecordingSink sink_;
    IntradayBarBuilder<RecordingSink> builder_{table_, 60, sink_};
};

/// @brief 换桶后的新 bar 以「上一笔已接受 tick 的当日累计」为基准，同日沿用不重置
TEST_F(IntradayBarDataTest, NewBarDeltaUsesPreviousAcceptedTickAsBasis) {
    builder_.on_tick(make_tick(9 * 3600, 100.0, 10, 1005.0, 7));       // 09:00 桶
    builder_.on_tick(make_tick(9 * 3600 + 30, 101.0, 25, 2550.0, 8));  // 09:00 桶
    builder_.on_tick(make_tick(9 * 3600 + 60, 102.0, 40, 4080.0, 9));  // 09:01 桶（换桶）

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.begin_sec, TSEC_09_00 + 60);
    EXPECT_EQ(bar.volume, 15);               // 40 - 25（同日沿用上一笔，不回到 0）
    EXPECT_DOUBLE_EQ(bar.turnover, 1530.0);  // 4080 - 2550
    EXPECT_EQ(bar.cum_volume, 40);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 4080.0);

    const Bar* closed = last_event_bar(sink_, EventKind::Close);
    ASSERT_NE(closed, nullptr);
    EXPECT_EQ(closed->volume, 25);  // 09:00 桶：25 - 0
    EXPECT_DOUBLE_EQ(closed->turnover, 2550.0);
}

/// @brief 重复快照（累计量额零增量）不放大成交量，但价格/OI/最后 tick 时间照常推进
TEST_F(IntradayBarDataTest, RepeatedSnapshotAddsZeroDelta) {
    builder_.on_tick(make_tick(9 * 3600, 100.0, 25, 2550.0, 7));
    builder_.on_tick(make_tick(9 * 3600 + 10, 101.0, 25, 2550.0, 8));  // 重复快照
    builder_.on_tick(make_tick(9 * 3600 + 20, 99.0, 25, 2550.0, 9));   // 再次重复

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.volume, 25);  // 不因重复快照变成 75
    EXPECT_DOUBLE_EQ(bar.turnover, 2550.0);
    EXPECT_EQ(bar.cum_volume, 25);
    EXPECT_EQ(bar.last_tick_sec, TSEC_09_00 + 20);
    EXPECT_DOUBLE_EQ(bar.high, 101.0);
    EXPECT_DOUBLE_EQ(bar.low, 99.0);
    EXPECT_DOUBLE_EQ(bar.close, 99.0);
    EXPECT_EQ(bar.open_interest, 9);
}

/// @brief 成交量与成交额同步累加：turnover 的 bar 内增量同样相对 bar 起始基准
///
/// 首笔 delta 以 0 为基准（见 NewBarDeltaUsesPreviousAcceptedTickAsBasis 的换桶对照）、
/// 多笔正增量累加由本用例与 RepeatedSnapshotAddsZeroDelta 共同覆盖。
TEST_F(IntradayBarDataTest, TurnoverTracksVolumeDeltaWithinBar) {
    // 成交额与成交量不成比例，可分辨「按额差分」与「按量推算」
    builder_.on_tick(make_tick(9 * 3600, 100.0, 10, 1000.0, 7));
    builder_.on_tick(make_tick(9 * 3600 + 30, 100.0, 20, 3500.0, 8));

    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().volume, 20);
    EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, 3500.0);

    builder_.on_tick(make_tick(9 * 3600 + 60, 100.0, 30, 9500.0, 9));  // 换桶
    EXPECT_EQ(builder_.current_bar().volume, 10);                      // 30 - 20
    EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, 6000.0);         // 9500 - 3500
}

/// @brief 持仓量取该 bar 最后一笔 tick，不累加、不取极值
TEST_F(IntradayBarDataTest, OpenInterestComesFromLastTick) {
    builder_.on_tick(make_tick(9 * 3600, 100.0, 10, 1000.0, 7));
    builder_.on_tick(make_tick(9 * 3600 + 10, 101.0, 12, 1212.0, 11));
    builder_.on_tick(make_tick(9 * 3600 + 20, 102.0, 15, 1530.0, 9));

    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().open_interest, 9);  // 最后一笔（既非 11 也非最大）
}

/// @brief high/low 只取 last_price：tick 自带的 highest_price/lowest_price 不参与
TEST_F(IntradayBarDataTest, HighLowIgnoreTickAggregateExtremes) {
    DzTick first = make_tick(9 * 3600, 100.0, 10, 1000.0, 7);
    first.highest_price = 500.0;
    first.lowest_price = 1.0;
    DzTick second = make_tick(9 * 3600 + 30, 103.0, 12, 1236.0, 8);
    second.highest_price = 900.0;
    second.lowest_price = 0.5;

    builder_.on_tick(first);
    builder_.on_tick(second);

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_DOUBLE_EQ(bar.open, 100.0);
    EXPECT_DOUBLE_EQ(bar.high, 103.0);  // 不是 500/900
    EXPECT_DOUBLE_EQ(bar.low, 100.0);   // 不是 1/0.5
    EXPECT_DOUBLE_EQ(bar.close, 103.0);
}

/// @brief 日内容 bar 不写 bid/ask/涨跌停/昨收：bar 全字段与不带这些字段的同一 tick 一致
TEST_F(IntradayBarDataTest, QuoteFieldsDoNotPolluteBar) {
    DzTick tick = make_tick(9 * 3600, 100.0, 10, 1000.0, 7);
    tick.pre_close_price = 99.0;
    tick.upper_limit_price = 110.0;
    tick.lower_limit_price = 90.0;
    tick.bid_price[0] = 99.5;
    tick.ask_price[0] = 100.5;

    builder_.on_tick(tick);
    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    // 逐字段字面量断言：报价字段没有可写入的落脚点
    EXPECT_EQ(bar.trading_day, TRADING_DAY);
    EXPECT_EQ(bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(bar.last_tick_sec, TSEC_09_00);
    EXPECT_EQ(bar.period_sec, 60);
    EXPECT_DOUBLE_EQ(bar.open, 100.0);
    EXPECT_DOUBLE_EQ(bar.high, 100.0);
    EXPECT_DOUBLE_EQ(bar.low, 100.0);
    EXPECT_DOUBLE_EQ(bar.close, 100.0);
    EXPECT_EQ(bar.volume, 10);
    EXPECT_DOUBLE_EQ(bar.turnover, 1000.0);
    EXPECT_EQ(bar.open_interest, 7);
    EXPECT_EQ(bar.cum_volume, 10);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 1000.0);
    EXPECT_FALSE(bar.is_final);
}
