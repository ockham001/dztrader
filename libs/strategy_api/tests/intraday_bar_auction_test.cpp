#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <cstddef>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

namespace {

/// @brief 开盘竞价窗口内的墙钟秒（08:59:10 / 08:59:20 / 08:59:30）
constexpr DzTime CLOCK_08_59_10 = 8 * 3600 + 59 * 60 + 10;
constexpr DzTime CLOCK_08_59_20 = 8 * 3600 + 59 * 60 + 20;
constexpr DzTime CLOCK_08_59_30 = 8 * 3600 + 59 * 60 + 30;
/// @brief 场内时刻：09:00:20（09:00 桶内）与 09:01:00（下一分钟桶）
constexpr DzTime CLOCK_09_00_20 = 9 * 3600 + 20;
constexpr DzTime CLOCK_09_01_00 = 9 * 3600 + 60;
/// @brief 商品多小节表的竞价与场内时刻
constexpr DzTime CLOCK_20_59_40 = 20 * 3600 + 59 * 60 + 40;  // 夜盘开盘竞价窗口内
constexpr DzTime CLOCK_09_29_00 = 9 * 3600 + 29 * 60;        // 早盘开盘竞价窗口内
constexpr DzTime CLOCK_09_30_00 = 9 * 3600 + 30 * 60;        // 早盘开市
constexpr DzTime CLOCK_14_59_00 = 14 * 3600 + 59 * 60;       // 午盘收盘竞价窗口内
constexpr DzTime CLOCK_15_00_00 = 15 * 3600;                 // 午盘小节末端/收盘竞价端点
/// @brief 窗口优先与窗口外判定用时刻
constexpr DzTime CLOCK_10_00_00 = 10 * 3600;
constexpr DzTime CLOCK_11_10_00 = 11 * 3600 + 10 * 60;
constexpr DzTime CLOCK_12_00_00 = 12 * 3600;
constexpr DzTime CLOCK_13_00_00 = 13 * 3600;

}  // namespace

/// @brief 单小节日盘表的集合竞价：Open 窗口 tick 归后一小节首桶、窗口内零回调、
///        竞价-only bar 收尾补发 open + close、竞价 bar 首次暴露补发 open、坏 tick 守卫
///
/// 断言对象一律是可观察行为（sink 事件序列 / current_bar() / stats()），不读内部状态；
/// sink 快照是判定「何时暴露给策略」的唯一依据。
class IntradayBarAuctionTest : public ::testing::Test {
protected:
    TradingSessionTable table_ = day_auction_session_table();
    RecordingSink sink_;
    IntradayBarBuilder<RecordingSink> builder_{table_, 60, sink_};
};

/// @brief 开盘竞价窗口内的 tick 归后一小节首桶，last_tick_sec 保留竞价真实时刻
TEST_F(IntradayBarAuctionTest, OpenAuctionTicksMapToFirstBucketOfTargetSession) {
    builder_.on_tick(make_tick(CLOCK_08_59_20, 100.5, 10, 1005.0, 7));

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY);
    EXPECT_EQ(bar.begin_sec, TSEC_09_00);  // 归后一小节首桶
    EXPECT_EQ(bar.period_sec, 60);
    EXPECT_EQ(bar.last_tick_sec, TradingTime::from_clock(CLOCK_08_59_20));  // 竞价时刻早于桶起点
    EXPECT_FALSE(bar.is_final);
    // 竞价窗口内的 tick 是有效 tick：不进入 out_of_session 计数
    EXPECT_EQ(builder_.stats().out_of_session, 0U);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
    EXPECT_EQ(builder_.stats().bad_volume, 0U);
}

/// @brief 同窗口多笔竞价 tick 逐个覆盖 O=H=L=C，量额累计差分、OI 取最后一笔；窗口内零回调
TEST_F(IntradayBarAuctionTest, OpenAuctionTicksOverrideOhlcAndEmitNoSinkEvents) {
    builder_.on_tick(make_tick(CLOCK_08_59_10, 100.5, 10, 1005.0, 7));
    builder_.on_tick(make_tick(CLOCK_08_59_20, 103.0, 15, 1545.0, 9));
    builder_.on_tick(make_tick(CLOCK_08_59_30, 99.0, 18, 1782.0, 8));

    // 竞价窗口内零 sink 事件（策略不可能在竞价时段下单）
    EXPECT_TRUE(sink_.events.empty());

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_DOUBLE_EQ(bar.open, 99.0);  // 逐 tick 覆盖，不是首笔竞价价格
    EXPECT_DOUBLE_EQ(bar.high, 99.0);
    EXPECT_DOUBLE_EQ(bar.low, 99.0);
    EXPECT_DOUBLE_EQ(bar.close, 99.0);
    EXPECT_EQ(bar.volume, 18);
    EXPECT_DOUBLE_EQ(bar.turnover, 1782.0);
    EXPECT_EQ(bar.open_interest, 8);  // 持仓量取最后一笔
    EXPECT_EQ(bar.cum_volume, 18);
    EXPECT_DOUBLE_EQ(bar.cum_turnover, 1782.0);
    EXPECT_EQ(bar.last_tick_sec, TradingTime::from_clock(CLOCK_08_59_30));
}

/// @brief 竞价-only bar 收尾：恰好一次 open（is_final=false）+ 一次 close（is_final=true）
TEST_F(IntradayBarAuctionTest, AuctionOnlyBarBackfillsOpenThenCloseWhenFinalized) {
    builder_.on_tick(make_tick(CLOCK_08_59_10, 100.5, 10, 1005.0, 7));
    builder_.on_tick(make_tick(CLOCK_08_59_30, 101.0, 12, 1212.0, 8));
    EXPECT_TRUE(sink_.events.empty());  // 窗口内零回调

    // 09:01:00 的场内 tick 跨过 09:00 桶：竞价 bar 收尾
    builder_.on_tick(make_tick(CLOCK_09_01_00, 102.0, 20, 2040.0, 9));

    ASSERT_EQ(sink_.events.size(), 4U);
    // 竞价-only bar：先补发 open（is_final=false），再 close（is_final=true）
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_FALSE(sink_.events[0].bar.is_final);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Close);
    EXPECT_TRUE(sink_.events[1].bar.is_final);
    for (std::size_t i = 0; i < 2; ++i) {
        const Bar& auction_bar = sink_.events[i].bar;
        EXPECT_EQ(auction_bar.begin_sec, TSEC_09_00);
        EXPECT_EQ(auction_bar.last_tick_sec, TradingTime::from_clock(CLOCK_08_59_30));
        EXPECT_DOUBLE_EQ(auction_bar.open, 101.0);
        EXPECT_DOUBLE_EQ(auction_bar.high, 101.0);
        EXPECT_DOUBLE_EQ(auction_bar.low, 101.0);
        EXPECT_DOUBLE_EQ(auction_bar.close, 101.0);
        EXPECT_EQ(auction_bar.volume, 12);
        EXPECT_EQ(auction_bar.open_interest, 8);
    }
    // 随后正常开新桶：open + update
    EXPECT_EQ(sink_.events[2].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[3].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[3].bar.begin_sec, TSEC_09_00 + 60);
}

/// @brief 竞价 bar 被同桶场内 tick 首次暴露：先补发 open（携带竞价状态）再 update
TEST_F(IntradayBarAuctionTest, AuctionBarExposedBySessionTickEmitsDeferredOpenBeforeUpdate) {
    builder_.on_tick(make_tick(CLOCK_08_59_10, 100.0, 10, 1000.0, 7));
    builder_.on_tick(make_tick(CLOCK_08_59_20, 99.0, 13, 1287.0, 8));
    EXPECT_TRUE(sink_.events.empty());

    builder_.on_tick(make_tick(CLOCK_09_00_20, 102.0, 20, 2040.0, 9));

    ASSERT_EQ(sink_.events.size(), 2U);
    // open 快照是竞价状态（本笔场内 tick 尚未并入）
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_FALSE(sink_.events[0].bar.is_final);
    EXPECT_EQ(sink_.events[0].bar.begin_sec, TSEC_09_00);
    EXPECT_DOUBLE_EQ(sink_.events[0].bar.open, 99.0);
    EXPECT_DOUBLE_EQ(sink_.events[0].bar.high, 99.0);
    EXPECT_DOUBLE_EQ(sink_.events[0].bar.low, 99.0);
    EXPECT_DOUBLE_EQ(sink_.events[0].bar.close, 99.0);
    EXPECT_EQ(sink_.events[0].bar.volume, 13);
    // update 快照并入本笔场内 tick
    EXPECT_EQ(sink_.events[1].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[1].bar.begin_sec, TSEC_09_00);
    EXPECT_DOUBLE_EQ(sink_.events[1].bar.open, 99.0);  // 竞价开价不随后续 tick 变化
    EXPECT_DOUBLE_EQ(sink_.events[1].bar.high, 102.0);
    EXPECT_DOUBLE_EQ(sink_.events[1].bar.close, 102.0);
    EXPECT_EQ(sink_.events[1].bar.volume, 20);
    EXPECT_EQ(sink_.events[1].bar.last_tick_sec, TSEC_09_00 + 20);
}

/// @brief 竞价 tick 与场内 tick 受同一套坏数据守卫约束：丢弃且不污染 bar / 不回调
TEST_F(IntradayBarAuctionTest, AuctionTicksAreSubjectToDataGuards) {
    builder_.on_tick(make_tick(CLOCK_08_59_20, 100.0, 10, 1000.0, 7));
    // 累计量倒退：bad_volume
    builder_.on_tick(make_tick(CLOCK_08_59_30, 102.0, 5, 510.0, 8));
    // 交易日内秒倒退：out_of_order
    builder_.on_tick(make_tick(CLOCK_08_59_10, 103.0, 12, 1236.0, 9));
    // 非法价格：跳过且不计数
    builder_.on_tick(
        make_tick(CLOCK_08_59_20 + 20, std::numeric_limits<double>::quiet_NaN(), 12, 1236.0, 9));

    EXPECT_TRUE(sink_.events.empty());
    const BarBuilderStats stats = builder_.stats();
    EXPECT_EQ(stats.out_of_session, 0U);
    EXPECT_EQ(stats.out_of_order, 1U);
    EXPECT_EQ(stats.bad_volume, 1U);
    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_DOUBLE_EQ(bar.close, 100.0);
    EXPECT_EQ(bar.volume, 10);
    EXPECT_EQ(bar.last_tick_sec, TradingTime::from_clock(CLOCK_08_59_20));
}

/// @brief 竞价窗口端点含：窗口起始秒接受、前一秒丢弃；窗口结束秒仍归竞价
TEST_F(IntradayBarAuctionTest, AuctionWindowBoundsAreInclusive) {
    constexpr DzTime CLOCK_08_58_59 = 8 * 3600 + 58 * 60 + 59;  // 窗口前一秒
    constexpr DzTime CLOCK_08_59_00 = 8 * 3600 + 59 * 60;       // 窗口起始秒
    constexpr DzTime CLOCK_08_59_59 = 8 * 3600 + 59 * 60 + 59;  // 窗口结束秒

    builder_.on_tick(make_tick(CLOCK_08_58_59, 100.0, 10, 1000.0, 7));  // 不在窗口/小节内
    EXPECT_FALSE(builder_.has_current_bar());
    EXPECT_EQ(builder_.stats().out_of_session, 1U);

    builder_.on_tick(make_tick(CLOCK_08_59_00, 101.0, 12, 1212.0, 8));  // 窗口起始秒
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_00);

    builder_.on_tick(make_tick(CLOCK_08_59_59, 102.0, 15, 1530.0, 9));  // 窗口结束秒
    EXPECT_TRUE(sink_.events.empty());
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00 - 1);
    EXPECT_DOUBLE_EQ(builder_.current_bar().close, 102.0);
    EXPECT_EQ(builder_.stats().out_of_session, 1U);  // 窗口内 tick 不计入丢弃
}

/// @brief 跨交易日：新交易日夜盘竞价静默建立新日首根，旧日末根延迟补发 close
TEST_F(IntradayBarAuctionTest, NextTradingDayNightAuctionIsSilentAndBackfillsPreviousDayBar) {
    constexpr DzDate NEXT_TRADING_DAY = TRADING_DAY + 1;

    builder_.on_tick(make_tick(CLOCK_15_00_00, 100.0, 100, 10000.0, 7, TRADING_DAY));
    // 新交易日 D+1 的夜盘开盘竞价（20:59:20）：静默换日，不回调
    builder_.on_tick(make_tick(CLOCK_08_59_20, 101.0, 130, 13130.0, 8, NEXT_TRADING_DAY));

    ASSERT_EQ(sink_.events.size(), 2U);  // 只有 D 日末根的 open + update
    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& auction_bar = builder_.current_bar();
    EXPECT_EQ(auction_bar.trading_day, NEXT_TRADING_DAY);
    EXPECT_EQ(auction_bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(auction_bar.last_tick_sec, TradingTime::from_clock(CLOCK_08_59_20));
    EXPECT_EQ(auction_bar.volume, 130);  // 新日累计基准归零
    EXPECT_EQ(builder_.stats().out_of_order, 0U);

    // 新日首笔场内 tick：先补发旧日末根 close，再暴露新日竞价 bar
    builder_.on_tick(make_tick(CLOCK_09_00_20, 102.0, 150, 15300.0, 9, NEXT_TRADING_DAY));
    ASSERT_EQ(sink_.events.size(), 5U);
    const Bar& previous_day_close = sink_.events[2].bar;
    EXPECT_EQ(sink_.events[2].kind, EventKind::Close);
    EXPECT_TRUE(previous_day_close.is_final);
    EXPECT_EQ(previous_day_close.trading_day, TRADING_DAY);
    EXPECT_EQ(previous_day_close.begin_sec, TSEC_15_00 - 60);
    EXPECT_EQ(sink_.events[3].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[3].bar.trading_day, NEXT_TRADING_DAY);
    EXPECT_EQ(sink_.events[3].bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(sink_.events[4].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[4].bar.volume, 150);
}

/// @brief 商品多小节 + 多个长度不同的 Open 窗口：各归其目标小节首桶；两个窗口内都零事件
class IntradayBarCommodityAuctionTest : public ::testing::Test {
protected:
    TradingSessionTable table_ = commodity_auction_session_table();
    RecordingSink sink_;
    IntradayBarBuilder<RecordingSink> builder_{table_, 60, sink_};
};

TEST_F(IntradayBarCommodityAuctionTest, MultipleOpenAuctionWindowsMapToTheirSessionFirstBuckets) {
    // 夜盘开盘竞价（30 秒窗口）：归 21:00 首桶
    builder_.on_tick(make_tick(CLOCK_20_59_40, 100.0, 10, 1000.0, 7));
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_21_00);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_21_00 - 20);

    // 早盘开盘竞价（90 秒窗口）：静默换根归 09:30 首桶
    builder_.on_tick(make_tick(CLOCK_09_29_00, 101.0, 12, 1212.0, 8));
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_30);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_30 - 60);
    EXPECT_TRUE(sink_.events.empty());  // 两个竞价窗口内都零事件
    EXPECT_EQ(builder_.stats().out_of_session, 0U);

    // 早盘开市：先补发夜盘竞价-only bar 的 open + close，再暴露早盘竞价 bar 的 open + update
    builder_.on_tick(make_tick(CLOCK_09_30_00, 102.0, 20, 2040.0, 9));
    ASSERT_EQ(sink_.events.size(), 4U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_FALSE(sink_.events[0].bar.is_final);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Close);
    EXPECT_TRUE(sink_.events[1].bar.is_final);
    for (std::size_t i = 0; i < 2; ++i) {
        const Bar& night_bar = sink_.events[i].bar;
        EXPECT_EQ(night_bar.begin_sec, TSEC_21_00);
        EXPECT_DOUBLE_EQ(night_bar.open, 100.0);
        EXPECT_DOUBLE_EQ(night_bar.close, 100.0);
        EXPECT_EQ(night_bar.volume, 10);
    }
    EXPECT_EQ(sink_.events[2].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[2].bar.begin_sec, TSEC_09_30);
    EXPECT_DOUBLE_EQ(sink_.events[2].bar.open, 101.0);
    EXPECT_EQ(sink_.events[3].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[3].bar.begin_sec, TSEC_09_30);
    // 早盘竞价 bar 量额基准 = 上一笔已接受 tick（夜盘竞价 tick 累计 10）
    EXPECT_EQ(sink_.events[3].bar.volume, 20 - 10);
    EXPECT_EQ(builder_.stats().out_of_session, 0U);
}

/// @brief Close 窗口按普通 session tick 处理：正常回调，端点 tick 归前一根
TEST_F(IntradayBarCommodityAuctionTest, CloseAuctionWindowTicksAreHandledAsNormalSessionTicks) {
    builder_.on_tick(make_tick(CLOCK_14_59_00, 100.0, 10, 1000.0, 7));  // 收盘竞价窗口内
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_15_00 - 60);
    EXPECT_EQ(count_events(sink_, EventKind::Open), 1);  // 不静默：正常暴露

    builder_.on_tick(make_tick(CLOCK_15_00_00, 101.0, 12, 1212.0, 8));  // 窗口端点 = 小节末端
    EXPECT_EQ(count_events(sink_, EventKind::Close), 0);                // 末端 tick 不换根
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_15_00 - 60);       // 归前一根
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_15_00);
    EXPECT_DOUBLE_EQ(builder_.current_bar().close, 101.0);
    EXPECT_EQ(builder_.stats().out_of_session, 0U);
}

/// @brief 窗口命中优先于 session 判定：重叠窗口内的 tick 归窗口目标小节首桶
TEST(IntradayBarAuctionPriorityTest, WindowMatchTakesPriorityOverSessionMatch) {
    const TradingSessionTable table = overlapping_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    // 11:10 同时落在早盘小节（09:00–11:30）与午盘开盘竞价窗口（11:00–11:29）内
    builder.on_tick(make_tick(CLOCK_11_10_00, 100.0, 10, 1000.0, 7));

    EXPECT_TRUE(sink.events.empty());  // 竞价窗口内零回调
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().begin_sec, TSEC_13_00);  // 窗口优先：归午盘首桶而非 11:10
    EXPECT_EQ(builder.current_bar().last_tick_sec, TradingTime::from_clock(CLOCK_11_10_00));
    EXPECT_EQ(builder.stats().out_of_session, 0U);
}

/// @brief 窗口外 tick 不误入竞价逻辑：小节内按普通 tick、非交易时段丢弃
TEST(IntradayBarAuctionPriorityTest, TicksOutsideWindowFollowNormalSessionOrDropPath) {
    const TradingSessionTable table = overlapping_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    // 10:00：outside 竞价窗口、在早盘小节内 → 普通 bar + 正常回调
    builder.on_tick(make_tick(CLOCK_10_00_00, 100.0, 10, 1000.0, 7));
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink.events[1].kind, EventKind::Update);
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().begin_sec, TSEC_10_00);

    // 11:10：窗口内 → 静默换根归午盘首桶（旧 bar 的 close 延迟到离开窗口后补发）
    builder.on_tick(make_tick(CLOCK_11_10_00, 101.0, 12, 1212.0, 8));
    EXPECT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(builder.current_bar().begin_sec, TSEC_13_00);

    // 12:00：既不在窗口也不在小节内 → 丢弃计数，且被丢弃的 tick 不触发补发
    builder.on_tick(make_tick(CLOCK_12_00_00, 102.0, 14, 1428.0, 9));
    EXPECT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(builder.stats().out_of_session, 1U);

    // 13:00 开市：补发 10:00 bar 的 close，再暴露 13:00 竞价 bar
    builder.on_tick(make_tick(CLOCK_13_00_00, 103.0, 20, 2060.0, 10));
    ASSERT_EQ(sink.events.size(), 5U);
    EXPECT_EQ(sink.events[2].kind, EventKind::Close);
    EXPECT_TRUE(sink.events[2].bar.is_final);
    EXPECT_EQ(sink.events[2].bar.begin_sec, TSEC_10_00);
    EXPECT_EQ(sink.events[3].kind, EventKind::Open);
    EXPECT_EQ(sink.events[3].bar.begin_sec, TSEC_13_00);
    EXPECT_EQ(sink.events[4].kind, EventKind::Update);
    EXPECT_EQ(sink.events[4].bar.begin_sec, TSEC_13_00);
    EXPECT_EQ(builder.stats().out_of_session, 1U);
}

/// @brief 目标桶 = floor(目标小节开始 / 周期) * 周期：1 小时周期下 09:30 归 09:00 桶
TEST(IntradayBarAuctionGridTest, OpenAuctionTargetBucketFloorsSessionBeginToPeriodGrid) {
    const TradingSessionTable table = commodity_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 3600, sink);

    builder.on_tick(make_tick(CLOCK_09_29_00, 100.0, 10, 1000.0, 7));  // 早盘开盘竞价
    ASSERT_TRUE(builder.has_current_bar());
    // floor((09:30 = 55800) / 3600) * 3600 = 54000（09:00 桶）
    EXPECT_EQ(builder.current_bar().begin_sec, TSEC_09_00);
    EXPECT_EQ(builder.current_bar().last_tick_sec, TSEC_09_30 - 60);
    EXPECT_TRUE(sink.events.empty());

    builder.on_tick(make_tick(CLOCK_09_30_00, 101.0, 12, 1212.0, 8));  // 开市进场内同桶
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink.events[0].bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(sink.events[1].kind, EventKind::Update);
    EXPECT_EQ(sink.events[1].bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(builder.stats().out_of_session, 0U);
}

/// @brief 粗周期跨小节拼根：目标桶已含场内 tick 时，竞价 tick 静默并入、不覆盖已公开 O/H/L
TEST(IntradayBarAuctionCollisionTest, AuctionTickMergesIntoExistingBucketWithoutOverwritingOhlc) {
    const TradingSessionTable table = day_break_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 3600, sink);

    builder.on_tick(make_tick(10 * 3600 + 5 * 60, 100.0, 10, 1000.0, 7));  // 10:05 场内
    ASSERT_TRUE(builder.has_current_bar());
    ASSERT_EQ(builder.current_bar().begin_sec, TSEC_10_00);
    ASSERT_EQ(sink.events.size(), 2U);

    // 10:16:10 落在 10:30 小节的开盘竞价窗口内；1 小时目标桶 = 10:00，与当前桶相同
    builder.on_tick(make_tick(10 * 3600 + 16 * 60 + 10, 105.0, 12, 1212.0, 8));
    EXPECT_EQ(sink.events.size(), 2U);  // 窗口内零回调
    ASSERT_TRUE(builder.has_current_bar());
    const Bar& bar = builder.current_bar();
    EXPECT_DOUBLE_EQ(bar.open, 100.0);  // 已公开开盘价不被竞价 tick 覆盖
    EXPECT_DOUBLE_EQ(bar.high, 105.0);  // 竞价 tick 按普通 tick 参与高低点
    EXPECT_DOUBLE_EQ(bar.low, 100.0);
    EXPECT_DOUBLE_EQ(bar.close, 105.0);
    EXPECT_EQ(bar.volume, 12);
    EXPECT_EQ(bar.last_tick_sec, TradingTime::from_clock(10 * 3600 + 16 * 60 + 10));

    // 10:30 恢复场内：同桶只 update（bar 早已暴露过，不补发 open）
    builder.on_tick(make_tick(10 * 3600 + 30 * 60, 99.0, 15, 1500.0, 9));
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[2].kind, EventKind::Update);
    EXPECT_DOUBLE_EQ(sink.events[2].bar.open, 100.0);
    EXPECT_DOUBLE_EQ(sink.events[2].bar.high, 105.0);
    EXPECT_DOUBLE_EQ(sink.events[2].bar.low, 99.0);
}

/// @brief 病态跨日仅竞价流：待补发队列溢出时丢弃最旧 bar，任何窗口内仍零回调
TEST(IntradayBarAuctionOverflowTest, PendingQueueOverflowDropsOldestBarWithoutWindowCallbacks) {
    const TradingSessionTable table = day_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    // 连续 9 个交易日只喂开盘竞价 tick：8 根待补发填满队列，窗口内零回调
    for (DzDate day = TRADING_DAY; day < TRADING_DAY + 9; ++day) {
        builder.on_tick(make_tick(CLOCK_08_59_20, 100.0, 10, 1000.0, 7, day));
    }
    EXPECT_TRUE(sink.events.empty());
    EXPECT_EQ(builder.stats().out_of_order, 0U);

    // 第 10 个交易日竞价 tick：溢出丢弃最旧（D 日）bar，仍不回调
    builder.on_tick(make_tick(CLOCK_08_59_20, 100.0, 10, 1000.0, 7, TRADING_DAY + 9));
    EXPECT_TRUE(sink.events.empty());

    // 第 10 个交易日开市：补发保留的 8 根（D+1..D+8），再暴露当前竞价 bar
    builder.on_tick(make_tick(CLOCK_09_00_20, 101.0, 12, 1212.0, 8, TRADING_DAY + 9));
    EXPECT_EQ(count_events(sink, EventKind::Open), 9);
    EXPECT_EQ(count_events(sink, EventKind::Close), 8);
    ASSERT_GE(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink.events[0].bar.trading_day, TRADING_DAY + 1);  // 最旧的 D 日已被丢弃
    EXPECT_EQ(sink.events[1].kind, EventKind::Close);
    EXPECT_EQ(sink.events[1].bar.trading_day, TRADING_DAY + 1);
}
