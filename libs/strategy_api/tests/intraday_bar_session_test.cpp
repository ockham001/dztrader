#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <cstdint>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

/// @brief 多小节与夜盘：交易时段判定、休息/非交易时段丢弃、小节末端归属
class IntradayBarSessionTest : public ::testing::Test {
protected:
    TradingSessionTable table_ = commodity_session_table();
    RecordingSink sink_;
    IntradayBarBuilder<RecordingSink> builder_{table_, 60, sink_};
};

TEST_F(IntradayBarSessionTest, NightSessionTicksInSameBucketOnlyUpdate) {
    builder_.on_tick(make_tick(21 * 3600, 100.0, 10, 1000.0, 7));       // 21:00:00
    builder_.on_tick(make_tick(21 * 3600 + 59, 102.0, 12, 1224.0, 8));  // 21:00:59 同桶

    EXPECT_EQ(builder_.stats().out_of_session, 0U);
    EXPECT_EQ(count_events(sink_, EventKind::Open), 1);
    EXPECT_EQ(count_events(sink_, EventKind::Update), 2);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 0);
    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY);
    EXPECT_EQ(bar.begin_sec, TSEC_21_00);
    EXPECT_EQ(bar.last_tick_sec, TSEC_21_00 + 59);
    EXPECT_DOUBLE_EQ(bar.high, 102.0);
    EXPECT_EQ(bar.volume, 12);
}

TEST_F(IntradayBarSessionTest, MultiSessionTableAssignsTicksToRespectiveSessionBuckets) {
    struct SessionTickCase {
        int32_t clock;
        int32_t begin_sec;
    };
    const SessionTickCase cases[] = {
        {21 * 3600, TSEC_21_00},           // 21:00:00 夜盘开始
        {23 * 3600, 18000},                // 23:00:00 夜盘
        {30 * 60, 23400},                  // 次日 00:30 夜盘
        {9 * 3600 + 30 * 60, TSEC_09_30},  // 09:30:00 早盘开始
        {10 * 3600, 57600},                // 10:00 早盘
        {13 * 3600, TSEC_13_00},           // 13:00:00 午盘开始
    };
    int32_t expected_opens = 0;
    for (const SessionTickCase& session_tick : cases) {
        SCOPED_TRACE(::testing::Message() << "clock=" << session_tick.clock);
        builder_.on_tick(make_tick(session_tick.clock, 100.0, 1, 100.0, 1));
        ++expected_opens;
        ASSERT_TRUE(builder_.has_current_bar());
        EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY);
        EXPECT_EQ(builder_.current_bar().begin_sec, session_tick.begin_sec);
        EXPECT_EQ(count_events(sink_, EventKind::Open), expected_opens);
        EXPECT_EQ(count_events(sink_, EventKind::Close), expected_opens - 1);
    }
    EXPECT_EQ(builder_.stats().out_of_session, 0U);
}

TEST_F(IntradayBarSessionTest, TicksAcrossMidnightAdvanceInSameTradingDay) {
    builder_.on_tick(make_tick(23 * 3600 + 59 * 60 + 30, 100.0, 10, 1000.0, 7));  // 23:59:30
    ASSERT_TRUE(builder_.has_current_bar());
    ASSERT_EQ(builder_.current_bar().begin_sec, 21540);      // 23:59 桶
    ASSERT_EQ(builder_.current_bar().last_tick_sec, 21570);  // 23:59:30

    builder_.on_tick(make_tick(0, 101.0, 12, 1212.0, 8));  // 次日 00:00:00
    EXPECT_EQ(builder_.stats().out_of_session, 0U);
    EXPECT_EQ(count_events(sink_, EventKind::Open), 2);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 1);
    const Bar* last_close = last_event_bar(sink_, EventKind::Close);
    ASSERT_NE(last_close, nullptr);
    EXPECT_TRUE(last_close->is_final);
    EXPECT_EQ(last_close->begin_sec, 21540);  // 23:59 桶
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY);  // 夜盘归属同一交易日
    EXPECT_EQ(builder_.current_bar().begin_sec, 21600);          // 00:00 桶
    EXPECT_EQ(builder_.current_bar().last_tick_sec, 21600);
    EXPECT_FALSE(builder_.current_bar().is_final);

    builder_.on_tick(make_tick(30, 99.0, 15, 1500.0, 9));  // 00:00:30 同桶更新
    EXPECT_EQ(count_events(sink_, EventKind::Open), 2);
    EXPECT_EQ(count_events(sink_, EventKind::Update), 3);
    EXPECT_EQ(builder_.current_bar().begin_sec, 21600);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, 21630);  // 00:00:30
    EXPECT_DOUBLE_EQ(builder_.current_bar().low, 99.0);
    EXPECT_DOUBLE_EQ(builder_.current_bar().close, 99.0);
}

TEST_F(IntradayBarSessionTest, SessionBreakTicksAreDroppedAndCounted) {
    builder_.on_tick(make_tick(10 * 3600, 100.0, 10, 1000.0, 7));  // 10:00 早盘
    ASSERT_TRUE(builder_.has_current_bar());
    ASSERT_EQ(count_events(sink_, EventKind::Open), 1);
    ASSERT_EQ(count_events(sink_, EventKind::Update), 1);
    ASSERT_EQ(count_events(sink_, EventKind::Close), 0);

    // 11:30:01–12:59:59 为早盘与午盘之间的休息时段
    builder_.on_tick(make_tick(11 * 3600 + 30 * 60 + 1, 101.0, 100, 10100.0, 8));
    builder_.on_tick(make_tick(12 * 3600, 102.0, 200, 20400.0, 9));
    builder_.on_tick(make_tick(12 * 3600 + 59 * 60 + 59, 103.0, 300, 30900.0, 10));

    const BarBuilderStats stats = builder_.stats();
    EXPECT_EQ(stats.out_of_session, 3U);
    EXPECT_EQ(stats.out_of_order, 0U);  // 丢掉休息时段 tick 不误增其他计数
    EXPECT_EQ(stats.bad_volume, 0U);
    EXPECT_EQ(count_events(sink_, EventKind::Open), 1);  // 被丢弃的 tick 不产生回调
    EXPECT_EQ(count_events(sink_, EventKind::Update), 1);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 0);
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.begin_sec, 57600);  // 10:00 桶
    EXPECT_EQ(bar.last_tick_sec, 57600);
    EXPECT_DOUBLE_EQ(bar.close, 100.0);
    EXPECT_EQ(bar.cum_volume, 10);

    // 13:00 恢复交易：被丢弃 tick 不进入累计基准（12 - 10 = 2）
    builder_.on_tick(make_tick(13 * 3600, 104.0, 12, 1248.0, 11));
    EXPECT_EQ(builder_.stats().out_of_session, 3U);
    EXPECT_EQ(count_events(sink_, EventKind::Open), 2);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 1);
    const Bar* last_close = last_event_bar(sink_, EventKind::Close);
    ASSERT_NE(last_close, nullptr);
    EXPECT_EQ(last_close->begin_sec, 57600);  // 10:00 桶
    EXPECT_TRUE(last_close->is_final);
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_13_00);
    EXPECT_EQ(builder_.current_bar().volume, 2);
    EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, 248.0);
}

TEST_F(IntradayBarSessionTest, NonTradingTimeTicksAreDroppedBeforeFirstBar) {
    // 夜盘前、夜盘后、早盘前、早盘开始前 1 秒：均不在任何交易小节内
    builder_.on_tick(make_tick(20 * 3600 + 59 * 60 + 59, 100.0, 10, 1000.0, 7));
    builder_.on_tick(make_tick(2 * 3600 + 30 * 60 + 1, 100.0, 10, 1000.0, 7));
    builder_.on_tick(make_tick(8 * 3600, 100.0, 10, 1000.0, 7));
    builder_.on_tick(make_tick(9 * 3600 + 29 * 60 + 59, 100.0, 10, 1000.0, 7));

    EXPECT_FALSE(builder_.has_current_bar());
    EXPECT_EQ(builder_.stats().out_of_session, 4U);
    EXPECT_TRUE(sink_.events.empty());

    builder_.on_tick(make_tick(9 * 3600 + 30 * 60, 100.5, 11, 1105.5, 7));  // 09:30:00 开市
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_30);
    EXPECT_EQ(builder_.current_bar().volume, 11);  // 首笔以 0 为基准
}

TEST_F(IntradayBarSessionTest, AfternoonSessionEndTickBelongsToPreviousBar) {
    builder_.on_tick(make_tick(14 * 3600 + 59 * 60 + 30, 100.0, 10, 1000.0, 7));  // 14:59:30
    builder_.on_tick(make_tick(15 * 3600, 101.0, 12, 1212.0, 8));  // 15:00:00 收盘时刻

    EXPECT_EQ(builder_.stats().out_of_session, 0U);
    EXPECT_EQ(count_events(sink_, EventKind::Open), 1);  // 不产生下一桶，也不产生下一 session 首根
    EXPECT_EQ(count_events(sink_, EventKind::Update), 2);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 0);
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.begin_sec, 75540);      // 14:59 桶
    EXPECT_EQ(bar.last_tick_sec, 75600);  // 15:00:00，last_tick_sec 记录真实 tsec
    EXPECT_DOUBLE_EQ(bar.close, 101.0);
    EXPECT_EQ(bar.volume, 12);  // 首笔 tick 的 bar 量额基准为 0
    EXPECT_FALSE(bar.is_final);

    builder_.on_tick(make_tick(15 * 3600 + 1, 102.0, 13, 1326.0, 9));  // 15:00:01 已收盘
    EXPECT_EQ(builder_.stats().out_of_session, 1U);
    EXPECT_EQ(count_events(sink_, EventKind::Update), 2);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, 75600);
}

TEST_F(IntradayBarSessionTest, SessionEndTickUsesPreviousBucketWhenNoPriorTick) {
    builder_.on_tick(make_tick(15 * 3600, 101.0, 10, 1010.0, 7));  // 15:00:00 为该日第一笔

    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, 75540);      // 归 14:59 桶而非 15:00 桶
    EXPECT_EQ(builder_.current_bar().last_tick_sec, 75600);  // 真实 tsec
    EXPECT_EQ(count_events(sink_, EventKind::Open), 1);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 0);
}

TEST_F(IntradayBarSessionTest, NightSessionEndTickBelongsToPreviousBar) {
    builder_.on_tick(make_tick(2 * 3600 + 29 * 60 + 30, 100.0, 10, 1000.0, 7));  // 02:29:30
    builder_.on_tick(make_tick(2 * 3600 + 30 * 60, 101.0, 12, 1212.0, 8));  // 02:30:00 夜盘收盘

    EXPECT_EQ(builder_.stats().out_of_session, 0U);
    EXPECT_EQ(count_events(sink_, EventKind::Open), 1);  // 不产生下一 session（09:30 早盘）首根
    EXPECT_EQ(count_events(sink_, EventKind::Close), 0);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, 30540);      // 02:29 桶
    EXPECT_EQ(builder_.current_bar().last_tick_sec, 30600);  // 02:30:00，真实 tsec
}
