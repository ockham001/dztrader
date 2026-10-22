#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <cstdint>
#include <iterator>
#include <vector>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

namespace {

/// @brief 多周期矩阵共用的日盘 tick 流（日盘三段小节，含 10:15–10:30 小节休息）
///
/// 覆盖：同桶多笔、09:04:59/10:14:59 等非整分钟时间点、10:00–10:15 与 10:30–11:00
/// 两段、10:15/11:30/15:00 小节末端、13:30 午盘恢复。输入时刻用墙钟秒构造，期望边界
/// 用交易日内秒常量手工推导，两侧不共享编码，避免断言与被测实现同义反复。
std::vector<DzTick> matrix_tick_stream() {
    constexpr DzTime clocks[] = {
        9 * 3600,                  // 09:00:00
        9 * 3600 + 3 * 60,         // 09:03:00
        9 * 3600 + 4 * 60 + 59,    // 09:04:59
        9 * 3600 + 15 * 60,        // 09:15:00
        10 * 3600,                 // 10:00:00
        10 * 3600 + 14 * 60 + 59,  // 10:14:59
        10 * 3600 + 15 * 60,       // 10:15:00（早盘小节末端）
        10 * 3600 + 30 * 60,       // 10:30:00（休息后恢复）
        10 * 3600 + 45 * 60,       // 10:45:00
        10 * 3600 + 59 * 60 + 59,  // 10:59:59
        11 * 3600,                 // 11:00:00
        11 * 3600 + 30 * 60,       // 11:30:00（早盘小节末端）
        13 * 3600 + 30 * 60,       // 13:30:00（午盘恢复）
        15 * 3600,                 // 15:00:00（日盘小节末端）
    };
    std::vector<DzTick> ticks;
    ticks.reserve(std::size(clocks));
    int index = 0;
    for (const DzTime clock : clocks) {
        const int64_t volume = 10 * static_cast<int64_t>(index + 1);  // 当日累计成交量
        ticks.push_back(make_tick(clock, 100.0 + static_cast<double>(index),
                                  static_cast<DzVolume>(volume), static_cast<double>(volume),
                                  static_cast<DzLargeVolume>(index + 1)));
        ++index;
    }
    return ticks;
}

/// @brief 收集完整 bar 序列：已收 bar 按 close 事件顺序，末尾补当前形成中 bar
std::vector<Bar> emitted_bars(const RecordingSink& sink,
                              const IntradayBarBuilder<RecordingSink>& builder) {
    std::vector<Bar> bars;
    for (const BarEvent& event : sink.events) {
        if (event.kind == EventKind::Close) {
            bars.push_back(event.bar);
        }
    }
    if (builder.has_current_bar()) {
        bars.push_back(builder.current_bar());
    }
    return bars;
}

/// @brief 提取 bar 序列的 begin_sec（逐根断言的直接比较对象）
std::vector<int32_t> begin_secs(const std::vector<Bar>& bars) {
    std::vector<int32_t> result;
    result.reserve(bars.size());
    for (const Bar& bar : bars) {
        result.push_back(bar.begin_sec);
    }
    return result;
}

/// @brief 一个周期的期望结果
struct PeriodCase {
    int32_t period_sec;                       ///< 周期秒数
    std::vector<int32_t> expected_begin_sec;  ///< 逐根 begin_sec（交易日内秒，18:00 锚点网格）
};

/// @brief 按 ADR 0031 边界公式手工推导的期望边界（逐根，不复用被测实现）
///
/// 旧版逐根一致性由 #92 差分验收测试另行覆盖（本文件不做旧版 oracle）。
std::vector<PeriodCase> period_cases() {
    return {
        // 1 分钟：10:14:59 与 10:15:00 有效时间相同归同一根；11:30/15:00 末端归前一根
        {60,
         {TSEC_09_00, TSEC_09_00 + 180, TSEC_09_00 + 240, TSEC_09_00 + 900, TSEC_10_00,
          TSEC_10_15 - 60, TSEC_10_30, TSEC_10_30 + 900, TSEC_11_00 - 60, TSEC_11_00,
          TSEC_11_30 - 60, TSEC_13_30, TSEC_15_00 - 60}},
        // 3 分钟：09:03 桶吸收 09:04:59
        {180,
         {TSEC_09_00, TSEC_09_00 + 180, TSEC_09_00 + 900, TSEC_10_00, TSEC_10_15 - 180, TSEC_10_30,
          TSEC_10_30 + 900, TSEC_11_00 - 180, TSEC_11_00, TSEC_11_30 - 180, TSEC_13_30,
          TSEC_15_00 - 180}},
        // 5 分钟：09:00 桶吸收 09:03/09:04:59
        {300,
         {TSEC_09_00, TSEC_09_00 + 900, TSEC_10_00, TSEC_10_15 - 300, TSEC_10_30, TSEC_10_30 + 900,
          TSEC_11_00 - 300, TSEC_11_00, TSEC_11_30 - 300, TSEC_13_30, TSEC_15_00 - 300}},
        // 15 分钟：10:14:59/10:15:00 与 10:00 同桶、10:59:59 与 10:45 同桶
        {900,
         {TSEC_09_00, TSEC_09_00 + 900, TSEC_10_00, TSEC_10_30, TSEC_10_30 + 900, TSEC_11_00,
          TSEC_11_30 - 900, TSEC_13_30, TSEC_15_00 - 900}},
        // 1 小时：10:00–10:15 与 10:30–11:00 拼进同一根 10:00–11:00（跨休息不切根）
        {3600, {TSEC_09_00, TSEC_10_00, TSEC_11_00, TSEC_13_00, TSEC_15_00 - 3600}},
    };
}

}  // namespace

/// @brief 多周期矩阵：1/3/5/15/60 分钟对同一 tick 流逐根断言 begin_sec 与 bar 数量
TEST(IntradayBarPeriodMatrixTest, SameTickStreamProducesPeriodSpecificBoundaries) {
    const TradingSessionTable table = day_break_session_table();
    const std::vector<DzTick> ticks = matrix_tick_stream();

    for (const PeriodCase& test_case : period_cases()) {
        SCOPED_TRACE(::testing::Message() << "period_sec=" << test_case.period_sec);
        RecordingSink sink;
        IntradayBarBuilder<RecordingSink> builder(table, test_case.period_sec, sink);
        for (const DzTick& tick : ticks) {
            builder.on_tick(tick);
        }

        EXPECT_EQ(builder.stats().out_of_session, 0U);  // 全部 tick 落在交易小节内（含末端）
        ASSERT_TRUE(builder.has_current_bar());
        const std::vector<Bar> bars = emitted_bars(sink, builder);
        EXPECT_EQ(begin_secs(bars), test_case.expected_begin_sec);  // 逐根边界 + bar 数量
        EXPECT_EQ(count_events(sink, EventKind::Open), static_cast<int>(bars.size()));
        EXPECT_EQ(count_events(sink, EventKind::Update), static_cast<int>(ticks.size()));
        for (const Bar& bar : bars) {
            EXPECT_EQ(bar.period_sec, test_case.period_sec);
        }
    }
}

/// @brief 1 小时周期跨小节休息拼根：休息前后的 tick 进入同一根 bar，不按小节切根
class IntradayBarHourBreakTest : public ::testing::Test {
protected:
    TradingSessionTable table_ = day_break_session_table();
    RecordingSink sink_;
    IntradayBarBuilder<RecordingSink> builder_{table_, 3600, sink_};
};

TEST_F(IntradayBarHourBreakTest, TicksOnBothSidesOfMorningBreakShareOneStitchedBar) {
    // 10:00–10:15 段
    builder_.on_tick(make_tick(10 * 3600, 100.0, 10, 1000.0, 7));                // 10:00:00
    builder_.on_tick(make_tick(10 * 3600 + 5 * 60, 102.0, 12, 1224.0, 8));       // 10:05:00
    builder_.on_tick(make_tick(10 * 3600 + 14 * 60 + 59, 99.0, 15, 1485.0, 9));  // 10:14:59
    builder_.on_tick(make_tick(10 * 3600 + 15 * 60, 100.0, 16, 1600.0, 9));  // 10:15:00 小节末端
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_10_00);
    EXPECT_EQ(builder_.current_bar().last_tick_sec, TSEC_10_15);  // 末端 tick 已进入本根
    EXPECT_EQ(count_events(sink_, EventKind::Close), 0);          // 小节结束不收盘

    // 10:15–10:30 休息时段：tick 丢弃，不收盘、不切根
    builder_.on_tick(make_tick(10 * 3600 + 20 * 60, 101.0, 17, 1717.0, 9));
    // 10:30–11:00 段：同一根 10:00–11:00 bar 继续 update
    builder_.on_tick(make_tick(10 * 3600 + 30 * 60, 101.0, 18, 1818.0, 10));       // 10:30:00
    builder_.on_tick(make_tick(10 * 3600 + 45 * 60, 104.0, 21, 2184.0, 11));       // 10:45:00
    builder_.on_tick(make_tick(10 * 3600 + 59 * 60 + 59, 103.0, 25, 2575.0, 12));  // 10:59:59

    const BarBuilderStats stats = builder_.stats();
    EXPECT_EQ(stats.out_of_session, 1U);
    EXPECT_EQ(count_events(sink_, EventKind::Open), 1);  // 跨休息只有一根 bar
    EXPECT_EQ(count_events(sink_, EventKind::Update), 7);
    EXPECT_EQ(count_events(sink_, EventKind::Close), 0);
    ASSERT_TRUE(builder_.has_current_bar());
    const Bar& bar = builder_.current_bar();
    EXPECT_EQ(bar.begin_sec, TSEC_10_00);
    EXPECT_EQ(bar.begin_clock(), 10 * 3600);  // 自然整点
    EXPECT_EQ(bar.last_tick_sec, TSEC_11_00 - 1);
    EXPECT_EQ(bar.period_sec, 3600);
    EXPECT_DOUBLE_EQ(bar.open, 100.0);   // 第一段首笔
    EXPECT_DOUBLE_EQ(bar.high, 104.0);   // 第二段
    EXPECT_DOUBLE_EQ(bar.low, 99.0);     // 第一段
    EXPECT_DOUBLE_EQ(bar.close, 103.0);  // 第二段末笔
    EXPECT_EQ(bar.volume, 25);           // 两段累计差分（基准 0）
    EXPECT_DOUBLE_EQ(bar.turnover, 2575.0);
    EXPECT_EQ(bar.open_interest, 12);
    EXPECT_FALSE(bar.is_final);

    // 11:00 桶边界 tick：先收 10:00 根（is_final=true）→ 再开 11:00 根
    builder_.on_tick(make_tick(11 * 3600, 105.0, 28, 2940.0, 13));
    ASSERT_EQ(count_events(sink_, EventKind::Close), 1);
    const Bar* closed = last_event_bar(sink_, EventKind::Close);
    ASSERT_NE(closed, nullptr);
    EXPECT_TRUE(closed->is_final);
    EXPECT_EQ(closed->begin_sec, TSEC_10_00);
    EXPECT_EQ(closed->last_tick_sec, TSEC_11_00 - 1);
    EXPECT_DOUBLE_EQ(closed->open, 100.0);
    EXPECT_DOUBLE_EQ(closed->high, 104.0);
    EXPECT_DOUBLE_EQ(closed->low, 99.0);
    EXPECT_DOUBLE_EQ(closed->close, 103.0);
    EXPECT_EQ(closed->volume, 25);
    EXPECT_DOUBLE_EQ(closed->turnover, 2575.0);
    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_11_00);
    EXPECT_DOUBLE_EQ(builder_.current_bar().open, 105.0);
    EXPECT_EQ(builder_.current_bar().volume, 3);               // 28 - 25
    EXPECT_DOUBLE_EQ(builder_.current_bar().turnover, 365.0);  // 2940 - 2575
}

/// @brief 1 小时周期按自然整点对齐（含夜盘跨午夜），不是按小节起点对齐
TEST(IntradayBarHourAlignmentTest, OneHourBarsAlignToNaturalWallClockHoursAcrossMidnight) {
    const TradingSessionTable table = commodity_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 3600, sink);

    constexpr DzTime clocks[] = {
        21 * 3600 + 30 * 60,       // 21:30 夜盘
        21 * 3600 + 59 * 60 + 59,  // 21:59:59
        22 * 3600,                 // 22:00
        23 * 3600 + 59 * 60 + 59,  // 23:59:59
        0,                         // 次日 00:00
        30 * 60,                   // 次日 00:30
        2 * 3600 + 30 * 60,        // 次日 02:30（夜盘小节末端）
        9 * 3600 + 30 * 60,        // 09:30（早盘小节开始）
        10 * 3600 + 59 * 60 + 59,  // 10:59:59
    };
    for (const DzTime clock : clocks) {
        builder.on_tick(make_tick(clock, 100.0, 1, 100.0, 1));
    }

    const std::vector<Bar> bars = emitted_bars(sink, builder);
    const std::vector<int32_t> expected_begin_sec = {
        TSEC_21_00,
        TSEC_21_00 + 3600,      // 22:00
        TSEC_21_00 + 2 * 3600,  // 23:00
        TSEC_21_00 + 3 * 3600,  // 次日 00:00
        TSEC_21_00 + 5 * 3600,  // 次日 02:00（02:30 小节末端归前一根）
        TSEC_09_00,             // 09:00（09:30 小节开始落在 09:00–10:00 桶内）
        TSEC_10_00,
    };
    const DzTime expected_begin_clock[] = {
        21 * 3600, 22 * 3600, 23 * 3600, 0, 2 * 3600, 9 * 3600, 10 * 3600,
    };
    ASSERT_EQ(bars.size(), std::size(expected_begin_clock));
    EXPECT_EQ(begin_secs(bars), expected_begin_sec);
    EXPECT_EQ(count_events(sink, EventKind::Open), 7);
    EXPECT_EQ(count_events(sink, EventKind::Close), 6);
    for (std::size_t i = 0; i < bars.size(); ++i) {
        EXPECT_EQ(bars[i].period_sec, 3600);
        EXPECT_EQ(bars[i].begin_clock(), expected_begin_clock[i]);  // 自然整点
    }
}

/// @brief 桶起点 = floor(trading_sec / period_sec) * period_sec（18:00 锚点整数网格）：
///        周期可以是任意正整数秒，不限于整分钟
TEST(IntradayBarAnchorGridTest, NonMinutePeriodStartsOnTradingDayAnchorGrid) {
    const TradingSessionTable table = day_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 1000, sink);

    builder.on_tick(make_tick(9 * 3600, 100.0, 10, 1000.0, 7));            // 09:00:00 → tsec 54000
    builder.on_tick(make_tick(9 * 3600 + 10 * 60, 101.0, 12, 1212.0, 8));  // 09:10:00 同桶
    builder.on_tick(make_tick(9 * 3600 + 16 * 60 + 39, 102.0, 15, 1530.0, 9));   // 09:16:39 同桶
    builder.on_tick(make_tick(9 * 3600 + 16 * 60 + 40, 103.0, 18, 1854.0, 10));  // 内部边界 → 新桶

    const std::vector<Bar> bars = emitted_bars(sink, builder);
    const std::vector<int32_t> expected_begin_sec = {TSEC_09_00, TSEC_09_00 + 1000};
    EXPECT_EQ(begin_secs(bars), expected_begin_sec);
    ASSERT_EQ(bars.size(), 2U);
    EXPECT_EQ(bars[0].begin_clock(), 9 * 3600);                 // 18:00 锚点网格上 09:00:00
    EXPECT_EQ(bars[1].begin_clock(), 9 * 3600 + 16 * 60 + 40);  // 09:16:40 边界 tick 归新 bar
    EXPECT_EQ(count_events(sink, EventKind::Close), 1);
    EXPECT_EQ(count_events(sink, EventKind::Update), 4);
}

/// @brief 无成交桶不生成 bar：相邻 bar 的 begin_sec 跳跃，累计量额基准仍接上一笔已接受 tick
TEST(IntradayBarEmptyBucketTest, EmptyHoursProduceNoBarsAndKeepCumulativeBasis) {
    const TradingSessionTable table = day_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 3600, sink);

    builder.on_tick(make_tick(9 * 3600, 100.0, 10, 1000.0, 7));   // 09:00 → 09:00 桶
    builder.on_tick(make_tick(11 * 3600, 101.0, 20, 2020.0, 8));  // 11:00（10:00 无成交）
    builder.on_tick(make_tick(14 * 3600, 102.0, 30, 3060.0, 9));  // 14:00（12:00/13:00 无成交）

    const std::vector<Bar> bars = emitted_bars(sink, builder);
    const std::vector<int32_t> expected_begin_sec = {TSEC_09_00, TSEC_11_00, TSEC_15_00 - 3600};
    EXPECT_EQ(begin_secs(bars), expected_begin_sec);
    EXPECT_EQ(builder.stats().out_of_session, 0U);
    EXPECT_EQ(count_events(sink, EventKind::Open), 3);
    EXPECT_EQ(count_events(sink, EventKind::Close), 2);
    EXPECT_EQ(count_events(sink, EventKind::Update), 3);
    ASSERT_EQ(bars.size(), 3U);
    // 空桶不生成 bar、也不重置差分基准：每根量额 = 本笔累计 - 上一笔已接受 tick 累计
    EXPECT_EQ(bars[0].volume, 10);
    EXPECT_EQ(bars[1].volume, 10);  // 20 - 10
    EXPECT_EQ(bars[2].volume, 10);  // 30 - 20
    EXPECT_DOUBLE_EQ(bars[0].turnover, 1000.0);
    EXPECT_DOUBLE_EQ(bars[1].turnover, 1020.0);  // 2020 - 1000
    EXPECT_DOUBLE_EQ(bars[2].turnover, 1040.0);  // 3060 - 2020
}
