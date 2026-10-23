/**
 * @file daily_bar_restore_test.cpp
 * @brief 日线 builder 的续接（restore）与重置（reset）用例（#91）
 *
 * 续接语义：restore final 只恢复交易日、下一次更晚交易日的 tick 才开新日线（同交易日 tick
 * 属于已收日线）；restore forming 采纳为当前形成中日线（不重复 open）；reset 清空状态供
 * 回测重跑。
 */

#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

namespace {

/// @brief 构造一根用于日线续接测试的 bar（begin_sec=0、period_sec=0、量额与累计一致）
Bar make_daily_bar(DzDate trading_day,
                   int32_t last_tick_sec,
                   double open,
                   double high,
                   double low,
                   double close,
                   int64_t volume,
                   double turnover,
                   DzLargeVolume open_interest,
                   bool is_final) {
    Bar bar{};
    bar.trading_day = trading_day;
    bar.begin_sec = 0;
    bar.last_tick_sec = last_tick_sec;
    bar.period_sec = 0;
    bar.open = open;
    bar.high = high;
    bar.low = low;
    bar.close = close;
    bar.volume = volume;
    bar.turnover = turnover;
    bar.open_interest = open_interest;
    bar.cum_volume = volume;
    bar.cum_turnover = turnover;
    bar.is_final = is_final;
    return bar;
}

/// @brief 已收日线（最后 tick 15:00，当日累计量 1000、累计额 101000）
Bar restored_final_daily_bar() {
    return make_daily_bar(TRADING_DAY, TSEC_15_00, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7,
                          true);
}

/// @brief 形成中日线（最后 tick 10:00，当日累计量 1500、累计额 156000）
Bar restored_forming_daily_bar() {
    return make_daily_bar(TRADING_DAY, TSEC_10_00, 100.0, 105.0, 98.0, 104.0, 1500, 156000.0, 9,
                          false);
}

}  // namespace

class DailyBarRestoreTest : public ::testing::Test {
protected:
    RecordingSink sink_;
    DailyBarBuilder<RecordingSink> builder_{sink_};
};

// ---------------------------------------------------------------------------
// restore final：只恢复交易日，不建立形成中 bar
// ---------------------------------------------------------------------------

TEST_F(DailyBarRestoreTest, RestoreFinalDoesNotExposeCurrentBar) {
    builder_.restore(restored_final_daily_bar());

    EXPECT_FALSE(builder_.has_current_bar());
    EXPECT_TRUE(sink_.events.empty());
    const BarBuilderStats stats = builder_.stats();
    EXPECT_EQ(stats.out_of_session, 0U);
    EXPECT_EQ(stats.out_of_order, 0U);
    EXPECT_EQ(stats.bad_volume, 0U);
}

/// @brief 日线 begin_sec 恒为 0：restore final 后同交易日任何 tick 都属于已收日线
TEST_F(DailyBarRestoreTest, RestoreFinalDropsSameTradingDayTicks) {
    builder_.restore(restored_final_daily_bar());

    builder_.on_tick(make_daily_tick(15 * 3600, 100.0, 102.0, 99.0, 101.0, 1001, 101101.0, 7));
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1002, 101202.0, 7));

    EXPECT_FALSE(builder_.has_current_bar());
    EXPECT_TRUE(sink_.events.empty());
    EXPECT_EQ(builder_.stats().out_of_order, 2U);
    EXPECT_EQ(builder_.stats().out_of_session, 0U);
    EXPECT_EQ(builder_.stats().bad_volume, 0U);

    // 丢弃不消费恢复状态：更晚交易日的 tick 仍能正常开新日线
    builder_.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 5, 505.0, 3, TRADING_DAY + 1));
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY + 1);
    EXPECT_EQ(builder_.current_bar().volume, 5);
    EXPECT_EQ(builder_.stats().out_of_order, 2U);
}

TEST_F(DailyBarRestoreTest, RestoreFinalDropsBackwardTradingDay) {
    builder_.restore(restored_final_daily_bar());
    builder_.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 5, 505.0, 3, TRADING_DAY - 1));

    EXPECT_FALSE(builder_.has_current_bar());
    EXPECT_TRUE(sink_.events.empty());
    EXPECT_EQ(builder_.stats().out_of_order, 1U);
}

/// @brief 守卫顺序：非法价格先跳过、不计乱序；随后的合法同交易日 tick 才计 out_of_order
TEST_F(DailyBarRestoreTest, RestoreFinalSameDayInvalidPriceIsSkippedBeforeOrderGuard) {
    builder_.restore(restored_final_daily_bar());

    builder_.on_tick(make_daily_tick(15 * 3600, 100.0, 102.0, 99.0,
                                     std::numeric_limits<double>::quiet_NaN(), 1001, 101101.0, 7));
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
    EXPECT_EQ(builder_.stats().bad_volume, 0U);
    EXPECT_FALSE(builder_.has_current_bar());

    builder_.on_tick(make_daily_tick(15 * 3600, 100.0, 102.0, 99.0, 101.0, 1001, 101101.0, 7));
    EXPECT_EQ(builder_.stats().out_of_order, 1U);
    EXPECT_FALSE(builder_.has_current_bar());
}

TEST_F(DailyBarRestoreTest, RestoreFinalNextTradingDayOpensNewBarWithZeroBasis) {
    builder_.restore(restored_final_daily_bar());
    builder_.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 200, 10100.0, 3, TRADING_DAY + 1));

    // 恢复的已收日线不重复 close；新日线只有 open + update
    ASSERT_EQ(sink_.events.size(), 2U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Update);
    for (const BarEvent& event : sink_.events) {
        EXPECT_FALSE(event.bar.is_final);
        EXPECT_EQ(event.bar.trading_day, TRADING_DAY + 1);
    }

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.begin_sec, 0);
    EXPECT_EQ(bar.period_sec, 0);
    EXPECT_EQ(bar.last_tick_sec, TSEC_09_00);
    EXPECT_DOUBLE_EQ(bar.open, 50.0);
    EXPECT_DOUBLE_EQ(bar.high, 51.0);
    EXPECT_DOUBLE_EQ(bar.low, 49.0);
    EXPECT_DOUBLE_EQ(bar.close, 50.5);
    EXPECT_EQ(bar.volume, 200);  // 基准按日重置为 0，即本笔当日累计量
    EXPECT_DOUBLE_EQ(bar.turnover, 10100.0);
    EXPECT_EQ(bar.cum_volume, 200);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 10100.0);
    EXPECT_EQ(bar.open_interest, 3);
    EXPECT_FALSE(bar.is_final);
}

TEST_F(DailyBarRestoreTest, RestoreFinalStateSurvivesRejectedTicks) {
    builder_.restore(restored_final_daily_bar());

    builder_.on_tick(make_daily_tick(90000, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    EXPECT_EQ(builder_.stats().out_of_session, 1U);
    builder_.on_tick(make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 200,
                                     std::numeric_limits<double>::quiet_NaN(), 3, TRADING_DAY + 1));
    EXPECT_EQ(builder_.stats().bad_volume, 1U);
    EXPECT_FALSE(builder_.has_current_bar());

    builder_.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 200, 10100.0, 3, TRADING_DAY + 1));
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY + 1);
}

// ---------------------------------------------------------------------------
// restore forming：采纳为当前日线，不重复 open
// ---------------------------------------------------------------------------

TEST_F(DailyBarRestoreTest, RestoreFormingAdoptsCurrentBarWithoutOpenEvent) {
    builder_.restore(restored_forming_daily_bar());

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY);
    EXPECT_EQ(bar.begin_sec, 0);
    EXPECT_EQ(bar.period_sec, 0);
    EXPECT_EQ(bar.last_tick_sec, TSEC_10_00);
    EXPECT_DOUBLE_EQ(bar.open, 100.0);
    EXPECT_DOUBLE_EQ(bar.high, 105.0);
    EXPECT_DOUBLE_EQ(bar.low, 98.0);
    EXPECT_DOUBLE_EQ(bar.close, 104.0);
    EXPECT_EQ(bar.volume, 1500);
    EXPECT_DOUBLE_EQ(bar.turnover, 156000.0);
    EXPECT_EQ(bar.cum_volume, 1500);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 156000.0);
    EXPECT_EQ(bar.open_interest, 9);
    EXPECT_FALSE(bar.is_final);
    EXPECT_TRUE(sink_.events.empty());  // 视为已 open，不补发 open
}

TEST_F(DailyBarRestoreTest, RestoreFormingSameDayTickUpdatesWithoutOpen) {
    builder_.restore(restored_forming_daily_bar());
    builder_.on_tick(
        make_daily_tick(10 * 3600 + 30 * 60, 100.5, 106.0, 97.5, 105.0, 1600, 167600.0, 10));

    ASSERT_EQ(sink_.events.size(), 1U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Update);
    EXPECT_FALSE(sink_.events[0].bar.is_final);

    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.last_tick_sec, TSEC_10_30);
    EXPECT_DOUBLE_EQ(bar.open, 100.5);  // 直接从本笔 tick 的当日累计字段更新
    EXPECT_DOUBLE_EQ(bar.high, 106.0);
    EXPECT_DOUBLE_EQ(bar.low, 97.5);
    EXPECT_DOUBLE_EQ(bar.close, 105.0);
    EXPECT_EQ(bar.volume, 1600);  // 日线量即当日累计量（起点基准恒为 0）
    EXPECT_DOUBLE_EQ(bar.turnover, 167600.0);
    EXPECT_EQ(bar.cum_volume, 1600);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 167600.0);
    EXPECT_EQ(bar.open_interest, 10);
    expect_bar_equal(sink_.events[0].bar, bar);
}

TEST_F(DailyBarRestoreTest, RestoreFormingSameSecondTickIsAccepted) {
    builder_.restore(restored_forming_daily_bar());
    builder_.on_tick(make_daily_tick(10 * 3600, 100.0, 106.0, 97.5, 105.0, 1600, 167600.0, 10));

    ASSERT_EQ(sink_.events.size(), 1U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Update);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_10_00);
    EXPECT_DOUBLE_EQ(builder_.current_bar().close, 105.0);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
}

TEST_F(DailyBarRestoreTest, RestoreFormingEarlierTickIsDropped) {
    builder_.restore(restored_forming_daily_bar());
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 106.0, 97.5, 105.0, 1600, 167600.0, 10));

    EXPECT_TRUE(sink_.events.empty());
    EXPECT_EQ(builder_.stats().out_of_order, 1U);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_10_00);
    EXPECT_EQ(builder_.current_bar().cum_volume, 1500);
}

/// @brief restore forming 后沿用恢复 bar 的累计量额作同日差分的倒退守卫
TEST_F(DailyBarRestoreTest, RestoreFormingSameDayCumulativeRegressionIsDropped) {
    builder_.restore(restored_forming_daily_bar());  // 当日累计量 1500、累计额 156000
    builder_.on_tick(
        make_daily_tick(10 * 3600 + 10 * 60, 100.0, 106.0, 97.5, 105.0, 1400, 145600.0, 10));

    EXPECT_TRUE(sink_.events.empty());
    EXPECT_EQ(builder_.stats().bad_volume, 1U);
    EXPECT_EQ(builder_.current_bar().cum_volume, 1500);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_10_00);
}

TEST_F(DailyBarRestoreTest, RestoreFormingNextTradingDayClosesThenOpens) {
    builder_.restore(restored_forming_daily_bar());
    builder_.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 200, 10100.0, 3, TRADING_DAY + 1));

    ASSERT_EQ(sink_.events.size(), 3U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Close);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[2].kind, EventKind::Update);

    // 恢复的形成中日线以 is_final=true 关闭，携带其原有数据
    const Bar& closed = sink_.events[0].bar;
    EXPECT_TRUE(closed.is_final);
    EXPECT_EQ(closed.trading_day, TRADING_DAY);
    EXPECT_EQ(closed.begin_sec, 0);
    EXPECT_EQ(closed.last_tick_sec, TSEC_10_00);
    EXPECT_DOUBLE_EQ(closed.open, 100.0);
    EXPECT_DOUBLE_EQ(closed.high, 105.0);
    EXPECT_DOUBLE_EQ(closed.low, 98.0);
    EXPECT_DOUBLE_EQ(closed.close, 104.0);
    EXPECT_EQ(closed.volume, 1500);
    EXPECT_DOUBLE_EQ(closed.turnover, 156000.0);

    // 新日线量额基准为 0
    const Bar& opened = sink_.events[1].bar;
    EXPECT_FALSE(opened.is_final);
    EXPECT_EQ(opened.trading_day, TRADING_DAY + 1);
    EXPECT_EQ(opened.volume, 200);
    EXPECT_DOUBLE_EQ(opened.turnover, 10100.0);
}

/// @brief 恢复 bar 的最后 tick 是 15:00，新交易日 09:00 的 tick 仍算前进（新交易日重新计时）
TEST_F(DailyBarRestoreTest, RestoreFormingNextTradingDayWithEarlierClockIsAccepted) {
    builder_.restore(make_daily_bar(TRADING_DAY, TSEC_15_00, 100.0, 105.0, 98.0, 104.0, 1500,
                                    156000.0, 9, false));
    builder_.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 200, 10100.0, 3, TRADING_DAY + 1));

    EXPECT_EQ(builder_.stats().out_of_order, 0U);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY + 1);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00);
}

// ---------------------------------------------------------------------------
// restore 前置条件校验：非法调用 fail-fast
// ---------------------------------------------------------------------------

TEST_F(DailyBarRestoreTest, RestoreRejectsPeriodMismatch) {
    Bar bar = restored_forming_daily_bar();
    bar.period_sec = 60;
    EXPECT_THROW(builder_.restore(bar), std::invalid_argument);
}

TEST_F(DailyBarRestoreTest, RestoreRejectsNonZeroBeginSec) {
    Bar bar = restored_forming_daily_bar();
    bar.begin_sec = TSEC_09_00;
    EXPECT_THROW(builder_.restore(bar), std::invalid_argument);
}

TEST_F(DailyBarRestoreTest, RestoreRejectsInvalidTradingDay) {
    Bar bar = restored_forming_daily_bar();
    bar.trading_day = 0;
    EXPECT_THROW(builder_.restore(bar), std::invalid_argument);
}

TEST_F(DailyBarRestoreTest, RestoreRejectsInvalidLastTickSec) {
    Bar too_late = restored_forming_daily_bar();
    too_late.last_tick_sec = 86400;
    EXPECT_THROW(builder_.restore(too_late), std::invalid_argument);

    Bar negative = restored_forming_daily_bar();
    negative.last_tick_sec = -1;
    EXPECT_THROW(builder_.restore(negative), std::invalid_argument);
}

/// @brief 日线 volume/turnover 即当日累计量额：续接 bar 与累计字段不一致时 fail-fast
TEST_F(DailyBarRestoreTest, RestoreRejectsInconsistentFormingCumulative) {
    Bar bar = restored_forming_daily_bar();
    bar.volume = bar.cum_volume - 100;  // 日线 bar 内量必须等于当日累计量
    EXPECT_THROW(builder_.restore(bar), std::invalid_argument);

    Bar turnover_bar = restored_forming_daily_bar();
    turnover_bar.turnover = turnover_bar.cum_turnover - 1.0;
    EXPECT_THROW(builder_.restore(turnover_bar), std::invalid_argument);
}

TEST_F(DailyBarRestoreTest, RestoreRejectsInvalidFinalCumulative) {
    Bar nan_bar = restored_final_daily_bar();
    nan_bar.cum_turnover = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(builder_.restore(nan_bar), std::invalid_argument);

    Bar infinite_bar = restored_final_daily_bar();
    infinite_bar.cum_turnover = std::numeric_limits<double>::infinity();
    EXPECT_THROW(builder_.restore(infinite_bar), std::invalid_argument);

    Bar negative_bar = restored_final_daily_bar();
    negative_bar.cum_volume = -1;
    EXPECT_THROW(builder_.restore(negative_bar), std::invalid_argument);

    Bar mismatch_bar = restored_final_daily_bar();
    mismatch_bar.volume = mismatch_bar.cum_volume + 1;
    EXPECT_THROW(builder_.restore(mismatch_bar), std::invalid_argument);
}

TEST_F(DailyBarRestoreTest, RestoreRejectsAfterTickReceived) {
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    EXPECT_THROW(builder_.restore(restored_forming_daily_bar()), std::invalid_argument);
}

TEST_F(DailyBarRestoreTest, RestoreRejectsAfterDroppedTick) {
    builder_.on_tick(make_daily_tick(90000, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    EXPECT_THROW(builder_.restore(restored_forming_daily_bar()), std::invalid_argument);
}

TEST_F(DailyBarRestoreTest, RestoreRejectsSecondRestoreWithoutReset) {
    builder_.restore(restored_final_daily_bar());
    EXPECT_THROW(builder_.restore(restored_forming_daily_bar()), std::invalid_argument);

    DailyBarBuilder<RecordingSink> forming_builder(sink_);
    forming_builder.restore(restored_forming_daily_bar());
    EXPECT_THROW(forming_builder.restore(restored_final_daily_bar()), std::invalid_argument);
}

TEST_F(DailyBarRestoreTest, RestoreRejectedThenValidRestoreStillAllowed) {
    Bar bar = restored_forming_daily_bar();
    bar.period_sec = 60;
    EXPECT_THROW(builder_.restore(bar), std::invalid_argument);

    builder_.restore(restored_forming_daily_bar());
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().cum_volume, 1500);
}

// ---------------------------------------------------------------------------
// reset：清空状态，保留 sink，可重新开始完整序列
// ---------------------------------------------------------------------------

TEST_F(DailyBarRestoreTest, ResetClearsBarAndStats) {
    builder_.on_tick(make_daily_tick(10 * 3600, 100.0, 105.0, 98.0, 104.0, 1500, 156000.0, 9));
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 105.0, 98.0, 104.0, 1600, 166400.0, 9));
    builder_.on_tick(make_daily_tick(11 * 3600, 100.0, 105.0, 98.0, 104.0, 1400, 145600.0, 9));
    builder_.on_tick(make_daily_tick(90000, 100.0, 105.0, 98.0, 104.0, 1500, 156000.0, 9));
    ASSERT_TRUE(builder_.has_current_bar());
    ASSERT_EQ(builder_.stats().out_of_order, 1U);
    ASSERT_EQ(builder_.stats().bad_volume, 1U);
    ASSERT_EQ(builder_.stats().out_of_session, 1U);

    builder_.reset();

    EXPECT_FALSE(builder_.has_current_bar());
    const BarBuilderStats stats = builder_.stats();
    EXPECT_EQ(stats.out_of_session, 0U);
    EXPECT_EQ(stats.out_of_order, 0U);
    EXPECT_EQ(stats.bad_volume, 0U);

    sink_.events.clear();
    builder_.on_tick(make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 50, 2525.0, 5));

    ASSERT_EQ(sink_.events.size(), 2U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Update);
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY);
    EXPECT_EQ(bar.volume, 50);  // 基准已清零
    EXPECT_DOUBLE_EQ(bar.turnover, 2525.0);
    EXPECT_EQ(bar.cum_volume, 50);
    EXPECT_FALSE(bar.is_final);
}

TEST_F(DailyBarRestoreTest, ResetAllowsRestoreAgain) {
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    builder_.reset();
    sink_.events.clear();

    builder_.restore(restored_forming_daily_bar());

    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().cum_volume, 1500);
    EXPECT_TRUE(sink_.events.empty());
}

TEST_F(DailyBarRestoreTest, ResetClearsRestoredFinalState) {
    builder_.restore(restored_final_daily_bar());
    builder_.reset();

    // 恢复期间会被判为已收日线的同交易日 tick：reset 后按全新序列接受
    builder_.on_tick(make_daily_tick(10 * 3600, 100.0, 102.0, 99.0, 101.0, 50, 5050.0, 5));

    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY);
    EXPECT_EQ(builder_.current_bar().volume, 50);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
}

TEST_F(DailyBarRestoreTest, ResetAllowsEarlierTradingDayAfterwards) {
    builder_.on_tick(
        make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7, TRADING_DAY));
    ASSERT_TRUE(builder_.has_current_bar());
    builder_.reset();
    sink_.events.clear();

    // reset 清空交易日基准：早于 reset 前 bar 的交易日照常开新日线，不算乱序
    builder_.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 7, 707.0, 5, TRADING_DAY - 1));

    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY - 1);
    EXPECT_EQ(builder_.current_bar().volume, 7);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
}
