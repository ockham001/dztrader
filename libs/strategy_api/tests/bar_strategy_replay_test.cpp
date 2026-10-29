/**
 * @file bar_strategy_replay_test.cpp
 * @brief 历史预热回放的纯组件单元测试（issue #161；接缝 C）
 *
 * 覆盖两件与帧通道无关、却决定回放保真度的事：
 *
 *   1. **合成 tick 回放**（组件文档 §5.3）：每根归档 bar 拆 4 个合成 tick（涨 `O→L→H→C`、
 *      跌 `O→H→L→C`、平 `O→L→H→C`），量/额按 `k/4` 线性累计。把合成 tick 喂回真实
 *      `IntradayBarBuilder`，产出的 bar 必须与归档 bar 逐字段一致——回放走的必须是 live 那条
 *      唯一的 tick→bar 实现，否则会出现两套 bar 语义。
 *   2. **分桶判定的唯一实现**（`bar::resolve_tick_bucket`）：builder 与 A 的缓冲 tick 续接
 *      过滤必须用同一份「tick 属于哪根 bar」的判定，否则过滤会按一套规则丢、builder 按另一套
 *      收，续接静默漂移。这里逐 tick 差分比较「resolver 的结论」与「builder 的实际行为」。
 */
#include "bar_test_data.h"

#include <dztrader/bar.h>
#include <dztrader/bar_strategy/dependencies.h>

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace dztrader;
using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;
using dztrader::bar_strategy_internal::make_replay_ticks;
using dztrader::bar_strategy_internal::REPLAY_TICKS_PER_BAR;

/// @brief 一根归档 bar 的构造参数（测试按字段名给，避免一大串位置参数传错）
struct ArchiveBarSpec {
    DzDate trading_day = TRADING_DAY;
    int32_t begin_sec = TSEC_09_00;
    int32_t period_sec = 60;
    double open = 0;
    double high = 0;
    double low = 0;
    double close = 0;
    std::int64_t volume = 0;
    double turnover = 0;
    DzLargeVolume open_interest = 0;
    std::int64_t cum_volume = 0;
    double cum_turnover = 0;
    bool is_final = true;
};

/// @brief 按 {@link ArchiveBarSpec} 构造一根归档 bar（last_tick_sec 取桶内最后一秒）
Bar make_archive_bar(const ArchiveBarSpec& spec) {
    Bar bar{};
    bar.trading_day = spec.trading_day;
    bar.begin_sec = spec.begin_sec;
    bar.last_tick_sec = spec.begin_sec + spec.period_sec - 1;
    bar.period_sec = spec.period_sec;
    bar.open = spec.open;
    bar.high = spec.high;
    bar.low = spec.low;
    bar.close = spec.close;
    bar.volume = spec.volume;
    bar.turnover = spec.turnover;
    bar.open_interest = spec.open_interest;
    bar.cum_volume = spec.cum_volume;
    bar.cum_turnover = spec.cum_turnover;
    bar.is_final = spec.is_final;
    return bar;
}

/// @brief 断言回放产出的 bar 与归档 bar 一致
///
/// `last_tick_sec` 也要相等：合成 tick 的时刻以归档的 `last_tick_sec` 为末点
/// （`make_replay_ticks` 口径），因此回放能还原它——这正是 forming bar 的缓冲 tick 能续接
/// 而不被当乱序丢掉的前提。
void expect_replay_bar_equal(const Bar& actual, const Bar& expected) {
    EXPECT_EQ(actual.trading_day, expected.trading_day);
    EXPECT_EQ(actual.begin_sec, expected.begin_sec);
    EXPECT_EQ(actual.last_tick_sec, expected.last_tick_sec);
    EXPECT_EQ(actual.period_sec, expected.period_sec);
    EXPECT_DOUBLE_EQ(actual.open, expected.open);
    EXPECT_DOUBLE_EQ(actual.high, expected.high);
    EXPECT_DOUBLE_EQ(actual.low, expected.low);
    EXPECT_DOUBLE_EQ(actual.close, expected.close);
    EXPECT_EQ(actual.volume, expected.volume);
    EXPECT_DOUBLE_EQ(actual.turnover, expected.turnover);
    EXPECT_EQ(actual.open_interest, expected.open_interest);
    EXPECT_EQ(actual.cum_volume, expected.cum_volume);
    EXPECT_DOUBLE_EQ(actual.cum_turnover, expected.cum_turnover);
    EXPECT_EQ(actual.is_final, expected.is_final);
}

/// @brief 把一根归档 bar 的 4 个合成 tick 喂进真实 builder（回放路径的最小复刻）
void feed_replay_bar(const Bar& bar, IntradayBarBuilder<RecordingSink>& builder) {
    for (const DzTick& tick : make_replay_ticks(bar)) {
        builder.on_tick(tick);
    }
}

/**
 * @brief 喂下一根 bar 的首笔 tick，让 builder 以 `is_final=true` 关闭当前 bar
 *
 * 纯 tick 驱动没有 flush（组件文档 §5.1）：最后一根只有在更晚的 tick 到达时才收。这里用
 * 一根虚构的「下一根」把 target 关掉，再看 close 事件里交出的 bar。
 */
void close_current_bar(const Bar& current, IntradayBarBuilder<RecordingSink>& builder) {
    const int32_t next_clock = TradingTime::to_clock(current.begin_sec + current.period_sec + 1);
    builder.on_tick(
        make_tick(next_clock, current.close, static_cast<DzVolume>(current.cum_volume + 1),
                  current.cum_turnover + 1.0, current.open_interest, current.trading_day));
}

/// @brief 目标 bar 的 4 个 update 事件里交出的收盘价序列（= 合成 tick 的价格顺序）
std::vector<double> update_closes_for(const RecordingSink& sink, std::int32_t begin_sec) {
    std::vector<double> closes;
    for (const BarEvent& event : sink.events) {
        if (event.kind == EventKind::Update && event.bar.begin_sec == begin_sec) {
            closes.push_back(event.bar.close);
        }
    }
    return closes;
}

/// @brief 目标 bar 的 4 个 update 事件里交出的成交量序列（= 四等分累计）
std::vector<std::int64_t> update_volumes_for(const RecordingSink& sink, std::int32_t begin_sec) {
    std::vector<std::int64_t> volumes;
    for (const BarEvent& event : sink.events) {
        if (event.kind == EventKind::Update && event.bar.begin_sec == begin_sec) {
            volumes.push_back(event.bar.volume);
        }
    }
    return volumes;
}

/**
 * @brief 回放目标 bar：先喂一根基线 bar 建立累计量额基准，再喂目标 bar 并关闭它
 *
 * 基线 bar 是必须的：归档 bar 的 `cum_volume/cum_turnover` 是**当日累计**，第一根 bar 的
 * 基准为 0。只喂目标 bar 会让 builder 以 0 为基准，量额凭空多出基线部分。
 *
 * @return 关闭事件里交出的目标 bar（builder 视角的「回放产出」）
 */
Bar replay_with_baseline(const Bar& baseline,
                         const Bar& target,
                         RecordingSink& sink,
                         IntradayBarBuilder<RecordingSink>& builder) {
    feed_replay_bar(baseline, builder);
    feed_replay_bar(target, builder);
    close_current_bar(target, builder);
    for (auto it = sink.events.rbegin(); it != sink.events.rend(); ++it) {
        if (it->kind == EventKind::Close && it->bar.begin_sec == target.begin_sec) {
            return it->bar;
        }
    }
    return Bar{};
}

/**
 * @brief 回放保全用例的公共基线：09:00 前一根（08:59 桶）已完成 bar
 *
 * 交易日、小节都取日盘单小节表（09:00–15:00）；基线放在 09:00 桶，目标桶 09:01。
 */
ArchiveBarSpec baseline_spec() {
    return ArchiveBarSpec{
        .begin_sec = TSEC_09_00,
        .open = 99.0,
        .high = 99.0,
        .low = 99.0,
        .close = 99.0,
        .volume = 100,
        .turnover = 9900.0,
        .open_interest = 1000,
        .cum_volume = 100,
        .cum_turnover = 9900.0,
        .is_final = true,
    };
}

// ============================================================
// 合成 tick：涨 / 跌 / 平三种顺序
// ============================================================

/// 上涨 bar（close > open）：合成 tick 依次 `O→L→H→C`，回放产出与归档逐字段一致
TEST(BarStrategyReplayTest, RisingBarIsRebuiltFromSyntheticTicks) {
    TradingSessionTable table = day_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    const Bar baseline = make_archive_bar(baseline_spec());
    const Bar target = make_archive_bar(ArchiveBarSpec{
        .begin_sec = TSEC_09_00 + 60,
        .open = 100.0,
        .high = 105.0,
        .low = 99.0,
        .close = 104.0,
        .volume = 40,
        .turnover = 4100.0,
        .open_interest = 1234,
        .cum_volume = 140,
        .cum_turnover = 14000.0,
    });

    const Bar produced = replay_with_baseline(baseline, target, sink, builder);
    expect_replay_bar_equal(produced, target);
    EXPECT_EQ(update_closes_for(sink, target.begin_sec),
              (std::vector<double>{100.0, 99.0, 105.0, 104.0}))
        << "a rising bar must replay as O -> L -> H -> C";
}

/// 下跌 bar（close < open）：合成 tick 依次 `O→H→L→C`
TEST(BarStrategyReplayTest, FallingBarIsRebuiltFromSyntheticTicks) {
    TradingSessionTable table = day_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    const Bar baseline = make_archive_bar(baseline_spec());
    const Bar target = make_archive_bar(ArchiveBarSpec{
        .begin_sec = TSEC_09_00 + 60,
        .open = 105.0,
        .high = 106.0,
        .low = 100.0,
        .close = 101.0,
        .volume = 30,
        .turnover = 3075.0,
        .open_interest = 4321,
        .cum_volume = 130,
        .cum_turnover = 12975.0,
    });

    const Bar produced = replay_with_baseline(baseline, target, sink, builder);
    expect_replay_bar_equal(produced, target);
    EXPECT_EQ(update_closes_for(sink, target.begin_sec),
              (std::vector<double>{105.0, 106.0, 100.0, 101.0}))
        << "a falling bar must replay as O -> H -> L -> C";
}

/// 平盘 bar（close == open）：合成 tick 依次 `O→L→H→C`，高低点仍被还原
TEST(BarStrategyReplayTest, FlatBarIsRebuiltFromSyntheticTicks) {
    TradingSessionTable table = day_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    const Bar baseline = make_archive_bar(baseline_spec());
    const Bar target = make_archive_bar(ArchiveBarSpec{
        .begin_sec = TSEC_09_00 + 60,
        .open = 100.0,
        .high = 101.5,
        .low = 98.5,
        .close = 100.0,
        .volume = 12,
        .turnover = 1200.0,
        .open_interest = 777,
        .cum_volume = 112,
        .cum_turnover = 11100.0,
    });

    const Bar produced = replay_with_baseline(baseline, target, sink, builder);
    expect_replay_bar_equal(produced, target);
    EXPECT_EQ(update_closes_for(sink, target.begin_sec),
              (std::vector<double>{100.0, 98.5, 101.5, 100.0}))
        << "a flat bar must replay as O -> L -> H -> C";
}

// ============================================================
// 量额四等分累计
// ============================================================

/// 量/额按 `k/4` 线性累计：中间 3 个 tick 的差分与整数/浮点四等分一致，第 4 个落在归档值
TEST(BarStrategyReplayTest, VolumeAndTurnoverAreAccumulatedInQuarters) {
    TradingSessionTable table = day_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    const Bar baseline = make_archive_bar(baseline_spec());
    const Bar target = make_archive_bar(ArchiveBarSpec{
        .begin_sec = TSEC_09_00 + 60,
        .open = 100.0,
        .high = 100.5,
        .low = 99.5,
        .close = 100.25,
        .volume = 41,
        .turnover = 41.5,
        .open_interest = 1000,
        .cum_volume = 141,
        .cum_turnover = 9941.5,
    });

    const Bar produced = replay_with_baseline(baseline, target, sink, builder);
    expect_replay_bar_equal(produced, target);
    // 41 × k/4：10、20、30、41（整数除法；最后一格必须精确等于归档量）
    EXPECT_EQ(update_volumes_for(sink, target.begin_sec),
              (std::vector<std::int64_t>{10, 20, 30, 41}));

    // 额按 k/4.0 线性：10.375、20.75、31.125、41.5
    std::vector<double> turnovers;
    for (const BarEvent& event : sink.events) {
        if (event.kind == EventKind::Update && event.bar.begin_sec == target.begin_sec) {
            turnovers.push_back(event.bar.turnover);
        }
    }
    ASSERT_EQ(turnovers.size(), 4U);
    EXPECT_DOUBLE_EQ(turnovers[0], 10.375);
    EXPECT_DOUBLE_EQ(turnovers[1], 20.75);
    EXPECT_DOUBLE_EQ(turnovers[2], 31.125);
    EXPECT_DOUBLE_EQ(turnovers[3], 41.5);
}

/// 跨交易日：新交易日首根 bar 的当日累计量额从 0 起算（archive 的 cum 字段同样是当日累计）
TEST(BarStrategyReplayTest, CrossDayReplayResetsDailyCumulativeBasis) {
    TradingSessionTable table = day_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 60, sink);

    const Bar day1_last = make_archive_bar(ArchiveBarSpec{
        .trading_day = TRADING_DAY,
        .begin_sec = TSEC_15_00 - 60,
        .open = 200.0,
        .high = 201.0,
        .low = 199.0,
        .close = 200.5,
        .volume = 50,
        .turnover = 10000.0,
        .open_interest = 900,
        .cum_volume = 5000,
        .cum_turnover = 1000000.0,
    });
    const Bar day2_first = make_archive_bar(ArchiveBarSpec{
        .trading_day = TRADING_DAY + 1,
        .begin_sec = TSEC_09_00,
        .open = 201.0,
        .high = 203.0,
        .low = 200.0,
        .close = 202.0,
        .volume = 7,
        .turnover = 1414.0,
        .open_interest = 901,
        // 新交易日累计从 0 起算：首根 bar 的当日累计 == 本 bar 量额
        .cum_volume = 7,
        .cum_turnover = 1414.0,
    });

    feed_replay_bar(day1_last, builder);
    feed_replay_bar(day2_first, builder);
    close_current_bar(day2_first, builder);

    const Bar* closed = nullptr;
    for (const BarEvent& event : sink.events) {
        if (event.kind == EventKind::Close && event.bar.trading_day == day2_first.trading_day &&
            event.bar.begin_sec == day2_first.begin_sec) {
            closed = &event.bar;
        }
    }
    ASSERT_NE(closed, nullptr) << "the first bar of the new trading day must be closed";
    expect_replay_bar_equal(*closed, day2_first);
}

// ============================================================
// 分桶判定的唯一实现：resolver 与 builder 逐 tick 差分
// ============================================================

/// @brief 一次差分用例：一笔 tick 的墙钟时刻与（期望的）分桶结论
struct BucketCase {
    DzTime clock;
    double price;
    DzVolume cum_volume;
    double cum_turnover;
};

/**
 * @brief 逐 tick 差分比较 `resolve_tick_bucket` 与 builder 的实际行为
 *
 * 断言两件事：
 *   1. resolver 判定为「在交易时段内」的 tick，builder 必须接受并把 bar 落在 resolver 给出的
 *      桶起点上（同一份判定）；
 *   2. resolver 判定为「不在任何小节/Open 竞价窗口内」的 tick，builder 必须计入
 *      `out_of_session` 且不改变当前 bar。
 */
void expect_resolver_matches_builder(const TradingSessionTable& table,
                                     int32_t period_sec,
                                     const std::vector<BucketCase>& cases) {
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, period_sec, sink);
    int32_t last_begin_sec = -1;

    for (const BucketCase& bucket_case : cases) {
        SCOPED_TRACE(::testing::Message() << "clock=" << bucket_case.clock);
        const int32_t tsec = TradingTime::from_clock(bucket_case.clock);
        const TickBucket expected = resolve_tick_bucket(tsec, table, period_sec);
        const auto sessions_before = builder.stats();
        const bool had_bar = builder.has_current_bar();
        const int32_t begin_before = had_bar ? builder.current_bar().begin_sec : -1;

        builder.on_tick(make_tick(bucket_case.clock, bucket_case.price, bucket_case.cum_volume,
                                  bucket_case.cum_turnover, 1000));

        if (!expected.in_session) {
            EXPECT_EQ(builder.stats().out_of_session, sessions_before.out_of_session + 1)
                << "an out-of-session tick must be dropped and counted";
            if (had_bar) {
                EXPECT_EQ(builder.current_bar().begin_sec, begin_before);
            }
            continue;
        }

        ASSERT_TRUE(builder.has_current_bar()) << "an accepted tick must open a bar";
        EXPECT_EQ(builder.current_bar().begin_sec, expected.begin_sec)
            << "resolver and builder must agree on the bar bucket";
        // 桶起点单调：同一张表内后面的 tick 不会退回更早的桶
        EXPECT_GE(builder.current_bar().begin_sec, last_begin_sec);
        last_begin_sec = builder.current_bar().begin_sec;
    }
}

/// 退化小节表（单小节 0–86399）：普通 tick、跨桶、小节末端 tick 三种情形
TEST(BarStrategyReplayTest, ResolverMatchesBuilderForDegradedTable) {
    const TradingSessionTable table = dztrader::degraded_trading_session_table();
    expect_resolver_matches_builder(
        table, 60,
        {
            // 18:00:00（tsec 0）→ 桶 0
            BucketCase{.clock = 64800, .price = 100.0, .cum_volume = 1, .cum_turnover = 100.0},
            // 18:00:59（tsec 59）→ 同桶 0
            BucketCase{.clock = 64859, .price = 101.0, .cum_volume = 2, .cum_turnover = 201.0},
            // 18:01:00（tsec 60）→ 桶 60
            BucketCase{.clock = 64860, .price = 102.0, .cum_volume = 3, .cum_turnover = 303.0},
        });
}

/// 多小节表：休息时段丢弃、小节首桶、小节末端 tick 归前一根（ADR 0031 决策 5）
TEST(BarStrategyReplayTest, ResolverMatchesBuilderForMultiSessionTable) {
    const TradingSessionTable table = day_break_session_table();
    expect_resolver_matches_builder(
        table, 60,
        {
            // 09:00:00 → 09:00 桶
            BucketCase{.clock = 9 * 3600, .price = 100.0, .cum_volume = 1, .cum_turnover = 100.0},
            // 09:59:00 → 09:59 桶
            BucketCase{.clock = 9 * 3600 + 59 * 60,
                       .price = 101.0,
                       .cum_volume = 2,
                       .cum_turnover = 201.0},
            // 10:15:00 小节末端 → 归 10:14 桶（不是 10:15 桶）
            BucketCase{.clock = 10 * 3600 + 15 * 60,
                       .price = 102.0,
                       .cum_volume = 3,
                       .cum_turnover = 303.0},
            // 10:20:00 早盘休息中 → out_of_session
            BucketCase{.clock = 10 * 3600 + 20 * 60,
                       .price = 103.0,
                       .cum_volume = 4,
                       .cum_turnover = 404.0},
            // 10:30:00 第二小节开始 → 10:30 桶
            BucketCase{.clock = 10 * 3600 + 30 * 60,
                       .price = 104.0,
                       .cum_volume = 5,
                       .cum_turnover = 505.0},
        });
}

/// Open 竞价窗口表：窗口内 tick 归目标小节首桶，且被 builder 接受（ADR 0032）
TEST(BarStrategyReplayTest, ResolverMatchesBuilderForOpenAuctionTable) {
    const TradingSessionTable table = day_auction_session_table();
    expect_resolver_matches_builder(
        table, 60,
        {
            // 08:59:30 竞价窗口内 → 目标桶 09:00
            BucketCase{
                .clock = 9 * 3600 - 30, .price = 100.0, .cum_volume = 1, .cum_turnover = 100.0},
            // 08:59:59 竞价窗口末尾 → 同桶 09:00
            BucketCase{
                .clock = 9 * 3600 - 1, .price = 101.0, .cum_volume = 2, .cum_turnover = 201.0},
            // 09:00:00 连续交易首笔 → 同桶 09:00
            BucketCase{.clock = 9 * 3600, .price = 102.0, .cum_volume = 3, .cum_turnover = 303.0},
            // 09:01:00 → 09:01 桶
            BucketCase{
                .clock = 9 * 3600 + 60, .price = 103.0, .cum_volume = 4, .cum_turnover = 404.0},
        });
}

/// 竞价窗口命中优先于小节判定：窗口内 tick 归**后一小节**首桶（overlap 表）
TEST(BarStrategyReplayTest, ResolverPrefersAuctionWindowOverSession) {
    const TradingSessionTable table = overlapping_auction_session_table();
    // 11:00:00 落在早盘小节（09:00–11:30）内，同时是午盘（13:00）开盘竞价窗口
    // → 目标桶 = floor(13:00 / 60s) = 13:00，而不是 11:00
    const int32_t tsec = TradingTime::from_clock(11 * 3600);
    const TickBucket bucket = resolve_tick_bucket(tsec, table, 60);
    ASSERT_TRUE(bucket.in_session);
    EXPECT_TRUE(bucket.open_auction);
    EXPECT_EQ(bucket.begin_sec, TSEC_13_00);
}

}  // namespace
