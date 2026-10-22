#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

class IntradayBarBuilderTest : public ::testing::Test {
protected:
    TradingSessionTable table_ = day_session_table();
    NullBarSink sink_;
    IntradayBarBuilder<> builder_{table_, 60, sink_};
};

TEST_F(IntradayBarBuilderTest, NoCurrentBarBeforeFirstTick) {
    EXPECT_FALSE(builder_.has_current_bar());
}

TEST_F(IntradayBarBuilderTest, FirstTickStartsFormingBar) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY);
    EXPECT_EQ(bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(bar.period_sec, 60);
    EXPECT_EQ(bar.last_tick_sec, TSEC_09_00);
    EXPECT_DOUBLE_EQ(bar.open, 100.5);
    EXPECT_DOUBLE_EQ(bar.high, 100.5);
    EXPECT_DOUBLE_EQ(bar.low, 100.5);
    EXPECT_DOUBLE_EQ(bar.close, 100.5);
    EXPECT_FALSE(bar.is_final);
    EXPECT_EQ(bar.volume, 10);
    EXPECT_DOUBLE_EQ(bar.turnover, 1005.0);
    EXPECT_EQ(bar.cum_volume, 10);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 1005.0);
    EXPECT_EQ(bar.open_interest, 7);
}

TEST_F(IntradayBarBuilderTest, SameBucketTicksUpdateHighLowCloseAndLastTickSec) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));
    builder_.on_tick(make_tick(9 * 3600 + 10, 103.0, 15, 1545.0, 9));
    builder_.on_tick(make_tick(9 * 3600 + 20, 99.0, 18, 1782.0, 8));
    builder_.on_tick(make_tick(9 * 3600 + 59, 101.0, 20, 2020.0, 10));

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_DOUBLE_EQ(bar.open, 100.5);  // 首笔 tick 的开盘价不随后续 tick 变化
    EXPECT_DOUBLE_EQ(bar.high, 103.0);
    EXPECT_DOUBLE_EQ(bar.low, 99.0);
    EXPECT_DOUBLE_EQ(bar.close, 101.0);
    EXPECT_EQ(bar.begin_sec, TSEC_09_00);  // 同桶不改变 bar 边界
    EXPECT_EQ(bar.last_tick_sec, TSEC_09_00 + 59);
    EXPECT_FALSE(bar.is_final);
}

TEST_F(IntradayBarBuilderTest, SameBucketTicksAccumulateVolumeTurnoverAndOpenInterest) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));
    builder_.on_tick(make_tick(9 * 3600 + 30, 102.0, 25, 2550.0, 11));

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    // 第一根 bar 的起始累计基准为 0：bar 量额 = 最新累计量额
    EXPECT_EQ(bar.volume, 25);
    EXPECT_DOUBLE_EQ(bar.turnover, 2550.0);
    EXPECT_EQ(bar.cum_volume, 25);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 2550.0);
    EXPECT_EQ(bar.open_interest, 11);  // 持仓量取最后一笔 tick
}

TEST_F(IntradayBarBuilderTest, TradingDayComesFromTick) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 1, 100.5, 1, TRADING_DAY + 1));

    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY + 1);
}

TEST(IntradayBarBuilderGridTest, BeginSecIsFlooredToPeriodGrid) {
    // 每个用例用独立 builder：本用例只覆盖单笔 tick 的网格取整（桶切换生命周期见
    // intraday_bar_lifecycle_test.cpp）。
    struct GridCase {
        int32_t clock;
        int32_t period_sec;
        int32_t begin_sec;
    };
    const GridCase cases[] = {
        {9 * 3600, 60, TSEC_09_00},
        {9 * 3600 + 59, 60, TSEC_09_00},
        {9 * 3600, 300, TSEC_09_00},
        {9 * 3600 + 150, 300, TSEC_09_00},  // 09:02:30 → 09:00 桶
        {9 * 3600 + 299, 300, TSEC_09_00},
        {9 * 3600 + 300, 300, TSEC_09_00 + 300},  // 内部边界 tick 归新桶
    };
    const TradingSessionTable table = day_session_table();
    for (const GridCase& grid_case : cases) {
        SCOPED_TRACE(::testing::Message()
                     << "clock=" << grid_case.clock << " period_sec=" << grid_case.period_sec);
        NullBarSink sink;
        IntradayBarBuilder<> builder(table, grid_case.period_sec, sink);
        builder.on_tick(make_tick(grid_case.clock, 100.0, 1, 100.0, 1));

        EXPECT_TRUE(builder.has_current_bar());
        if (!builder.has_current_bar()) {
            continue;
        }
        const Bar& bar = builder.current_bar();
        EXPECT_EQ(bar.begin_sec, grid_case.begin_sec);
        EXPECT_EQ(bar.period_sec, grid_case.period_sec);
        EXPECT_EQ(bar.begin_clock(), grid_case.clock - grid_case.clock % grid_case.period_sec);
        EXPECT_EQ(bar.last_tick_sec, TradingTime::from_clock(grid_case.clock));
    }
}

TEST(IntradayBarBuilderConstructorTest, RejectsNonPositivePeriodSec) {
    const TradingSessionTable table = day_session_table();
    NullBarSink sink;
    EXPECT_THROW(IntradayBarBuilder<>(table, 0, sink), std::invalid_argument);
    EXPECT_THROW(IntradayBarBuilder<>(table, -60, sink), std::invalid_argument);
}

TEST(IntradayBarBuilderConstructorTest, RejectsInvalidSessionTable) {
    NullBarSink sink;
    EXPECT_THROW(IntradayBarBuilder<>(TradingSessionTable{}, 60, sink), std::invalid_argument);

    TradingSessionTable out_of_order{};
    out_of_order.session_count = 2;
    out_of_order.sessions[0] = TradingSession{TSEC_09_00, TSEC_15_00};
    out_of_order.sessions[1] = TradingSession{TSEC_21_00, TSEC_02_30};
    EXPECT_THROW(IntradayBarBuilder<>(out_of_order, 60, sink), std::invalid_argument);
}

TEST(IntradayBarBuilderConstructorTest, AcceptsValidMultiSessionTableWithAuctions) {
    TradingSessionTable table{};
    table.session_count = 2;
    table.sessions[0] = TradingSession{TSEC_21_00, TSEC_02_30};
    table.sessions[1] = TradingSession{TSEC_09_00, TSEC_15_00};
    table.auction_count = 2;
    table.auctions[0] = AuctionWindow{AuctionKind::Open, 0, TSEC_21_00 - 60, TSEC_21_00 - 1};
    table.auctions[1] = AuctionWindow{AuctionKind::Open, 1, TSEC_09_00 - 60, TSEC_09_00 - 1};
    ASSERT_TRUE(table.valid());

    NullBarSink sink;
    IntradayBarBuilder<> builder(table, 60, sink);
    EXPECT_FALSE(builder.has_current_bar());

    const int32_t clock = 9 * 3600 + 1800;  // 09:30
    builder.on_tick(make_tick(clock, 100.0, 1, 100.0, 1));
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().begin_sec, TradingTime::from_clock(clock));
}
