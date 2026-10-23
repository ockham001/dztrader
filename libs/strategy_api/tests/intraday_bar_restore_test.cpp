/**
 * @file intraday_bar_restore_test.cpp
 * @brief 日内容 builder 的续接（restore）与重置（reset）用例（#90）
 *
 * 续接语义：restore final 只恢复交易日与累计基准、下一次更晚桶 tick 才开新 bar；
 * restore forming 采纳为当前形成中 bar（不重复 open）；reset 清空状态供回测重跑。
 */

#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

namespace {

/// @brief 构造一根用于续接测试的 bar
Bar make_bar(DzDate trading_day,
             int32_t begin_sec,
             int32_t last_tick_sec,
             int32_t period_sec,
             double open,
             double high,
             double low,
             double close,
             int64_t volume,
             double turnover,
             DzLargeVolume open_interest,
             int64_t cum_volume,
             double cum_turnover,
             bool is_final) {
    Bar bar{};
    bar.trading_day = trading_day;
    bar.begin_sec = begin_sec;
    bar.last_tick_sec = last_tick_sec;
    bar.period_sec = period_sec;
    bar.open = open;
    bar.high = high;
    bar.low = low;
    bar.close = close;
    bar.volume = volume;
    bar.turnover = turnover;
    bar.open_interest = open_interest;
    bar.cum_volume = cum_volume;
    bar.cum_turnover = cum_turnover;
    bar.is_final = is_final;
    return bar;
}

/// @brief 已收的 1 分钟 bar（默认 09:00 桶，累计量 1000、累计额 101000）
Bar restored_final_bar(int32_t begin_sec = TSEC_09_00, int32_t last_tick_sec = TSEC_09_00 + 59) {
    return make_bar(TRADING_DAY, begin_sec, last_tick_sec, 60, 100.0, 102.0, 99.0, 101.0, 100,
                    10100.0, 7, 1000, 101000.0, true);
}

/// @brief 形成中的 1 分钟 bar（09:00 桶，bar 内量额 100/10050、累计 1000/100500）
///
/// 起点基准 = 累计 - bar 内差分 = 900/90450，续接后的同桶 tick 必须据此重算量额。
Bar restored_forming_bar() {
    return make_bar(TRADING_DAY, TSEC_09_00, TSEC_09_00 + 30, 60, 100.0, 101.0, 99.0, 100.5, 100,
                    10050.0, 7, 1000, 100500.0, false);
}

}  // namespace

class IntradayBarRestoreTest : public ::testing::Test {
protected:
    TradingSessionTable table_ = day_session_table();
    RecordingSink sink_;
    IntradayBarBuilder<RecordingSink> builder_{table_, 60, sink_};
};

// ---------------------------------------------------------------------------
// restore final：只恢复基准，不建立形成中 bar
// ---------------------------------------------------------------------------

TEST_F(IntradayBarRestoreTest, RestoreFinalDoesNotExposeCurrentBar) {
    builder_.restore(restored_final_bar());

    EXPECT_FALSE(builder_.has_current_bar());
    EXPECT_TRUE(sink_.events.empty());
    const BarBuilderStats stats = builder_.stats();
    EXPECT_EQ(stats.out_of_session, 0U);
    EXPECT_EQ(stats.out_of_order, 0U);
    EXPECT_EQ(stats.bad_volume, 0U);
}

TEST_F(IntradayBarRestoreTest, RestoreFinalNextLaterBucketOpensNewBarWithRestoredBasis) {
    builder_.restore(restored_final_bar());
    builder_.on_tick(make_tick(9 * 3600 + 90, 103.0, 1010, 102010.0, 8));  // 09:01:30

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY);
    EXPECT_EQ(bar.begin_sec, TSEC_09_00 + 60);
    EXPECT_EQ(bar.last_tick_sec, TSEC_09_00 + 90);
    EXPECT_EQ(bar.period_sec, 60);
    EXPECT_DOUBLE_EQ(bar.open, 103.0);
    EXPECT_DOUBLE_EQ(bar.high, 103.0);
    EXPECT_DOUBLE_EQ(bar.low, 103.0);
    EXPECT_DOUBLE_EQ(bar.close, 103.0);
    EXPECT_EQ(bar.volume, 10);               // 1010 - 恢复的累计 1000
    EXPECT_DOUBLE_EQ(bar.turnover, 1010.0);  // 102010 - 恢复的累计 101000
    EXPECT_EQ(bar.cum_volume, 1010);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 102010.0);
    EXPECT_EQ(bar.open_interest, 8);
    EXPECT_FALSE(bar.is_final);

    // 恢复的已收 bar 不重复 close；新 bar 只有 open + update
    ASSERT_EQ(sink_.events.size(), 2U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Update);
    for (const BarEvent& event : sink_.events) {
        EXPECT_FALSE(event.bar.is_final);
        EXPECT_EQ(event.bar.begin_sec, TSEC_09_00 + 60);
    }
}

TEST_F(IntradayBarRestoreTest, RestoreFinalDropsSameOrEarlierBucketSameDay) {
    builder_.restore(restored_final_bar(TSEC_09_00 + 60, TSEC_09_00 + 119));

    // 同桶 tick：已收桶不得重新生成 bar
    builder_.on_tick(make_tick(9 * 3600 + 90, 100.0, 1001, 101101.0, 7));
    // 更早桶 tick：桶起点 09:00 不大于恢复 bar 的 09:01
    builder_.on_tick(make_tick(9 * 3600 + 30, 100.0, 1002, 101202.0, 7));

    EXPECT_FALSE(builder_.has_current_bar());
    EXPECT_TRUE(sink_.events.empty());
    EXPECT_EQ(builder_.stats().out_of_order, 2U);
    EXPECT_EQ(builder_.stats().out_of_session, 0U);
    EXPECT_EQ(builder_.stats().bad_volume, 0U);

    // 丢弃不消费恢复基准：更晚桶 tick 仍能正常开新 bar
    builder_.on_tick(make_tick(9 * 3600 + 120, 101.0, 1005, 101505.0, 8));  // 09:02:00
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_00 + 120);
    EXPECT_EQ(builder_.current_bar().volume, 5);
    EXPECT_EQ(builder_.stats().out_of_order, 2U);
}

TEST_F(IntradayBarRestoreTest, RestoreFinalNextTradingDayResetsBasisToZero) {
    builder_.restore(restored_final_bar());
    builder_.on_tick(make_tick(9 * 3600, 100.0, 20, 2020.0, 7, TRADING_DAY + 1));

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY + 1);
    EXPECT_EQ(bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(bar.volume, 20);
    EXPECT_DOUBLE_EQ(bar.turnover, 2020.0);
    EXPECT_EQ(bar.cum_volume, 20);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 2020.0);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
}

TEST_F(IntradayBarRestoreTest, RestoreFinalDropsBackwardTradingDay) {
    builder_.restore(restored_final_bar());
    builder_.on_tick(make_tick(9 * 3600, 100.0, 20, 2020.0, 7, TRADING_DAY - 1));

    EXPECT_FALSE(builder_.has_current_bar());
    EXPECT_TRUE(sink_.events.empty());
    EXPECT_EQ(builder_.stats().out_of_order, 1U);
}

TEST_F(IntradayBarRestoreTest, RestoreFinalStateSurvivesRejectedTicks) {
    builder_.restore(restored_final_bar());

    builder_.on_tick(make_tick(8 * 3600, 100.0, 900, 90000.0, 7));  // 非交易时段
    EXPECT_EQ(builder_.stats().out_of_session, 1U);
    builder_.on_tick(make_tick(9 * 3600 + 60, 100.0, 990, 99990.0, 7));  // 累计量倒退
    EXPECT_EQ(builder_.stats().bad_volume, 1U);
    EXPECT_FALSE(builder_.has_current_bar());

    builder_.on_tick(make_tick(9 * 3600 + 120, 100.0, 1005, 101505.0, 7));  // 09:02:00
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().volume, 5);
    EXPECT_EQ(builder_.current_bar().cum_volume, 1005);
}

TEST_F(IntradayBarRestoreTest, RestoreFinalAppliesBucketRuleToAuctionTicks) {
    // 09:00 小节的开盘竞价窗口 08:59:00–08:59:59，目标桶 = 09:00
    const TradingSessionTable table = day_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);
    builder.restore(restored_final_bar());

    builder.on_tick(make_tick(8 * 3600 + 59 * 60 + 30, 100.0, 1001, 101101.0, 7));
    EXPECT_FALSE(builder.has_current_bar());
    EXPECT_TRUE(sink.events.empty());
    EXPECT_EQ(builder.stats().out_of_order, 1U);
}

TEST_F(IntradayBarRestoreTest, RestoreFinalAcceptedAuctionTickStartsAuctionBarWithRestoredBasis) {
    // 11:00–11:29 是午盘（13:00）的开盘竞价窗口，目标桶 13:00 晚于恢复 bar 的 09:00 桶
    const TradingSessionTable table = overlapping_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);
    builder.restore(restored_final_bar());

    builder.on_tick(make_tick(11 * 3600 + 10 * 60, 105.0, 1010, 102010.0, 8));

    EXPECT_TRUE(sink.events.empty());  // 竞价窗口内零回调
    ASSERT_TRUE(builder.has_current_bar());
    const Bar& bar = builder.current_bar();
    EXPECT_EQ(bar.begin_sec, TSEC_13_00);
    EXPECT_EQ(bar.last_tick_sec, TradingTime::from_clock(11 * 3600 + 10 * 60));
    EXPECT_DOUBLE_EQ(bar.open, 105.0);
    EXPECT_EQ(bar.volume, 10);               // 1010 - 恢复的累计 1000
    EXPECT_DOUBLE_EQ(bar.turnover, 1010.0);  // 102010 - 恢复的累计 101000
    EXPECT_FALSE(bar.is_final);

    // 13:00 场内：暴露竞价 bar（补 open）后 update，量额基准仍为恢复的累计
    builder.on_tick(make_tick(13 * 3600, 106.0, 1020, 103020.0, 9));
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink.events[0].bar.begin_sec, TSEC_13_00);
    EXPECT_EQ(sink.events[1].kind, EventKind::Update);
    EXPECT_EQ(sink.events[1].bar.volume, 20);  // 1020 - 恢复的累计 1000
}

// ---------------------------------------------------------------------------
// restore forming：采纳为当前 bar，不重复 open
// ---------------------------------------------------------------------------

TEST_F(IntradayBarRestoreTest, RestoreFormingAdoptsCurrentBarWithoutOpenEvent) {
    builder_.restore(restored_forming_bar());

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY);
    EXPECT_EQ(bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(bar.last_tick_sec, TSEC_09_00 + 30);
    EXPECT_EQ(bar.period_sec, 60);
    EXPECT_DOUBLE_EQ(bar.open, 100.0);
    EXPECT_DOUBLE_EQ(bar.high, 101.0);
    EXPECT_DOUBLE_EQ(bar.low, 99.0);
    EXPECT_DOUBLE_EQ(bar.close, 100.5);
    EXPECT_EQ(bar.volume, 100);
    EXPECT_DOUBLE_EQ(bar.turnover, 10050.0);
    EXPECT_EQ(bar.cum_volume, 1000);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 100500.0);
    EXPECT_FALSE(bar.is_final);
    EXPECT_TRUE(sink_.events.empty());
}

TEST_F(IntradayBarRestoreTest, RestoreFormingSameBucketTickUpdatesWithoutOpen) {
    builder_.restore(restored_forming_bar());
    builder_.on_tick(make_tick(9 * 3600 + 45, 102.0, 1010, 101250.0, 9));

    ASSERT_EQ(sink_.events.size(), 1U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Update);
    EXPECT_FALSE(sink_.events[0].bar.is_final);

    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(bar.last_tick_sec, TSEC_09_00 + 45);
    EXPECT_DOUBLE_EQ(bar.open, 100.0);  // 恢复的 open 保留
    EXPECT_DOUBLE_EQ(bar.high, 102.0);
    EXPECT_DOUBLE_EQ(bar.low, 99.0);
    EXPECT_DOUBLE_EQ(bar.close, 102.0);
    EXPECT_EQ(bar.volume, 110);               // 1010 - (1000 - 100)
    EXPECT_DOUBLE_EQ(bar.turnover, 10800.0);  // 101250 - (100500 - 10050)
    EXPECT_EQ(bar.cum_volume, 1010);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 101250.0);
    EXPECT_EQ(bar.open_interest, 9);
    expect_bar_equal(sink_.events[0].bar, bar);
}

TEST_F(IntradayBarRestoreTest, RestoreFormingSameSecondTickIsAccepted) {
    builder_.restore(restored_forming_bar());
    builder_.on_tick(make_tick(9 * 3600 + 30, 100.6, 1001, 100601.0, 7));

    ASSERT_EQ(sink_.events.size(), 1U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Update);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00 + 30);
    EXPECT_DOUBLE_EQ(builder_.current_bar().close, 100.6);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
}

TEST_F(IntradayBarRestoreTest, RestoreFormingEarlierTickIsDropped) {
    builder_.restore(restored_forming_bar());
    builder_.on_tick(make_tick(9 * 3600, 100.0, 1001, 100601.0, 7));  // 早于 last_tick_sec

    EXPECT_TRUE(sink_.events.empty());
    EXPECT_EQ(builder_.stats().out_of_order, 1U);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00 + 30);
    EXPECT_EQ(builder_.current_bar().cum_volume, 1000);
}

TEST_F(IntradayBarRestoreTest, RestoreFormingCrossBucketClosesThenOpens) {
    builder_.restore(restored_forming_bar());
    builder_.on_tick(make_tick(9 * 3600 + 60, 103.0, 1020, 102300.0, 8));

    ASSERT_EQ(sink_.events.size(), 3U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Close);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[2].kind, EventKind::Update);

    // 恢复的形成中 bar 以 is_final=true 关闭，携带其原有数据
    const Bar& closed = sink_.events[0].bar;
    EXPECT_TRUE(closed.is_final);
    EXPECT_EQ(closed.begin_sec, TSEC_09_00);
    EXPECT_EQ(closed.last_tick_sec, TSEC_09_00 + 30);
    EXPECT_DOUBLE_EQ(closed.open, 100.0);
    EXPECT_DOUBLE_EQ(closed.high, 101.0);
    EXPECT_DOUBLE_EQ(closed.low, 99.0);
    EXPECT_DOUBLE_EQ(closed.close, 100.5);
    EXPECT_EQ(closed.volume, 100);
    EXPECT_DOUBLE_EQ(closed.turnover, 10050.0);

    // 新 bar 的量额基准接恢复 bar 的累计量额
    const Bar& opened = sink_.events[1].bar;
    EXPECT_FALSE(opened.is_final);
    EXPECT_EQ(opened.begin_sec, TSEC_09_00 + 60);
    EXPECT_DOUBLE_EQ(opened.open, 103.0);
    EXPECT_EQ(opened.volume, 20);               // 1020 - 累计 1000
    EXPECT_DOUBLE_EQ(opened.turnover, 1800.0);  // 102300 - 累计 100500
    expect_bar_equal(sink_.events[2].bar, builder_.current_bar());
}

TEST_F(IntradayBarRestoreTest, RestoreFormingCrossTradingDayClosesThenOpensWithZeroBasis) {
    builder_.restore(restored_forming_bar());
    builder_.on_tick(make_tick(9 * 3600, 100.0, 7, 707.0, 7, TRADING_DAY + 1));

    ASSERT_EQ(sink_.events.size(), 3U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Close);
    EXPECT_TRUE(sink_.events[0].bar.is_final);
    EXPECT_EQ(sink_.events[0].bar.trading_day, TRADING_DAY);

    const Bar& opened = sink_.events[1].bar;
    EXPECT_EQ(opened.trading_day, TRADING_DAY + 1);
    EXPECT_EQ(opened.volume, 7);
    EXPECT_DOUBLE_EQ(opened.turnover, 707.0);
    EXPECT_EQ(opened.cum_volume, 7);
}

TEST_F(IntradayBarRestoreTest, RestoreFormingNightSessionBarContinuesAcrossSessions) {
    const TradingSessionTable table = commodity_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);
    // 夜盘 21:00 桶的形成中 bar，最后一笔 21:05
    builder.restore(make_bar(TRADING_DAY, TSEC_21_00, TSEC_21_00 + 300, 60, 100.0, 101.0, 99.0,
                             100.5, 100, 10050.0, 7, 1000, 100500.0, false));

    // 同日日盘 09:30：跨小节、跨桶先 close 恢复 bar 再 open 新 bar
    builder.on_tick(make_tick(9 * 3600 + 30 * 60, 102.0, 1010, 101505.0, 8));

    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[0].kind, EventKind::Close);
    EXPECT_EQ(sink.events[0].bar.begin_sec, TSEC_21_00);
    EXPECT_TRUE(sink.events[0].bar.is_final);
    EXPECT_EQ(sink.events[1].kind, EventKind::Open);
    EXPECT_EQ(sink.events[1].bar.begin_sec, TSEC_09_30);
    EXPECT_EQ(sink.events[1].bar.volume, 10);  // 1010 - 恢复的累计 1000
    EXPECT_EQ(sink.events[1].bar.trading_day, TRADING_DAY);
}

TEST_F(IntradayBarRestoreTest, RestoreFormingAuctionBarContinuesAuctionOhlcOverride) {
    const TradingSessionTable table = day_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);
    // 归档的 forming 竞价 bar：O=H=L=C，最后一笔 08:59:30 落在 09:00 桶的 Open 窗口内
    builder.restore(make_bar(TRADING_DAY, TSEC_09_00,
                             TradingTime::from_clock(8 * 3600 + 59 * 60 + 30), 60, 100.5, 100.5,
                             100.5, 100.5, 10, 1005.0, 7, 10, 1005.0, false));
    ASSERT_TRUE(builder.has_current_bar());
    ASSERT_TRUE(sink.events.empty());  // 视为已 open，不补发 open

    builder.on_tick(make_tick(8 * 3600 + 59 * 60 + 40, 103.0, 15, 1545.0, 8));

    EXPECT_TRUE(sink.events.empty());  // 仍在竞价窗口内：零回调
    const Bar& bar = builder.current_bar();
    EXPECT_DOUBLE_EQ(bar.open, 103.0);  // 重启后继续按竞价口径覆盖 O=H=L=C
    EXPECT_DOUBLE_EQ(bar.high, 103.0);
    EXPECT_DOUBLE_EQ(bar.low, 103.0);
    EXPECT_DOUBLE_EQ(bar.close, 103.0);
    EXPECT_EQ(bar.volume, 15);  // 起始基准为 0
    EXPECT_EQ(bar.cum_volume, 15);
    EXPECT_EQ(bar.last_tick_sec, TradingTime::from_clock(8 * 3600 + 59 * 60 + 40));
}

TEST_F(IntradayBarRestoreTest, RestoreFormingAuctionBarSessionTickEmitsUpdateWithoutReopen) {
    const TradingSessionTable table = day_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);
    builder.restore(make_bar(TRADING_DAY, TSEC_09_00,
                             TradingTime::from_clock(8 * 3600 + 59 * 60 + 30), 60, 103.0, 103.0,
                             103.0, 103.0, 15, 1545.0, 8, 15, 1545.0, false));

    builder.on_tick(make_tick(9 * 3600 + 20, 104.0, 20, 2080.0, 9));  // 09:00:20 场内同桶

    ASSERT_EQ(sink.events.size(), 1U);  // 视为已 open：只有 update，不补 open
    EXPECT_EQ(sink.events[0].kind, EventKind::Update);
    const Bar& bar = builder.current_bar();
    EXPECT_DOUBLE_EQ(bar.open, 103.0);  // 竞价开盘价保留
    EXPECT_DOUBLE_EQ(bar.high, 104.0);
    EXPECT_DOUBLE_EQ(bar.close, 104.0);
    EXPECT_EQ(bar.volume, 20);  // 20 - 起始基准 0
    EXPECT_EQ(bar.last_tick_sec, TradingTime::from_clock(9 * 3600 + 20));
}

// ---------------------------------------------------------------------------
// restore 前置条件校验：非法调用 fail-fast
// ---------------------------------------------------------------------------

TEST_F(IntradayBarRestoreTest, RestoreRejectsPeriodMismatch) {
    Bar bar = restored_forming_bar();
    bar.period_sec = 300;
    EXPECT_THROW(builder_.restore(bar), std::invalid_argument);
}

TEST_F(IntradayBarRestoreTest, RestoreRejectsAfterTickReceived) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));
    EXPECT_THROW(builder_.restore(restored_forming_bar()), std::invalid_argument);
}

TEST_F(IntradayBarRestoreTest, RestoreRejectsAfterDroppedTick) {
    builder_.on_tick(make_tick(8 * 3600, 100.5, 10, 1005.0, 7));  // 非交易时段，被丢弃
    EXPECT_THROW(builder_.restore(restored_forming_bar()), std::invalid_argument);
}

TEST_F(IntradayBarRestoreTest, RestoreRejectsSecondRestoreWithoutReset) {
    builder_.restore(restored_final_bar());
    EXPECT_THROW(builder_.restore(restored_final_bar()), std::invalid_argument);
}

TEST_F(IntradayBarRestoreTest, RestoreRejectsBeginSecOutsideSessionTable) {
    Bar bar = restored_forming_bar();
    bar.begin_sec = 36000;  // 交易日内秒 04:00（墙钟），不落在 09:00–15:00 的任何桶
    EXPECT_THROW(builder_.restore(bar), std::invalid_argument);
}

TEST_F(IntradayBarRestoreTest, RestoreRejectsUnalignedBeginSec) {
    Bar bar = restored_forming_bar();
    bar.begin_sec = TSEC_09_00 + 30;  // 不是 60 秒网格起点
    EXPECT_THROW(builder_.restore(bar), std::invalid_argument);
}

TEST_F(IntradayBarRestoreTest, RestoreRejectsInvalidTradingDay) {
    Bar bar = restored_forming_bar();
    bar.trading_day = 0;
    EXPECT_THROW(builder_.restore(bar), std::invalid_argument);
}

TEST_F(IntradayBarRestoreTest, RestoreRejectsInvalidLastTickSec) {
    Bar bar = restored_forming_bar();
    bar.last_tick_sec = 86400;
    EXPECT_THROW(builder_.restore(bar), std::invalid_argument);
}

TEST_F(IntradayBarRestoreTest, RestoreRejectsInconsistentFormingCumulative) {
    Bar bar = restored_forming_bar();
    bar.volume = bar.cum_volume + 1;  // bar 内量大于当日累计量，无法反推起始基准
    EXPECT_THROW(builder_.restore(bar), std::invalid_argument);
}

TEST_F(IntradayBarRestoreTest, RestoreRejectsInvalidFinalCumulative) {
    Bar nan_bar = restored_final_bar();
    nan_bar.cum_turnover = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(builder_.restore(nan_bar), std::invalid_argument);

    Bar infinite_bar = restored_final_bar();
    infinite_bar.cum_turnover = std::numeric_limits<double>::infinity();
    EXPECT_THROW(builder_.restore(infinite_bar), std::invalid_argument);

    Bar negative_bar = restored_final_bar();
    negative_bar.cum_volume = -1;
    EXPECT_THROW(builder_.restore(negative_bar), std::invalid_argument);

    Bar negative_turnover_bar = restored_final_bar();
    negative_turnover_bar.cum_turnover = -1.0;
    EXPECT_THROW(builder_.restore(negative_turnover_bar), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// reset：清空状态，保留配置与 sink，可重新开始完整序列
// ---------------------------------------------------------------------------

TEST_F(IntradayBarRestoreTest, ResetClearsBarBasisAndStats) {
    builder_.on_tick(make_tick(9 * 3600 + 30, 100.5, 10, 1005.0, 7));
    builder_.on_tick(make_tick(9 * 3600, 99.0, 9, 900.0, 7));        // 交易日内秒倒退
    builder_.on_tick(make_tick(16 * 3600, 99.0, 9, 900.0, 7));       // 非交易时段（16:00 墙钟）
    builder_.on_tick(make_tick(9 * 3600 + 60, 101.0, 5, 500.0, 7));  // 累计量倒退
    ASSERT_TRUE(builder_.has_current_bar());
    ASSERT_EQ(builder_.stats().out_of_order, 1U);
    ASSERT_EQ(builder_.stats().out_of_session, 1U);
    ASSERT_EQ(builder_.stats().bad_volume, 1U);

    builder_.reset();

    EXPECT_FALSE(builder_.has_current_bar());
    const BarBuilderStats stats = builder_.stats();
    EXPECT_EQ(stats.out_of_session, 0U);
    EXPECT_EQ(stats.out_of_order, 0U);
    EXPECT_EQ(stats.bad_volume, 0U);

    sink_.events.clear();
    builder_.on_tick(make_tick(9 * 3600, 100.0, 50, 5000.0, 5));

    ASSERT_EQ(sink_.events.size(), 2U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Update);
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(bar.volume, 50);  // 基准已清零
    EXPECT_DOUBLE_EQ(bar.turnover, 5000.0);
    EXPECT_EQ(bar.cum_volume, 50);
    EXPECT_FALSE(bar.is_final);
}

TEST_F(IntradayBarRestoreTest, ResetAllowsRestoreAgain) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));
    builder_.reset();
    sink_.events.clear();

    builder_.restore(restored_forming_bar());

    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().cum_volume, 1000);
    EXPECT_TRUE(sink_.events.empty());
}

TEST_F(IntradayBarRestoreTest, ResetClearsRestoredFinalBasis) {
    builder_.restore(restored_final_bar(TSEC_09_00 + 60, TSEC_09_00 + 119));
    builder_.reset();

    // 恢复期间会被判为旧桶的 09:01 tick：reset 后按全新序列接受，且基准为 0
    builder_.on_tick(make_tick(9 * 3600 + 90, 100.0, 50, 5000.0, 5));

    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_00 + 60);
    EXPECT_EQ(builder_.current_bar().volume, 50);
    EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, 5000.0);
}

TEST_F(IntradayBarRestoreTest, ResetAllowsEarlierTradingDayAfterwards) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7, TRADING_DAY));
    ASSERT_TRUE(builder_.has_current_bar());
    builder_.reset();
    sink_.events.clear();

    // reset 清空交易日基准：早于 reset 前 bar 的交易日照常开新 bar，不算乱序
    builder_.on_tick(make_tick(9 * 3600, 50.0, 7, 707.0, 5, TRADING_DAY - 1));

    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY - 1);
    EXPECT_EQ(builder_.current_bar().volume, 7);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
}

TEST_F(IntradayBarRestoreTest, ResetDropsPendingAuctionBars) {
    const TradingSessionTable table = day_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    builder.on_tick(make_tick(8 * 3600 + 59 * 60 + 30, 100.0, 10, 1000.0, 7));  // 竞价窗口
    ASSERT_TRUE(builder.has_current_bar());
    ASSERT_TRUE(sink.events.empty());

    builder.reset();
    EXPECT_FALSE(builder.has_current_bar());
    sink.events.clear();

    builder.on_tick(make_tick(9 * 3600, 101.0, 20, 2020.0, 8));
    ASSERT_EQ(sink.events.size(), 2U);  // open + update，无补发的竞价 bar
    EXPECT_EQ(sink.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink.events[1].kind, EventKind::Update);
    EXPECT_EQ(sink.events[0].bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(sink.events[0].bar.volume, 20);  // reset 后基准为 0
}
