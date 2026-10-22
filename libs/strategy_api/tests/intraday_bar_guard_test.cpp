#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

namespace {

/// @brief 非数（NaN）
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
/// @brief 正无穷
constexpr double kInf = std::numeric_limits<double>::infinity();

}  // namespace

/// @brief 坏数据守卫：时间/交易日非法、乱序、非法价格、累计量额倒退
///
/// 每笔被丢弃的 tick 必须不更新任何 bar 字段、不推进乱序基准、不触发 sink 回调，
/// 且只在对应计数器上精确递增一次。
class IntradayBarGuardTest : public ::testing::Test {
protected:
    /// @brief 建立一根形成中 bar 并返回其快照（后续坏 tick 必须不改变它）
    Bar accepted_reference_bar() {
        builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));
        builder_.on_tick(make_tick(9 * 3600 + 10, 103.0, 15, 1545.0, 9));
        EXPECT_TRUE(builder_.has_current_bar());
        return builder_.current_bar();
    }

    /// @brief 断言三个丢弃计数器精确等于给定值
    void expect_stats(uint64_t out_of_session, uint64_t out_of_order, uint64_t bad_volume) {
        const BarBuilderStats stats = builder_.stats();
        EXPECT_EQ(stats.out_of_session, out_of_session);
        EXPECT_EQ(stats.out_of_order, out_of_order);
        EXPECT_EQ(stats.bad_volume, bad_volume);
    }

    TradingSessionTable table_ = day_session_table();
    RecordingSink sink_;
    IntradayBarBuilder<RecordingSink> builder_{table_, 60, sink_};
};

/// @brief time 超出 0..86399：丢弃并计入 out_of_session，不建立 bar、不回调
TEST_F(IntradayBarGuardTest, InvalidTimeIsDroppedAndCountedAsOutOfSession) {
    const DzTime invalid_times[] = {-1,
                                    -86400,
                                    86400,
                                    90000,
                                    std::numeric_limits<DzTime>::min(),
                                    std::numeric_limits<DzTime>::max()};
    for (const DzTime invalid_time : invalid_times) {
        SCOPED_TRACE(::testing::Message() << "time=" << invalid_time);
        builder_.on_tick(make_tick(invalid_time, 100.5, 10, 1005.0, 7));
        EXPECT_FALSE(builder_.has_current_bar());
    }
    expect_stats(6, 0, 0);
    EXPECT_TRUE(sink_.events.empty());

    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));  // 合法 tick 照常接受
    expect_stats(6, 0, 0);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_00);
}

/// @brief last_price <= 0 / NaN / Inf：跳过且不计数、不更新任何 bar 字段
TEST_F(IntradayBarGuardTest, InvalidPriceIsSkippedWithoutCounting) {
    const Bar reference = accepted_reference_bar();
    const std::size_t events_before = sink_.events.size();

    const double invalid_prices[] = {0.0, -0.5, -100.0, kNaN, kInf, -kInf};
    for (const double invalid_price : invalid_prices) {
        SCOPED_TRACE(::testing::Message() << "last_price=" << invalid_price);
        builder_.on_tick(make_tick(9 * 3600 + 20, invalid_price, 18, 1854.0, 11));
        ASSERT_TRUE(builder_.has_current_bar());
        expect_bar_equal(builder_.current_bar(), reference);  // 未更新任何字段
    }
    expect_stats(0, 0, 0);                          // 非法价格只跳过，不计数
    EXPECT_EQ(sink_.events.size(), events_before);  // 不触发回调
}

/// @brief 首笔 tick 之前全是非法价格：不建立 bar，也不计数
TEST_F(IntradayBarGuardTest, InvalidPriceBeforeFirstBarLeavesNoBar) {
    builder_.on_tick(make_tick(9 * 3600, 0.0, 10, 1005.0, 7));
    builder_.on_tick(make_tick(9 * 3600 + 10, kNaN, 12, 1212.0, 8));

    EXPECT_FALSE(builder_.has_current_bar());
    expect_stats(0, 0, 0);
    EXPECT_TRUE(sink_.events.empty());
}

/// @brief 累计成交量倒退：丢弃并 bad_volume 精确递增，bar 与差分基准都不前进
TEST_F(IntradayBarGuardTest, VolumeRegressionIsDroppedAndCounted) {
    const Bar reference = accepted_reference_bar();  // 累计量 15

    builder_.on_tick(make_tick(9 * 3600 + 20, 104.0, 14, 1456.0, 12));  // 14 < 15
    EXPECT_EQ(builder_.stats().bad_volume, 1U);
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);

    // 被丢弃的 tick 不进入差分基准：下一笔仍以本 bar 起始基准（0）计差量
    builder_.on_tick(make_tick(9 * 3600 + 30, 105.0, 20, 2100.0, 13));
    EXPECT_EQ(builder_.stats().bad_volume, 1U);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().volume, 20);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00 + 30);
}

/// @brief 仅累计成交量倒退、成交额仍在前进：量守卫单独生效（不被 turnover 检查掩盖）
TEST_F(IntradayBarGuardTest, VolumeOnlyRegressionIsDroppedAndCounted) {
    const Bar reference = accepted_reference_bar();  // 累计量 15、累计额 1545

    // 量 14 < 15，但额 1600 > 1545：只有成交量守卫能拒掉这一笔
    builder_.on_tick(make_tick(9 * 3600 + 20, 104.0, 14, 1600.0, 12));
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);
}

/// @brief 仅累计成交额倒退、成交量仍在前进：额守卫单独生效
TEST_F(IntradayBarGuardTest, TurnoverOnlyRegressionIsDroppedAndCounted) {
    const Bar reference = accepted_reference_bar();  // 累计量 15、累计额 1545

    builder_.on_tick(make_tick(9 * 3600 + 20, 104.0, 18, 1500.0, 12));  // 量 18 > 15、额倒退
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);
}

/// @brief 累计成交额倒退（量前进）：丢弃并 bad_volume 精确递增
TEST_F(IntradayBarGuardTest, TurnoverRegressionIsDroppedAndCounted) {
    const Bar reference = accepted_reference_bar();  // 累计额 1545

    builder_.on_tick(make_tick(9 * 3600 + 20, 104.0, 18, 1500.0, 12));  // 额 1500 < 1545
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);
}

/// @brief 累计成交额为 NaN：按坏量丢弃（NaN 不参与比较，必须显式判定）
TEST_F(IntradayBarGuardTest, NaNTurnoverIsDroppedAsBadVolume) {
    const Bar reference = accepted_reference_bar();

    builder_.on_tick(make_tick(9 * 3600 + 20, 104.0, 18, kNaN, 12));
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);
}

/// @brief 首笔 tick 的成交额为 NaN：同样按坏量丢弃（不建立 NaN bar）
TEST_F(IntradayBarGuardTest, NaNTurnoverOnFirstTickIsDropped) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, kNaN, 7));

    EXPECT_FALSE(builder_.has_current_bar());
    expect_stats(0, 0, 1);
    EXPECT_TRUE(sink_.events.empty());
}

/// @brief 累计成交额为正无穷：按坏量丢弃（Inf 能通过 >= 比较但会永久毒化后续差分）
TEST_F(IntradayBarGuardTest, InfiniteTurnoverIsDroppedAsBadVolume) {
    const Bar reference = accepted_reference_bar();

    builder_.on_tick(make_tick(9 * 3600 + 20, 104.0, 18, kInf, 12));
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);

    // 后续合法 tick 不被 Inf 基准毒化
    builder_.on_tick(make_tick(9 * 3600 + 30, 105.0, 20, 2100.0, 13));
    expect_stats(0, 0, 1);
    EXPECT_EQ(builder_.current_bar().cum_volume, 20);
    EXPECT_DOUBLE_EQ(builder_.current_bar().cum_turnover, 2100.0);
}

/// @brief 首笔 tick 的成交额为 Inf：同样按坏量丢弃（不建立 Inf bar）
TEST_F(IntradayBarGuardTest, InfiniteTurnoverOnFirstTickIsDropped) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, kInf, 7));

    EXPECT_FALSE(builder_.has_current_bar());
    expect_stats(0, 0, 1);
    EXPECT_TRUE(sink_.events.empty());
}

/// @brief 被丢弃的 tick 不推进乱序基准：随后的合法 tick 只与最后一笔已接受 tick 比较
TEST_F(IntradayBarGuardTest, DroppedTicksDoNotAdvanceOutOfOrderBaseline) {
    accepted_reference_bar();  // 最后接受 tick 在 09:00:10

    // 非法价格落在 09:00:20：若错误地推进乱序基准，此后的 09:00:15 会被误判为乱序
    builder_.on_tick(make_tick(9 * 3600 + 20, kNaN, 16, 1600.0, 10));
    expect_stats(0, 0, 0);

    builder_.on_tick(make_tick(9 * 3600 + 15, 101.0, 17, 1717.0, 11));
    expect_stats(0, 0, 0);  // 09:00:15 > 09:00:10，合法接受
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00 + 15);

    // 坏量 tick 同样不推进基准
    builder_.on_tick(make_tick(9 * 3600 + 40, 102.0, 1, 100.0, 12));  // 量额倒退被丢弃
    expect_stats(0, 0, 1);
    builder_.on_tick(make_tick(9 * 3600 + 25, 102.0, 18, 1836.0, 12));
    expect_stats(0, 0, 1);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00 + 25);
}

/// @brief 坏量 tick 不改变任何 bar 字段（不只是不切桶）
TEST_F(IntradayBarGuardTest, BadVolumeTickLeavesEveryBarFieldUnchanged) {
    const Bar reference = accepted_reference_bar();
    const std::size_t events_before = sink_.events.size();

    builder_.on_tick(make_tick(9 * 3600 + 20, 200.0, 1, 100.0, 99));  // 价格/OI 都很有诱惑力
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);  // 一个字段都不许变
    EXPECT_EQ(sink_.events.size(), events_before);
}

/// @brief 跨桶的坏量 tick 不触发 close/open：状态不前进，桶边界不会被坏数据推进
TEST_F(IntradayBarGuardTest, BadVolumeTickInLaterBucketDoesNotCloseOrOpenBar) {
    const Bar reference = accepted_reference_bar();
    const int closes_before = count_events(sink_, EventKind::Close);
    const int opens_before = count_events(sink_, EventKind::Open);

    builder_.on_tick(make_tick(9 * 3600 + 60, 104.0, 12, 1248.0, 12));  // 下一桶，但量 12 < 15
    expect_stats(0, 0, 1);
    EXPECT_EQ(count_events(sink_, EventKind::Close), closes_before);
    EXPECT_EQ(count_events(sink_, EventKind::Open), opens_before);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);  // 仍是 09:00 根

    builder_.on_tick(make_tick(9 * 3600 + 60, 104.0, 20, 2080.0, 13));  // 同桶合法 tick
    EXPECT_EQ(builder_.stats().bad_volume, 1U);
    EXPECT_EQ(count_events(sink_, EventKind::Close), closes_before + 1);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_00 + 60);
    EXPECT_EQ(builder_.current_bar().volume, 5);  // 20 - 15（基准仍是最后一笔已接受 tick）
}

/// @brief tsec 倒退：丢弃并 out_of_order 精确递增，不建立更早的 bar
TEST_F(IntradayBarGuardTest, OutOfOrderTickIsDroppedAndCounted) {
    const Bar reference = accepted_reference_bar();  // 最后接受 tick 在 09:00:10

    builder_.on_tick(make_tick(9 * 3600 + 9, 99.0, 16, 1584.0, 10));  // 09:00:09 倒退
    expect_stats(0, 1, 0);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);

    // 同交易日内的更早时刻：同样丢弃
    builder_.on_tick(make_tick(9 * 3600 + 5, 98.0, 17, 1666.0, 10));
    expect_stats(0, 2, 0);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_00);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 0);
}

/// @brief 同一 tsec 的多笔 tick 全部接受（乱序判定是严格小于）
TEST_F(IntradayBarGuardTest, TicksWithinSameSecondAreAllAccepted) {
    builder_.on_tick(make_tick(9 * 3600, 100.0, 10, 1000.0, 7));
    builder_.on_tick(make_tick(9 * 3600, 101.0, 13, 1313.0, 8));
    builder_.on_tick(make_tick(9 * 3600, 102.0, 15, 1530.0, 9));

    expect_stats(0, 0, 0);
    EXPECT_EQ(count_events(sink_, EventKind::Update), 3);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_DOUBLE_EQ(builder_.current_bar().high, 102.0);
    EXPECT_EQ(builder_.current_bar().volume, 15);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00);
}

/// @brief 交易日非正：丢弃并计入 out_of_order，不建立 bar
TEST_F(IntradayBarGuardTest, NonPositiveTradingDayIsDroppedAsOutOfOrder) {
    builder_.on_tick(make_tick(9 * 3600, 100.0, 10, 1000.0, 7, 0));
    builder_.on_tick(make_tick(9 * 3600 + 10, 101.0, 12, 1212.0, 8, -3));

    EXPECT_FALSE(builder_.has_current_bar());
    expect_stats(0, 2, 0);
    EXPECT_TRUE(sink_.events.empty());
}

/// @brief 交易日倒退：丢弃并计入 out_of_order，当前 bar 不被更早交易日污染
TEST_F(IntradayBarGuardTest, RegressedTradingDayIsDroppedAsOutOfOrder) {
    const Bar reference = accepted_reference_bar();

    builder_.on_tick(make_tick(9 * 3600 + 20, 104.0, 18, 1854.0, 12, TRADING_DAY - 1));
    expect_stats(0, 1, 0);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);
}

/// @brief 交易日前进：累计基准重置为 0，新交易日首笔 delta 不被旧日累计污染
///
/// 只断言差分基准（本票范围）；跨日的空交易日/夜盘组合等完整生命周期由 #88 覆盖。
TEST_F(IntradayBarGuardTest, ForwardTradingDayResetsCumulativeBasis) {
    accepted_reference_bar();  // 当日累计量 15、累计额 1545

    // 新交易日、新桶：若错误沿用旧日基准，volume 会算成 3-15=-12
    builder_.on_tick(make_tick(9 * 3600 + 60, 100.0, 3, 300.0, 7, TRADING_DAY + 1));
    expect_stats(0, 0, 0);  // 既不是坏量也不是乱序
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_00 + 60);
    EXPECT_EQ(builder_.current_bar().volume, 3);
    EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, 300.0);

    builder_.on_tick(make_tick(9 * 3600 + 120, 101.0, 5, 505.0, 8, TRADING_DAY + 1));
    expect_stats(0, 0, 0);
    EXPECT_EQ(builder_.current_bar().volume, 2);  // 5 - 3
    EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, 205.0);
}

/// @brief 交易日前进但落在同一桶：开新根且量额以 0 为基准，绝不发布负量额
TEST_F(IntradayBarGuardTest, ForwardTradingDayInSameBucketNeverPublishesNegativeVolume) {
    const Bar reference =
        accepted_reference_bar();  // 当日累计量 15、累计额 1545，最后 tick 09:00:10
    const int closes_before = count_events(sink_, EventKind::Close);
    const int opens_before = count_events(sink_, EventKind::Open);

    // 09:00:30 仍属 09:00 桶，但已是新交易日、当日累计从 3 起
    builder_.on_tick(make_tick(9 * 3600 + 30, 100.0, 3, 300.0, 7, TRADING_DAY + 1));
    expect_stats(0, 0, 0);  // 跨日不算坏量

    // 交易日变化即便同桶也必须先 close 旧根、再 open 新根
    EXPECT_EQ(count_events(sink_, EventKind::Close), closes_before + 1);
    EXPECT_EQ(count_events(sink_, EventKind::Open), opens_before + 1);
    const Bar* closed = last_event_bar(sink_, EventKind::Close);
    ASSERT_NE(closed, nullptr);
    EXPECT_TRUE(closed->is_final);
    EXPECT_EQ(closed->trading_day, TRADING_DAY);
    EXPECT_EQ(closed->begin_sec, TSEC_09_00);
    // 被收的正是旧交易日那根，内容未被新日 tick 污染（is_final 由 close 置位，其余逐字段比对）
    EXPECT_EQ(closed->last_tick_sec, reference.last_tick_sec);
    EXPECT_DOUBLE_EQ(closed->open, reference.open);
    EXPECT_DOUBLE_EQ(closed->high, reference.high);
    EXPECT_DOUBLE_EQ(closed->low, reference.low);
    EXPECT_DOUBLE_EQ(closed->close, reference.close);
    EXPECT_EQ(closed->volume, reference.volume);
    EXPECT_DOUBLE_EQ(closed->turnover, reference.turnover);
    EXPECT_EQ(closed->open_interest, reference.open_interest);
    EXPECT_EQ(closed->cum_volume, reference.cum_volume);
    EXPECT_DOUBLE_EQ(closed->cum_turnover, reference.cum_turnover);

    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.trading_day, TRADING_DAY + 1);
    EXPECT_EQ(bar.begin_sec, TSEC_09_00);  // 同一桶：begin_sec 不变
    EXPECT_EQ(bar.volume, 3);              // 以 0 为基准，不是 3 - 15
    EXPECT_DOUBLE_EQ(bar.turnover, 300.0);
    EXPECT_GE(bar.volume, 0);
    EXPECT_GE(bar.turnover, 0.0);
}

/// @brief 跨日同桶 tick 之后交易日倒退仍按乱序丢弃（跨日已换根，基准是当前 bar 的交易日）
TEST_F(IntradayBarGuardTest, RegressedTradingDayAfterSameBucketCrossDayIsDropped) {
    accepted_reference_bar();
    builder_.on_tick(make_tick(9 * 3600 + 30, 100.0, 3, 300.0, 7, TRADING_DAY + 1));
    ASSERT_TRUE(builder_.has_current_bar());
    const Bar reference = builder_.current_bar();

    builder_.on_tick(make_tick(9 * 3600 + 40, 101.0, 5, 505.0, 8, TRADING_DAY));  // 回到旧交易日
    expect_stats(0, 1, 0);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);
}

/// @brief 各守卫互不串扰：四种缺陷交替出现时三个计数器各自精确递增
TEST_F(IntradayBarGuardTest, GuardsDoNotCrossIncrementCounters) {
    builder_.on_tick(make_tick(9 * 3600, 100.0, 10, 1000.0, 7));   // 接受
    builder_.on_tick(make_tick(16 * 3600, 101.0, 11, 1111.0, 8));  // 日盘 09:00–15:00 之外
    expect_stats(1, 0, 0);

    builder_.on_tick(make_tick(9 * 3600 + 30, kNaN, 12, 1212.0, 9));  // 非法价格
    expect_stats(1, 0, 0);

    builder_.on_tick(
        make_tick(9 * 3600 - 10, 102.0, 13, 1326.0, 10));  // 乱序（早于已接受 09:00:00）
    expect_stats(1, 1, 0);

    builder_.on_tick(make_tick(9 * 3600 + 40, 103.0, 9, 927.0, 11));  // 量额倒退
    expect_stats(1, 1, 1);

    builder_.on_tick(make_tick(9 * 3600 + 50, 104.0, 20, 2080.0, 12));  // 接受
    expect_stats(1, 1, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().volume, 20);  // 同一根内基准为 0：20 - 0
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00 + 50);
    EXPECT_EQ(count_events(sink_, EventKind::Update), 2);  // 只有两笔被接受的 tick
}

/// @brief 时间域守卫优先于量额守卫：同一笔 tick 同时 time 越界与量额倒退只计一次
TEST_F(IntradayBarGuardTest, TimeDomainGuardsTakePrecedenceOverVolumeGuard) {
    accepted_reference_bar();

    builder_.on_tick(make_tick(90000, 104.0, 1, 100.0, 12));  // time 越界 + 量额倒退
    expect_stats(1, 0, 0);

    builder_.on_tick(make_tick(9 * 3600 + 5, 104.0, 1, 100.0, 12));  // tsec 倒退 + 量额倒退
    expect_stats(1, 1, 0);
}
