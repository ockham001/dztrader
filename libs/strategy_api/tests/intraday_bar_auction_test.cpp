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

/// @brief 交易日内秒（tick 的 last_tick_sec 口径）
constexpr int32_t tsec_of(DzTime clock) { return TradingTime::from_clock(clock); }

/// @brief 断言一次事件的类型、竞价相位与 bar 身份（桶起点 / 最后 tick 时刻）
void expect_event(const BarEvent& event,
                  EventKind kind,
                  bool is_auction,
                  int32_t begin_sec,
                  int32_t last_tick_sec) {
    EXPECT_EQ(event.kind, kind);
    EXPECT_EQ(event.is_auction, is_auction);
    EXPECT_EQ(event.bar.begin_sec, begin_sec);
    EXPECT_EQ(event.bar.last_tick_sec, last_tick_sec);
}

/// @brief 断言竞价覆盖口径：O=H=L=C=price
void expect_auction_ohlc(const Bar& bar, double price) {
    EXPECT_DOUBLE_EQ(bar.open, price);
    EXPECT_DOUBLE_EQ(bar.high, price);
    EXPECT_DOUBLE_EQ(bar.low, price);
    EXPECT_DOUBLE_EQ(bar.close, price);
}

}  // namespace

/// @brief 单小节日盘表的集合竞价：Open 窗口 tick 归后一小节首桶并正常发事件
///        （首笔 open+update、同桶后续 update、离场首笔 update、跨桶先 close）、坏 tick 守卫
///
/// 断言对象一律是可观察行为（sink 事件序列 / current_bar() / stats()），不读内部状态；
/// 事件序列同时断言 is_auction（触发回调的 tick 是否处于 Open 窗口）。
class IntradayBarAuctionTest : public ::testing::Test {
protected:
    TradingSessionTable table_ = day_auction_session_table();
    RecordingSink sink_;
    IntradayBarBuilder<RecordingSink> builder_{table_, 60, sink_};
};

/// @brief 窗口首笔（实例首根，无上一根）：恰好 open(is_auction=true) → update(true)
TEST_F(IntradayBarAuctionTest, FirstAuctionTickEmitsOpenThenUpdate) {
    builder_.on_tick(make_tick(CLOCK_08_59_20, 100.5, 10, 1005.0, 7));

    ASSERT_EQ(sink_.events.size(), 2U);
    expect_event(sink_.events[0], EventKind::Open, true, TSEC_09_00, tsec_of(CLOCK_08_59_20));
    expect_event(sink_.events[1], EventKind::Update, true, TSEC_09_00, tsec_of(CLOCK_08_59_20));
    for (const BarEvent& event : sink_.events) {
        EXPECT_FALSE(event.bar.is_final);
        EXPECT_EQ(event.bar.trading_day, TRADING_DAY);
        EXPECT_EQ(event.bar.period_sec, 60);
        expect_auction_ohlc(event.bar, 100.5);  // 归后一小节首桶，O=H=L=C=竞价价
        EXPECT_EQ(event.bar.volume, 10);
        EXPECT_DOUBLE_EQ(event.bar.turnover, 1005.0);
        EXPECT_EQ(event.bar.open_interest, 7);
        EXPECT_EQ(event.bar.cum_volume, 10);
        EXPECT_DOUBLE_EQ(event.bar.cum_turnover, 1005.0);
    }
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), sink_.events[1].bar);
    // 竞价窗口内的 tick 是有效 tick：不进入 out_of_session 计数
    EXPECT_EQ(builder_.stats().out_of_session, 0U);
    EXPECT_EQ(builder_.stats().out_of_order, 0U);
    EXPECT_EQ(builder_.stats().bad_volume, 0U);
}

/// @brief 同窗口多笔竞价 tick：每笔一次 update(true)，O=H=L=C 逐笔覆盖，量额差分/OI 正确
TEST_F(IntradayBarAuctionTest, EachAuctionTickEmitsUpdateAndOverridesOhlc) {
    builder_.on_tick(make_tick(CLOCK_08_59_10, 100.5, 10, 1005.0, 7));
    builder_.on_tick(make_tick(CLOCK_08_59_20, 103.0, 15, 1545.0, 9));
    builder_.on_tick(make_tick(CLOCK_08_59_30, 99.0, 18, 1782.0, 8));

    ASSERT_EQ(sink_.events.size(), 4U);  // open + update（首笔）+ 两笔 update
    expect_event(sink_.events[0], EventKind::Open, true, TSEC_09_00, tsec_of(CLOCK_08_59_10));
    expect_event(sink_.events[1], EventKind::Update, true, TSEC_09_00, tsec_of(CLOCK_08_59_10));
    expect_event(sink_.events[2], EventKind::Update, true, TSEC_09_00, tsec_of(CLOCK_08_59_20));
    expect_event(sink_.events[3], EventKind::Update, true, TSEC_09_00, tsec_of(CLOCK_08_59_30));
    expect_auction_ohlc(sink_.events[0].bar, 100.5);
    expect_auction_ohlc(sink_.events[2].bar, 103.0);
    // 逐 tick 覆盖：最终 O=H=L=C=最后一笔竞价价，不是首笔价格
    expect_auction_ohlc(sink_.events[3].bar, 99.0);
    EXPECT_EQ(sink_.events[3].bar.volume, 18);
    EXPECT_DOUBLE_EQ(sink_.events[3].bar.turnover, 1782.0);
    EXPECT_EQ(sink_.events[3].bar.open_interest, 8);  // 持仓量取最后一笔
    EXPECT_EQ(sink_.events[3].bar.cum_volume, 18);
    EXPECT_DOUBLE_EQ(sink_.events[3].bar.cum_turnover, 1782.0);
    expect_bar_equal(builder_.current_bar(), sink_.events[3].bar);
}

/// @brief 竞价-only bar 在下一桶首笔时 close(is_final=true)；实例首根没有 close
TEST_F(IntradayBarAuctionTest, AuctionOnlyBarClosesWhenNextBucketStarts) {
    builder_.on_tick(make_tick(CLOCK_08_59_10, 100.5, 10, 1005.0, 7));
    builder_.on_tick(make_tick(CLOCK_08_59_30, 101.0, 12, 1212.0, 8));
    ASSERT_EQ(sink_.events.size(), 3U);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 0);  // 首根 bar 无 close

    // 09:01:00 的场内 tick 跨过 09:00 桶：竞价-only bar 正常收尾
    builder_.on_tick(make_tick(CLOCK_09_01_00, 102.0, 20, 2040.0, 9));

    ASSERT_EQ(sink_.events.size(), 6U);
    // close 的 is_auction 取触发 tick 的相位：本笔是连续交易 tick，故为 false
    expect_event(sink_.events[3], EventKind::Close, false, TSEC_09_00, tsec_of(CLOCK_08_59_30));
    EXPECT_TRUE(sink_.events[3].bar.is_final);
    expect_auction_ohlc(sink_.events[3].bar, 101.0);
    EXPECT_EQ(sink_.events[3].bar.volume, 12);
    EXPECT_EQ(sink_.events[3].bar.open_interest, 8);
    // 随后正常开新桶：open + update（is_auction=false）
    expect_event(sink_.events[4], EventKind::Open, false, TSEC_09_00 + 60, tsec_of(CLOCK_09_01_00));
    expect_event(sink_.events[5], EventKind::Update, false, TSEC_09_00 + 60,
                 tsec_of(CLOCK_09_01_00));
}

/// @brief 离场第一笔连续 tick：只有 update(false)，同一根 bar 继续更新、不重复 open
TEST_F(IntradayBarAuctionTest, FirstSessionTickAfterWindowOnlyUpdatesSameBar) {
    builder_.on_tick(make_tick(CLOCK_08_59_10, 100.0, 10, 1000.0, 7));
    builder_.on_tick(make_tick(CLOCK_08_59_20, 99.0, 13, 1287.0, 8));
    ASSERT_EQ(sink_.events.size(), 3U);

    builder_.on_tick(make_tick(CLOCK_09_00_20, 102.0, 20, 2040.0, 9));

    ASSERT_EQ(sink_.events.size(), 4U);
    expect_event(sink_.events[3], EventKind::Update, false, TSEC_09_00, tsec_of(CLOCK_09_00_20));
    const Bar& bar = sink_.events[3].bar;
    EXPECT_DOUBLE_EQ(bar.open, 99.0);  // 竞价开价不随后续 tick 变化
    EXPECT_DOUBLE_EQ(bar.high, 102.0);
    EXPECT_DOUBLE_EQ(bar.low, 99.0);
    EXPECT_DOUBLE_EQ(bar.close, 102.0);
    EXPECT_EQ(bar.volume, 20);
    EXPECT_EQ(bar.last_tick_sec, tsec_of(CLOCK_09_00_20));
    EXPECT_EQ(count_events(sink_, EventKind::Open), 1);  // 不再重复 open
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

    ASSERT_EQ(sink_.events.size(), 2U);  // 只有首笔的事件，被丢弃的 tick 不产生回调
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Update);
    for (const BarEvent& event : sink_.events) {
        EXPECT_TRUE(event.is_auction);
    }
    const BarBuilderStats stats = builder_.stats();
    EXPECT_EQ(stats.out_of_session, 0U);
    EXPECT_EQ(stats.out_of_order, 1U);
    EXPECT_EQ(stats.bad_volume, 1U);
    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_DOUBLE_EQ(bar.close, 100.0);
    EXPECT_EQ(bar.volume, 10);
    EXPECT_EQ(bar.last_tick_sec, tsec_of(CLOCK_08_59_20));
}

/// @brief 竞价窗口端点含：窗口起始秒接受、前一秒丢弃；窗口结束秒仍归竞价
TEST_F(IntradayBarAuctionTest, AuctionWindowBoundsAreInclusive) {
    constexpr DzTime CLOCK_08_58_59 = 8 * 3600 + 58 * 60 + 59;  // 窗口前一秒
    constexpr DzTime CLOCK_08_59_00 = 8 * 3600 + 59 * 60;       // 窗口起始秒
    constexpr DzTime CLOCK_08_59_59 = 8 * 3600 + 59 * 60 + 59;  // 窗口结束秒

    builder_.on_tick(make_tick(CLOCK_08_58_59, 100.0, 10, 1000.0, 7));  // 不在窗口/小节内
    EXPECT_FALSE(builder_.has_current_bar());
    EXPECT_TRUE(sink_.events.empty());
    EXPECT_EQ(builder_.stats().out_of_session, 1U);

    builder_.on_tick(make_tick(CLOCK_08_59_00, 101.0, 12, 1212.0, 8));  // 窗口起始秒
    ASSERT_EQ(sink_.events.size(), 2U);
    expect_event(sink_.events[0], EventKind::Open, true, TSEC_09_00, tsec_of(CLOCK_08_59_00));
    expect_event(sink_.events[1], EventKind::Update, true, TSEC_09_00, tsec_of(CLOCK_08_59_00));

    builder_.on_tick(make_tick(CLOCK_08_59_59, 102.0, 15, 1530.0, 9));  // 窗口结束秒
    ASSERT_EQ(sink_.events.size(), 3U);
    expect_event(sink_.events[2], EventKind::Update, true, TSEC_09_00, tsec_of(CLOCK_08_59_59));
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00 - 1);
    EXPECT_DOUBLE_EQ(builder_.current_bar().close, 102.0);
    EXPECT_EQ(builder_.stats().out_of_session, 1U);  // 窗口内 tick 不计入丢弃
}

/// @brief 跨交易日：新交易日夜盘竞价窗口首笔 close 旧日末根、open 新日首根并 update
TEST_F(IntradayBarAuctionTest, NextTradingDayAuctionClosesPreviousDayBarAndOpensNewDay) {
    constexpr DzDate NEXT_TRADING_DAY = TRADING_DAY + 1;

    builder_.on_tick(make_tick(CLOCK_15_00_00, 100.0, 100, 10000.0, 7, TRADING_DAY));
    ASSERT_EQ(sink_.events.size(), 2U);  // D 日末根 open + update

    // 新交易日 D+1 的夜盘开盘竞价（20:59:20）：close 旧日末根 → open 新日首根 → update
    builder_.on_tick(make_tick(CLOCK_08_59_20, 101.0, 130, 13130.0, 8, NEXT_TRADING_DAY));
    ASSERT_EQ(sink_.events.size(), 5U);
    expect_event(sink_.events[2], EventKind::Close, true, TSEC_15_00 - 60, TSEC_15_00);
    EXPECT_TRUE(sink_.events[2].bar.is_final);
    EXPECT_EQ(sink_.events[2].bar.trading_day, TRADING_DAY);
    expect_event(sink_.events[3], EventKind::Open, true, TSEC_09_00, tsec_of(CLOCK_08_59_20));
    expect_event(sink_.events[4], EventKind::Update, true, TSEC_09_00, tsec_of(CLOCK_08_59_20));
    EXPECT_EQ(sink_.events[4].bar.trading_day, NEXT_TRADING_DAY);
    EXPECT_EQ(sink_.events[4].bar.volume, 130);  // 新日累计基准归零
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, NEXT_TRADING_DAY);
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_00);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, tsec_of(CLOCK_08_59_20));
    EXPECT_EQ(builder_.stats().out_of_order, 0U);

    // 新日首笔场内 tick：同桶只 update（不再 open）
    builder_.on_tick(make_tick(CLOCK_09_00_20, 102.0, 150, 15300.0, 9, NEXT_TRADING_DAY));
    ASSERT_EQ(sink_.events.size(), 6U);
    expect_event(sink_.events[5], EventKind::Update, false, TSEC_09_00, tsec_of(CLOCK_09_00_20));
    EXPECT_EQ(sink_.events[5].bar.volume, 150);
}

/// @brief tick 流在竞价窗口内结束：最后一根 bar 保持 forming、不产生 close（惰性收盘）
TEST_F(IntradayBarAuctionTest, StreamEndingInsideAuctionWindowLeavesLastBarForming) {
    builder_.on_tick(make_tick(CLOCK_15_00_00, 100.0, 100, 10000.0, 7, TRADING_DAY));
    builder_.on_tick(make_tick(CLOCK_08_59_20, 101.0, 130, 13130.0, 8, TRADING_DAY + 1));

    EXPECT_EQ(count_events(sink_, EventKind::Open), 2);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 1);  // 只关闭了 D 日末根
    EXPECT_EQ(count_events(sink_, EventKind::Update), 2);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_FALSE(builder_.current_bar().is_final);
    EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY + 1);
    EXPECT_EQ(builder_.current_bar().volume, 130);  // 新日累计基准归零
}

/// @brief 商品多小节 + 多个长度不同的 Open 窗口：各归其目标小节首桶，事件序与相位正确
class IntradayBarCommodityAuctionTest : public ::testing::Test {
protected:
    TradingSessionTable table_ = commodity_auction_session_table();
    RecordingSink sink_;
    IntradayBarBuilder<RecordingSink> builder_{table_, 60, sink_};
};

TEST_F(IntradayBarCommodityAuctionTest, MultipleOpenAuctionWindowsEmitEventsInTheirWindows) {
    // 夜盘开盘竞价（30 秒窗口）：归 21:00 首桶，open+update 都携带 is_auction=true
    builder_.on_tick(make_tick(CLOCK_20_59_40, 100.0, 10, 1000.0, 7));
    ASSERT_EQ(sink_.events.size(), 2U);
    expect_event(sink_.events[0], EventKind::Open, true, TSEC_21_00, tsec_of(CLOCK_20_59_40));
    expect_event(sink_.events[1], EventKind::Update, true, TSEC_21_00, tsec_of(CLOCK_20_59_40));
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_21_00);

    // 早盘开盘竞价（90 秒窗口）：跨桶先 close 夜盘竞价 bar，再开 09:30 首桶
    builder_.on_tick(make_tick(CLOCK_09_29_00, 101.0, 12, 1212.0, 8));
    ASSERT_EQ(sink_.events.size(), 5U);
    expect_event(sink_.events[2], EventKind::Close, true, TSEC_21_00, tsec_of(CLOCK_20_59_40));
    EXPECT_TRUE(sink_.events[2].bar.is_final);
    expect_event(sink_.events[3], EventKind::Open, true, TSEC_09_30, tsec_of(CLOCK_09_29_00));
    expect_event(sink_.events[4], EventKind::Update, true, TSEC_09_30, tsec_of(CLOCK_09_29_00));
    EXPECT_EQ(sink_.events[4].bar.volume, 2);  // 12 - 上一笔累计 10
    EXPECT_EQ(builder_.stats().out_of_session, 0U);

    // 早盘开市：同桶只 update，相位切回 false
    builder_.on_tick(make_tick(CLOCK_09_30_00, 102.0, 20, 2040.0, 9));
    ASSERT_EQ(sink_.events.size(), 6U);
    expect_event(sink_.events[5], EventKind::Update, false, TSEC_09_30, TSEC_09_30);
    EXPECT_DOUBLE_EQ(sink_.events[5].bar.open, 101.0);  // 竞价开盘价保留
    EXPECT_EQ(sink_.events[5].bar.volume, 10);          // 20 - 10（bar 起始基准）
    EXPECT_EQ(builder_.stats().out_of_session, 0U);
}

/// @brief Close 窗口按普通 session tick 处理：正常回调，端点 tick 归前一根，is_auction=false
TEST_F(IntradayBarCommodityAuctionTest, CloseAuctionWindowTicksAreHandledAsNormalSessionTicks) {
    builder_.on_tick(make_tick(CLOCK_14_59_00, 100.0, 10, 1000.0, 7));  // 收盘竞价窗口内
    ASSERT_EQ(sink_.events.size(), 2U);
    expect_event(sink_.events[0], EventKind::Open, false, TSEC_15_00 - 60, tsec_of(CLOCK_14_59_00));
    expect_event(sink_.events[1], EventKind::Update, false, TSEC_15_00 - 60,
                 tsec_of(CLOCK_14_59_00));

    builder_.on_tick(make_tick(CLOCK_15_00_00, 101.0, 12, 1212.0, 8));  // 窗口端点 = 小节末端
    ASSERT_EQ(sink_.events.size(), 3U);
    expect_event(sink_.events[2], EventKind::Update, false, TSEC_15_00 - 60, TSEC_15_00);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 0);           // 末端 tick 不换根
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_15_00 - 60);  // 归前一根
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

    ASSERT_EQ(sink.events.size(), 2U);
    expect_event(sink.events[0], EventKind::Open, true, TSEC_13_00, tsec_of(CLOCK_11_10_00));
    expect_event(sink.events[1], EventKind::Update, true, TSEC_13_00, tsec_of(CLOCK_11_10_00));
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().begin_sec, TSEC_13_00);  // 窗口优先：归午盘首桶而非 11:10
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
    expect_event(sink.events[0], EventKind::Open, false, TSEC_10_00, tsec_of(CLOCK_10_00_00));
    expect_event(sink.events[1], EventKind::Update, false, TSEC_10_00, tsec_of(CLOCK_10_00_00));
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().begin_sec, TSEC_10_00);

    // 11:10：窗口内 → close 10:00 bar（相位 true）→ open 13:00 首桶 → update
    builder.on_tick(make_tick(CLOCK_11_10_00, 101.0, 12, 1212.0, 8));
    ASSERT_EQ(sink.events.size(), 5U);
    expect_event(sink.events[2], EventKind::Close, true, TSEC_10_00, tsec_of(CLOCK_10_00_00));
    expect_event(sink.events[3], EventKind::Open, true, TSEC_13_00, tsec_of(CLOCK_11_10_00));
    expect_event(sink.events[4], EventKind::Update, true, TSEC_13_00, tsec_of(CLOCK_11_10_00));

    // 12:00：既不在窗口也不在小节内 → 丢弃计数，不产生事件
    builder.on_tick(make_tick(CLOCK_12_00_00, 102.0, 14, 1428.0, 9));
    EXPECT_EQ(sink.events.size(), 5U);
    EXPECT_EQ(builder.stats().out_of_session, 1U);

    // 13:00 开市：同桶只 update（is_auction 切回 false）
    builder.on_tick(make_tick(CLOCK_13_00_00, 103.0, 20, 2060.0, 10));
    ASSERT_EQ(sink.events.size(), 6U);
    expect_event(sink.events[5], EventKind::Update, false, TSEC_13_00, TSEC_13_00);
    EXPECT_EQ(sink.events[5].bar.volume, 10);  // 20 - 10（bar 起始基准）
    EXPECT_EQ(builder.stats().out_of_session, 1U);
}

/// @brief 目标桶 = floor(目标小节开始 / 周期) * 周期：1 小时周期下 09:30 归 09:00 桶
TEST(IntradayBarAuctionGridTest, OpenAuctionTargetBucketFloorsSessionBeginToPeriodGrid) {
    const TradingSessionTable table = commodity_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 3600, sink);

    builder.on_tick(make_tick(CLOCK_09_29_00, 100.0, 10, 1000.0, 7));  // 早盘开盘竞价
    ASSERT_EQ(sink.events.size(), 2U);
    // floor((09:30 = 55800) / 3600) * 3600 = 54000（09:00 桶）
    expect_event(sink.events[0], EventKind::Open, true, TSEC_09_00, tsec_of(CLOCK_09_29_00));
    expect_event(sink.events[1], EventKind::Update, true, TSEC_09_00, tsec_of(CLOCK_09_29_00));

    builder.on_tick(make_tick(CLOCK_09_30_00, 101.0, 12, 1212.0, 8));  // 开市进场内同桶
    ASSERT_EQ(sink.events.size(), 3U);
    expect_event(sink.events[2], EventKind::Update, false, TSEC_09_00, TSEC_09_30);
    EXPECT_DOUBLE_EQ(sink.events[2].bar.open, 100.0);  // 竞价开价保留
    EXPECT_EQ(builder.stats().out_of_session, 0U);
}

/// @brief 粗周期跨小节拼根：目标桶已含场内 tick 时，竞价 tick 只 update、不覆盖已公开 O/H/L
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
    ASSERT_EQ(sink.events.size(), 3U);
    expect_event(sink.events[2], EventKind::Update, true, TSEC_10_00,
                 tsec_of(10 * 3600 + 16 * 60 + 10));
    ASSERT_TRUE(builder.has_current_bar());
    const Bar& bar = builder.current_bar();
    EXPECT_DOUBLE_EQ(bar.open, 100.0);  // 已公开开盘价不被竞价 tick 覆盖
    EXPECT_DOUBLE_EQ(bar.high, 105.0);  // 竞价 tick 按普通 tick 参与高低点
    EXPECT_DOUBLE_EQ(bar.low, 100.0);
    EXPECT_DOUBLE_EQ(bar.close, 105.0);
    EXPECT_EQ(bar.volume, 12);
    EXPECT_EQ(bar.last_tick_sec, tsec_of(10 * 3600 + 16 * 60 + 10));

    // 10:30 恢复场内：同桶只 update
    builder.on_tick(make_tick(10 * 3600 + 30 * 60, 99.0, 15, 1500.0, 9));
    ASSERT_EQ(sink.events.size(), 4U);
    expect_event(sink.events[3], EventKind::Update, false, TSEC_10_00, TSEC_10_30);
    EXPECT_DOUBLE_EQ(sink.events[3].bar.open, 100.0);
    EXPECT_DOUBLE_EQ(sink.events[3].bar.high, 105.0);
    EXPECT_DOUBLE_EQ(sink.events[3].bar.low, 99.0);
}

/// @brief 已公开 O/H/L 的桶再迎多笔竞价 tick：即使桶内价格恰好全相等，也不得覆盖已公开 O/H/L
///
/// 回归：不能从 OHLC 平坦性反推「桶只由竞价 tick 建立」——场内 tick 与竞价价恰好相同时
/// 桶仍是平的，后续第二笔竞价 tick 就会误走溢价覆盖分支。
TEST(IntradayBarAuctionCollisionTest, FlatPriceDoesNotMakeSessionBarLookAuctionOnly) {
    const TradingSessionTable table = day_break_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 3600, sink);

    builder.on_tick(make_tick(10 * 3600 + 5 * 60, 100.0, 10, 1000.0, 7));  // 10:05 场内
    // 10:16:00 竞价 tick 与已公开价相同：按普通 tick 合并，桶仍保持 O=H=L=C=100
    builder.on_tick(make_tick(10 * 3600 + 16 * 60, 100.0, 12, 1200.0, 8));
    ASSERT_EQ(sink.events.size(), 3U);
    expect_event(sink.events[2], EventKind::Update, true, TSEC_10_00, tsec_of(10 * 3600 + 16 * 60));

    // 第二笔竞价 tick：桶已含场内 tick，不得覆盖 10:05 已公开的 open/low
    builder.on_tick(make_tick(10 * 3600 + 16 * 60 + 30, 105.0, 15, 1575.0, 9));
    ASSERT_EQ(sink.events.size(), 4U);
    expect_event(sink.events[3], EventKind::Update, true, TSEC_10_00,
                 tsec_of(10 * 3600 + 16 * 60 + 30));
    const Bar& bar = builder.current_bar();
    EXPECT_DOUBLE_EQ(bar.open, 100.0);  // 已公开开盘价不被后续竞价 tick 覆盖
    EXPECT_DOUBLE_EQ(bar.high, 105.0);
    EXPECT_DOUBLE_EQ(bar.low, 100.0);
    EXPECT_DOUBLE_EQ(bar.close, 105.0);
    EXPECT_EQ(bar.volume, 15);
    EXPECT_EQ(bar.last_tick_sec, tsec_of(10 * 3600 + 16 * 60 + 30));
}

/// @brief 连续多个交易日只有竞价 tick（无任何场内 tick）：每天的事件照样完整、无延迟补发
TEST(IntradayBarAuctionOnlyStreamTest, ConsecutiveAuctionOnlyDaysEmitCompleteSequences) {
    const TradingSessionTable table = day_auction_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    // 超过旧实现待补发队列容量（MAX_AUCTIONS）的天数：证明已无队列/溢出路径
    constexpr int32_t kDays = TradingSessionTable::MAX_AUCTIONS + 2;
    for (int32_t day_offset = 0; day_offset < kDays; ++day_offset) {
        builder.on_tick(make_tick(CLOCK_08_59_20, 100.0, 10, 1000.0, 7, TRADING_DAY + day_offset));
    }

    EXPECT_EQ(count_events(sink, EventKind::Open), kDays);       // 每天恰好一次 open
    EXPECT_EQ(count_events(sink, EventKind::Close), kDays - 1);  // 每天关闭前一日
    EXPECT_EQ(count_events(sink, EventKind::Update), kDays);     // 每天恰好一次 update
    for (const BarEvent& event : sink.events) {
        EXPECT_TRUE(event.is_auction);  // 全部由竞价 tick 触发
    }
    // 首日：open → update；次日：close → open → update（旧竞价 bar 在下一窗口首笔关闭）
    ASSERT_GE(sink.events.size(), 5U);
    expect_event(sink.events[0], EventKind::Open, true, TSEC_09_00, tsec_of(CLOCK_08_59_20));
    expect_event(sink.events[1], EventKind::Update, true, TSEC_09_00, tsec_of(CLOCK_08_59_20));
    expect_event(sink.events[2], EventKind::Close, true, TSEC_09_00, tsec_of(CLOCK_08_59_20));
    EXPECT_EQ(sink.events[2].bar.trading_day, TRADING_DAY);
    EXPECT_TRUE(sink.events[2].bar.is_final);
    expect_event(sink.events[3], EventKind::Open, true, TSEC_09_00, tsec_of(CLOCK_08_59_20));
    EXPECT_EQ(sink.events[3].bar.trading_day, TRADING_DAY + 1);
    expect_event(sink.events[4], EventKind::Update, true, TSEC_09_00, tsec_of(CLOCK_08_59_20));
    EXPECT_EQ(sink.events[4].bar.volume, 10);  // 新日累计基准归零

    EXPECT_EQ(sink.events.back().kind, EventKind::Update);
    EXPECT_EQ(sink.events.back().bar.trading_day, TRADING_DAY + kDays - 1);
    EXPECT_FALSE(builder.current_bar().is_final);
    const BarBuilderStats stats = builder.stats();
    EXPECT_EQ(stats.out_of_session, 0U);
    EXPECT_EQ(stats.out_of_order, 0U);
    EXPECT_EQ(stats.bad_volume, 0U);
}
