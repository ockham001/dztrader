/**
 * @file daily_bar_guard_test.cpp
 * @brief 日线 builder 的坏数据守卫与丢弃计数用例（#91）
 *
 * 日线不依赖交易小节/竞价表，守卫只剩时间域、交易日/时间顺序、非法价格与累计量额倒退；
 * 语义与日内容 builder 一致。每笔被丢弃的 tick 必须不更新任何 bar 字段、不推进基准、
 * 不触发 sink 回调，且只在对应计数器上精确递增一次。
 */

#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <cstddef>
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

class DailyBarGuardTest : public ::testing::Test {
protected:
    /// @brief 建立一根形成中日线并返回其快照（后续坏 tick 必须不改变它）
    Bar accepted_reference_bar() {
        builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
        builder_.on_tick(make_daily_tick(10 * 3600, 100.0, 105.0, 98.0, 104.0, 1500, 156000.0, 9));
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

    RecordingSink sink_;
    DailyBarBuilder<RecordingSink> builder_{sink_};
};

/// @brief time 超出 0..86399：丢弃并计入 out_of_session，不建立 bar、不回调
TEST_F(DailyBarGuardTest, InvalidTimeIsDroppedAndCountedAsOutOfSession) {
    const DzTime invalid_times[] = {-1,
                                    -86400,
                                    86400,
                                    90000,
                                    std::numeric_limits<DzTime>::min(),
                                    std::numeric_limits<DzTime>::max()};
    for (const DzTime invalid_time : invalid_times) {
        SCOPED_TRACE(::testing::Message() << "time=" << invalid_time);
        builder_.on_tick(
            make_daily_tick(invalid_time, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
        EXPECT_FALSE(builder_.has_current_bar());
    }
    expect_stats(6, 0, 0);
    EXPECT_TRUE(sink_.events.empty());

    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    expect_stats(6, 0, 0);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00);
}

/// @brief last_price <= 0 / NaN / Inf：跳过且不计数、不更新任何 bar 字段
TEST_F(DailyBarGuardTest, InvalidPriceIsSkippedWithoutCounting) {
    const Bar reference = accepted_reference_bar();
    const std::size_t events_before = sink_.events.size();

    const double invalid_prices[] = {0.0, -0.5, -100.0, kNaN, kInf, -kInf};
    for (const double invalid_price : invalid_prices) {
        SCOPED_TRACE(::testing::Message() << "last_price=" << invalid_price);
        builder_.on_tick(
            make_daily_tick(11 * 3600, 100.0, 106.0, 97.0, invalid_price, 1800, 187200.0, 10));
        ASSERT_TRUE(builder_.has_current_bar());
        expect_bar_equal(builder_.current_bar(), reference);  // 未更新任何字段
    }
    expect_stats(0, 0, 0);                          // 非法价格只跳过，不计数
    EXPECT_EQ(sink_.events.size(), events_before);  // 不触发回调
}

/// @brief 首笔 tick 之前全是非法价格：不建立 bar，也不计数
TEST_F(DailyBarGuardTest, InvalidPriceBeforeFirstBarLeavesNoBar) {
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 0.0, 1000, 101000.0, 7));
    builder_.on_tick(make_daily_tick(9 * 3600 + 10, 100.0, 102.0, 99.0, kNaN, 1000, 101000.0, 7));

    EXPECT_FALSE(builder_.has_current_bar());
    expect_stats(0, 0, 0);
    EXPECT_TRUE(sink_.events.empty());
}

/// @brief 交易日非正：丢弃并计入 out_of_order，不建立 bar
TEST_F(DailyBarGuardTest, NonPositiveTradingDayIsDroppedAsOutOfOrder) {
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7, 0));
    builder_.on_tick(
        make_daily_tick(9 * 3600 + 10, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7, -3));

    EXPECT_FALSE(builder_.has_current_bar());
    expect_stats(0, 2, 0);
    EXPECT_TRUE(sink_.events.empty());
}

/// @brief 交易日倒退：丢弃并计入 out_of_order，当前 bar 不被更早交易日污染
TEST_F(DailyBarGuardTest, RegressedTradingDayIsDroppedAsOutOfOrder) {
    const Bar reference = accepted_reference_bar();

    builder_.on_tick(
        make_daily_tick(11 * 3600, 90.0, 91.0, 89.0, 90.5, 1800, 187200.0, 10, TRADING_DAY - 1));
    expect_stats(0, 1, 0);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);
}

/// @brief 同交易日时间倒退：丢弃并计入 out_of_order，当前 bar 不变
TEST_F(DailyBarGuardTest, RegressedTimeSameDayIsDroppedAsOutOfOrder) {
    const Bar reference = accepted_reference_bar();  // 最后接受 tick 在 10:00

    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 106.0, 97.0, 105.0, 1800, 187200.0, 10));
    expect_stats(0, 1, 0);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);

    builder_.on_tick(make_daily_tick(9 * 3600 + 30, 100.0, 106.0, 97.0, 105.0, 1800, 187200.0,
                                     10));  // 仍早于 10:00
    expect_stats(0, 2, 0);
    expect_bar_equal(builder_.current_bar(), reference);
}

/// @brief 累计成交量倒退：丢弃并 bad_volume 精确递增，bar 与基准都不前进
TEST_F(DailyBarGuardTest, VolumeRegressionIsDroppedAndCounted) {
    const Bar reference = accepted_reference_bar();  // 累计量 1500

    builder_.on_tick(make_daily_tick(11 * 3600, 100.0, 106.0, 97.0, 105.0, 1499, 187200.0, 10));
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);

    // 被丢弃的 tick 不进入基准：下一笔合法 tick 照常接受
    builder_.on_tick(make_daily_tick(11 * 3600, 100.0, 106.0, 97.0, 105.0, 1600, 167600.0, 10));
    expect_stats(0, 0, 1);
    EXPECT_EQ(builder_.current_bar().volume, 1600);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_11_00);
}

/// @brief 仅累计成交量倒退、成交额仍在前进：量守卫单独生效
TEST_F(DailyBarGuardTest, VolumeOnlyRegressionIsDroppedAndCounted) {
    const Bar reference = accepted_reference_bar();  // 累计量 1500、累计额 156000

    // 量 1499 < 1500，但额 160000 > 156000：只有成交量守卫能拒掉这一笔
    builder_.on_tick(make_daily_tick(11 * 3600, 100.0, 106.0, 97.0, 105.0, 1499, 160000.0, 10));
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);
}

/// @brief 仅累计成交额倒退、成交量仍在前进：额守卫单独生效
TEST_F(DailyBarGuardTest, TurnoverOnlyRegressionIsDroppedAndCounted) {
    const Bar reference = accepted_reference_bar();  // 累计量 1500、累计额 156000

    builder_.on_tick(make_daily_tick(11 * 3600, 100.0, 106.0, 97.0, 105.0, 1800, 150000.0, 10));
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);
}

/// @brief 累计成交额为 NaN：按坏量丢弃（NaN 不参与比较，必须显式判定）
TEST_F(DailyBarGuardTest, NaNTurnoverIsDroppedAsBadVolume) {
    const Bar reference = accepted_reference_bar();

    builder_.on_tick(make_daily_tick(11 * 3600, 100.0, 106.0, 97.0, 105.0, 1800, kNaN, 10));
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);
}

/// @brief 累计成交额为 Inf：按坏量丢弃（Inf 会永久毒化后续差分），后续合法 tick 不受影响
TEST_F(DailyBarGuardTest, InfiniteTurnoverIsDroppedAsBadVolume) {
    const Bar reference = accepted_reference_bar();

    builder_.on_tick(make_daily_tick(11 * 3600, 100.0, 106.0, 97.0, 105.0, 1800, kInf, 10));
    expect_stats(0, 0, 1);
    expect_bar_equal(builder_.current_bar(), reference);

    builder_.on_tick(make_daily_tick(12 * 3600, 100.0, 107.0, 97.0, 106.0, 2000, 210000.0, 11));
    expect_stats(0, 0, 1);
    EXPECT_EQ(builder_.current_bar().cum_volume, 2000);
    EXPECT_DOUBLE_EQ(builder_.current_bar().cum_turnover, 210000.0);
}

/// @brief 首笔 tick 的累计量为负：按坏量丢弃（日线量即当日累计量，不允许负值）
TEST_F(DailyBarGuardTest, NegativeVolumeOnFirstTickIsDroppedAsBadVolume) {
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, -1, 101000.0, 7));

    EXPECT_FALSE(builder_.has_current_bar());
    expect_stats(0, 0, 1);
    EXPECT_TRUE(sink_.events.empty());
}

/// @brief 首笔 tick 的累计成交额为负：按坏量丢弃
TEST_F(DailyBarGuardTest, NegativeTurnoverOnFirstTickIsDroppedAsBadVolume) {
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, -1.0, 7));

    EXPECT_FALSE(builder_.has_current_bar());
    expect_stats(0, 0, 1);
    EXPECT_TRUE(sink_.events.empty());
}

/// @brief 首笔 tick 的成交额为 NaN：同样按坏量丢弃（不建立 NaN bar）
TEST_F(DailyBarGuardTest, NaNTurnoverOnFirstTickIsDropped) {
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, kNaN, 7));

    EXPECT_FALSE(builder_.has_current_bar());
    expect_stats(0, 0, 1);
    EXPECT_TRUE(sink_.events.empty());
}

/// @brief 首笔 tick 的成交额为 Inf：同样按坏量丢弃（不建立 Inf bar）
TEST_F(DailyBarGuardTest, InfiniteTurnoverOnFirstTickIsDropped) {
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, kInf, 7));

    EXPECT_FALSE(builder_.has_current_bar());
    expect_stats(0, 0, 1);
    EXPECT_TRUE(sink_.events.empty());
}

/// @brief 被丢弃的 tick 不推进乱序基准：随后的合法 tick 只与最后一笔已接受 tick 比较
TEST_F(DailyBarGuardTest, DroppedTicksDoNotAdvanceOutOfOrderBaseline) {
    accepted_reference_bar();  // 最后接受 tick 在 10:00

    // 非法价格落在 10:20：若错误地推进乱序基准，此后的 10:10 会被误判为乱序
    builder_.on_tick(
        make_daily_tick(10 * 3600 + 20 * 60, 100.0, 106.0, 97.0, kNaN, 1600, 167600.0, 10));
    expect_stats(0, 0, 0);

    builder_.on_tick(
        make_daily_tick(10 * 3600 + 10 * 60, 100.0, 106.0, 97.0, 105.0, 1600, 167600.0, 10));
    expect_stats(0, 0, 0);  // 10:10 > 10:00，合法接受
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_10_00 + 600);

    // 坏量 tick 同样不推进基准
    builder_.on_tick(make_daily_tick(10 * 3600 + 40 * 60, 100.0, 106.0, 97.0, 105.0, 1, 100.0, 10));
    expect_stats(0, 0, 1);
    builder_.on_tick(
        make_daily_tick(10 * 3600 + 25 * 60, 100.0, 106.0, 97.0, 105.0, 1700, 178500.0, 10));
    expect_stats(0, 0, 1);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_10_00 + 1500);
}

/// @brief 坏量 tick 不改变任何 bar 字段（不只是不切根）
TEST_F(DailyBarGuardTest, BadVolumeTickLeavesEveryBarFieldUnchanged) {
    const Bar reference = accepted_reference_bar();
    const std::size_t events_before = sink_.events.size();

    builder_.on_tick(make_daily_tick(11 * 3600, 90.0, 120.0, 80.0, 115.0, 1, 100.0, 99));
    expect_stats(0, 0, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    expect_bar_equal(builder_.current_bar(), reference);  // 一个字段都不许变
    EXPECT_EQ(sink_.events.size(), events_before);
}

/// @brief 交易日前进：累计基准重置为 0，新交易日首笔量额不被旧日累计污染
TEST_F(DailyBarGuardTest, ForwardTradingDayResetsCumulativeBasis) {
    accepted_reference_bar();  // 当日累计量 1500、累计额 156000

    // 新交易日、量额远小于旧日累计：若错误沿用旧日基准会被误判为 bad_volume
    builder_.on_tick(
        make_daily_tick(9 * 3600, 50.0, 51.0, 49.0, 50.5, 5, 505.0, 3, TRADING_DAY + 1));
    expect_stats(0, 0, 0);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().trading_day, TRADING_DAY + 1);
    EXPECT_EQ(builder_.current_bar().volume, 5);
    EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, 505.0);
    EXPECT_EQ(builder_.current_bar().cum_volume, 5);
}

/// @brief 各守卫互不串扰：缺陷交替出现时三个计数器各自精确递增
TEST_F(DailyBarGuardTest, GuardsDoNotCrossIncrementCounters) {
    builder_.on_tick(make_daily_tick(9 * 3600, 100.0, 102.0, 99.0, 101.0, 1000, 101000.0, 7));
    builder_.on_tick(make_daily_tick(90000, 100.0, 102.0, 99.0, 101.0, 1100, 111100.0, 8));
    expect_stats(1, 0, 0);

    builder_.on_tick(make_daily_tick(9 * 3600 + 30, 100.0, 102.0, 99.0, kNaN, 1100, 111100.0, 8));
    expect_stats(1, 0, 0);  // 非法价格只跳过

    builder_.on_tick(make_daily_tick(9 * 3600 - 10, 100.0, 102.0, 99.0, 101.0, 1100, 111100.0, 8));
    expect_stats(1, 1, 0);  // 时间倒退

    builder_.on_tick(make_daily_tick(9 * 3600 + 40, 100.0, 102.0, 99.0, 101.0, 999, 111000.0, 8));
    expect_stats(1, 1, 1);  // 累计量倒退

    builder_.on_tick(make_daily_tick(9 * 3600 + 50, 100.0, 102.0, 99.0, 101.0, 1200, 121200.0, 8));
    expect_stats(1, 1, 1);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().volume, 1200);  // 日线量即累计量
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_09_00 + 50);
    EXPECT_EQ(count_events(sink_, EventKind::Update), 2);  // 只有两笔被接受的 tick
}

/// @brief 时间域守卫优先于量额守卫：同一笔 tick 同时 time 越界与量额倒退只计一次
TEST_F(DailyBarGuardTest, TimeDomainGuardsTakePrecedenceOverVolumeGuard) {
    accepted_reference_bar();

    builder_.on_tick(make_daily_tick(90000, 100.0, 102.0, 99.0, 101.0, 1, 100.0, 12));
    expect_stats(1, 0, 0);

    builder_.on_tick(make_daily_tick(9 * 3600 + 5, 100.0, 102.0, 99.0, 101.0, 1, 100.0, 12));
    expect_stats(1, 1, 0);
}
