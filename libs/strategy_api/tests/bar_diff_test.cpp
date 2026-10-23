/**
 * @file bar_diff_test.cpp
 * @brief issue #92：旧版查表语义 oracle 的差分验收测试（slow 档）
 *
 * 硬验收目标：同一 tick 流分别喂入被测 builder 与本仓测试内的旧版参考实现
 * （legacy_bar_oracle.h，按 2023-08 前实盘版本的「预计算 18:00 分钟网格 + 字符串时间键」
 * 查表语义独立实现），逐根比较 bar 的
 * `trading_day/begin_sec/last_tick_sec/OHLC/volume/turnover/open_interest/cum_volume/cum_turnover`，
 * 证明新实现没有改变旧版的数据语义。
 *
 * ## 允许差异白名单（显式列出；任何放宽都必须同步改本文件）
 *
 * 1. **新增字段**：旧版 `data::bar` 没有 `period_sec`/`is_final`/`turnover`/`cum_turnover`。
 *    oracle 按本仓 Bar 语义自行推导这四项后**仍然逐字段比较**，因此不构成豁免；
 *    字段级豁免数为 0（见 `DifferentialAcceptanceTest.WhitelistHasNoBarFieldExemptions`）。
 * 2. **非法数据守卫**：新 builder 的时间越界 / 交易日倒退 / tsec 倒退 / 累计量额倒退 / 非法价格
 *    守卫旧版没有。差分测试只把 tick 流中 `expect == DiffDrop::None` 的 tick 喂给 oracle
 *    （由 `DiffDrop` 标签显式排除），守卫本身由 #87 的用例覆盖；本文件另断言 `stats()` 的三个
 *    计数器与流的期望计数完全相等，确保被排除的 tick 恰好是被 builder 丢弃的那些。
 * 3. **事件回调时机**：只比较 bar 序列与字段，不比较 open/update/close 的触发时刻
 *    （竞价窗口内零回调、惰性收盘、无 flush 属新语义；见 ADR 0031 决策 5/8）。
 * 4. **旧库 bid/ask/涨跌停列**：不进入核心 Bar（ADR 0031 决策 9），不比较。
 * 5. **竞价窗口可表达范围**：旧版 `callAuction1` 只覆盖单个分钟键（`callAuction1` 分钟 →
 *    后一分钟的网格项），因此长度为整分钟、且目标分钟落在交易小节内、窗口本身不与小节重叠的
 *    Open 窗口才可表达；超出一律 `legacy_table_compatible()` 判否（见
 *    `DifferentialAcceptanceTest.LegacyOracleRejectsUnexpressibleAuctionWindows`）。
 *    Close 窗口旧版会置 `isCallAuction` 覆盖 O=H=L=C，本仓定为普通 tick（ADR 0031 决策 5），
 *    故 oracle 不建模 Close 窗口映射。
 * 6. **夜盘跨午夜的表达**：旧版以两段墙钟小节（21:00–24:00 + 00:00–02:30）描述夜盘，
 *    oracle 用交易日内秒连续区间等价表达（18:00 锚点下 tsec 单调，语义相同）。
 * 7. **日线不查小节表**：旧版 dispatcher 对日线也先查小节表；ADR 0031 决策 7 明确日线不依赖
 *    小节，故 `LegacyDailyBarOracle` 不做小节过滤。
 */
#include "bar_diff_streams.h"
#include "bar_test_data.h"
#include "legacy_bar_oracle.h"

#include <dztrader/bar.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

namespace {

/// @brief 差分比较的 Bar 字段名（顺序即逐字段比较顺序；新增 Bar 字段必须同步本表）
constexpr std::array<const char*, 14> COMPARED_BAR_FIELDS = {
    "trading_day",   "begin_sec",  "last_tick_sec", "period_sec", "open",
    "high",          "low",        "close",         "volume",     "turnover",
    "open_interest", "cum_volume", "cum_turnover",  "is_final"};

/**
 * @brief 字段级豁免数量：Bar 的 14 个公开字段全部逐字段比较，不允许任何豁免
 *
 * 放宽此值等于放宽数据语义一致性验收，必须同时改 issue 与 ADR。
 */
constexpr int DIFF_WHITELIST_BAR_FIELD_EXEMPTIONS = 0;

/// @brief 单根 bar 的一个不一致字段
struct FieldMismatch {
    std::string field;     ///< 字段名
    std::string expected;  ///< oracle 期望值（字符串化）
    std::string actual;    ///< builder 实际值（字符串化）
};

/// @brief bar 序列比较结果（首个不一致即停止）
struct BarSequenceDiff {
    bool matched = true;                    ///< true = 序列与全部字段一致
    std::size_t index = 0;                  ///< 首个不一致的 bar 下标
    std::string reason;                     ///< 不一致原因（数量不等 / 字段不等）
    std::vector<FieldMismatch> mismatches;  ///< 该 bar 上全部不一致字段
};

/// @brief 把 double 以可读且可区分的形式字符串化
std::string format_double(double value) {
    std::ostringstream stream;
    stream.precision(17);
    stream << value;
    return stream.str();
}

/// @brief 比较单根 bar 的全部公开字段，收集不一致字段
std::vector<FieldMismatch> compare_bar_fields(const Bar& actual, const Bar& expected) {
    std::vector<FieldMismatch> mismatches;
    const auto check_int = [&mismatches](const char* field, int64_t actual_value,
                                         int64_t expected_value) {
        if (actual_value != expected_value) {
            mismatches.push_back(
                {field, std::to_string(expected_value), std::to_string(actual_value)});
        }
    };
    const auto check_double = [&mismatches](const char* field, double actual_value,
                                            double expected_value) {
        if (actual_value != expected_value) {
            mismatches.push_back(
                {field, format_double(expected_value), format_double(actual_value)});
        }
    };
    check_int("trading_day", actual.trading_day, expected.trading_day);
    check_int("begin_sec", actual.begin_sec, expected.begin_sec);
    check_int("last_tick_sec", actual.last_tick_sec, expected.last_tick_sec);
    check_int("period_sec", actual.period_sec, expected.period_sec);
    check_double("open", actual.open, expected.open);
    check_double("high", actual.high, expected.high);
    check_double("low", actual.low, expected.low);
    check_double("close", actual.close, expected.close);
    check_int("volume", actual.volume, expected.volume);
    check_double("turnover", actual.turnover, expected.turnover);
    check_int("open_interest", actual.open_interest, expected.open_interest);
    check_int("cum_volume", actual.cum_volume, expected.cum_volume);
    check_double("cum_turnover", actual.cum_turnover, expected.cum_turnover);
    check_int("is_final", static_cast<int64_t>(actual.is_final),
              static_cast<int64_t>(expected.is_final));
    return mismatches;
}

/**
 * @brief 逐根比较 bar 序列，定位首个不一致的 bar 与字段
 *
 * 失败信息包含 bar 下标、标识 (trading_day, begin_sec)、全部不一致字段的期望/实际值，
 * 满足 issue #92「失败信息能定位首个不一致 bar 与字段」。
 *
 * @param actual 被测 builder 产出的 bar 序列
 * @param expected oracle 产出的 bar 序列
 * @param scenario 场景描述（拼进失败信息，便于定位是哪条流）
 * @return 比较结果
 */
BarSequenceDiff diff_bar_sequences(const std::vector<Bar>& actual,
                                   const std::vector<Bar>& expected,
                                   const std::string& scenario) {
    BarSequenceDiff result;
    if (actual.size() != expected.size()) {
        result.matched = false;
        std::size_t common = actual.size() < expected.size() ? actual.size() : expected.size();
        // 数量不等时仍先定位首根字段差异（若存在），否则报告数量差异
        for (std::size_t i = 0; i < common; ++i) {
            const std::vector<FieldMismatch> mismatches =
                compare_bar_fields(actual[i], expected[i]);
            if (!mismatches.empty()) {
                result.index = i;
                result.mismatches = mismatches;
                result.reason =
                    "bar count differs and first field mismatch found before end | scenario=" +
                    scenario + " actual_count=" + std::to_string(actual.size()) +
                    " expected_count=" + std::to_string(expected.size());
                return result;
            }
        }
        result.index = common;
        result.reason = "bar count differs | scenario=" + scenario +
                        " actual_count=" + std::to_string(actual.size()) +
                        " expected_count=" + std::to_string(expected.size());
        return result;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const std::vector<FieldMismatch> mismatches = compare_bar_fields(actual[i], expected[i]);
        if (!mismatches.empty()) {
            result.matched = false;
            result.index = i;
            result.mismatches = mismatches;
            result.reason = "bar field mismatch | scenario=" + scenario;
            return result;
        }
    }
    return result;
}

/// @brief 把比较结果拼成可直接贴进断言失败信息的文本
std::string describe_diff(const BarSequenceDiff& diff,
                          const std::vector<Bar>& actual,
                          const std::vector<Bar>& expected) {
    std::ostringstream stream;
    stream << diff.reason << " | first_mismatch_bar=" << diff.index;
    if (diff.index < actual.size()) {
        stream << " actual_id=(" << actual[diff.index].trading_day << ","
               << actual[diff.index].begin_sec << ")";
    }
    if (diff.index < expected.size()) {
        stream << " expected_id=(" << expected[diff.index].trading_day << ","
               << expected[diff.index].begin_sec << ")";
    }
    for (const FieldMismatch& mismatch : diff.mismatches) {
        stream << " | field=" << mismatch.field << " expected=" << mismatch.expected
               << " actual=" << mismatch.actual;
    }
    return stream.str();
}

/// @brief 收集 builder 的完整 bar 序列：已收 bar 按 close 事件顺序，末尾补当前形成中 bar
std::vector<Bar> builder_bars(const RecordingSink& sink,
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

/// @brief 收集日线 builder 的完整 bar 序列（同上，日线版）
std::vector<Bar> builder_bars(const RecordingSink& sink,
                              const DailyBarBuilder<RecordingSink>& builder) {
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

/// @brief 断言 builder 的丢弃计数与 tick 流的期望计数完全一致
void expect_stats_match(const BarBuilderStats& stats,
                        const DiffStream& stream,
                        const std::string& scenario) {
    EXPECT_EQ(stats.out_of_session, stream.out_of_session)
        << "out_of_session mismatch | scenario=" << scenario;
    EXPECT_EQ(stats.out_of_order, stream.out_of_order)
        << "out_of_order mismatch | scenario=" << scenario;
    EXPECT_EQ(stats.bad_volume, stream.bad_volume) << "bad_volume mismatch | scenario=" << scenario;
}

/// @brief 差分断言：bar 序列逐根逐字段一致（失败时给出首个不一致 bar 与字段）
void expect_sequences_equal(const std::vector<Bar>& actual,
                            const std::vector<Bar>& expected,
                            const std::string& scenario) {
    const BarSequenceDiff diff = diff_bar_sequences(actual, expected, scenario);
    ASSERT_TRUE(diff.matched) << describe_diff(diff, actual, expected);
}

/// @brief 日盘三段小节表 + 可被旧版查表语义表达的开盘竞价窗口（08:59:00–08:59:59）
///
/// 旧版 `callAuction1` 只覆盖单个分钟键，因此这里只用整分钟长度、且目标分钟（09:00）落在
/// 交易小节内的窗口；超出可表达范围的窗口由 `commodity_auction_session_table()` 单独断言拒绝。
constexpr TradingSessionTable legacy_day_auction_table() {
    TradingSessionTable table = day_session_table();
    table.auction_count = 1;
    table.auctions[0] = AuctionWindow{AuctionKind::Open, 0, TSEC_09_00 - 60, TSEC_09_00 - 1};
    return table;
}

/**
 * @brief 夜盘 + 日盘三小节表 + 两个可表达的 Open 竞价窗口（20:59、09:29 各一分钟）
 *
 * 夜盘跨午夜（21:00–02:30）在旧版属两段墙钟小节，oracle 用交易日内秒连续区间等价表达
 * （白名单第 6 条）。两个窗口都整分钟、目标分钟（21:00 / 09:30）落在对应小节内。
 */
constexpr TradingSessionTable legacy_commodity_auction_table() {
    TradingSessionTable table = commodity_session_table();
    table.auction_count = 2;
    table.auctions[0] = AuctionWindow{AuctionKind::Open, 0, TSEC_21_00 - 60, TSEC_21_00 - 1};
    table.auctions[1] = AuctionWindow{AuctionKind::Open, 1, TSEC_09_30 - 60, TSEC_09_30 - 1};
    return table;
}

/// @brief 日盘三段小节表（10:15–10:30 休息落在 1 小时桶内部）的共享用例表
constexpr TradingSessionTable break_table() { return day_break_session_table(); }

/// @brief 日盘单小节表（09:00–15:00）
constexpr TradingSessionTable flat_table() { return day_session_table(); }

}  // namespace

/// @brief 白名单自身必须显式且不可静默放宽：Bar 字段级零豁免、比较字段数固定
TEST(DifferentialAcceptanceTest, WhitelistHasNoBarFieldExemptions) {
    EXPECT_EQ(DIFF_WHITELIST_BAR_FIELD_EXEMPTIONS, 0)
        << "差异化验收不允许字段级豁免；新增允许差异必须改 issue #92 与 ADR 0031";
    EXPECT_EQ(COMPARED_BAR_FIELDS.size(), static_cast<std::size_t>(14))
        << "Bar 公开字段数量变化时必须同步差分比较与白名单说明";
}

/// @brief 旧版查表只能表达整分钟、单分钟键、目标在小节内的 Open 窗口
TEST(DifferentialAcceptanceTest, LegacyOracleRejectsUnexpressibleAuctionWindows) {
    std::string reason;
    // 90 秒窗口（09:28:30–09:29:59）跨分钟键，旧版 callAuction1 语义无法表达
    EXPECT_FALSE(legacy_table_compatible(commodity_auction_session_table(), &reason)) << reason;
    EXPECT_FALSE(reason.empty());

    // 整分钟、目标分钟落在小节内的窗口可表达
    EXPECT_TRUE(legacy_table_compatible(legacy_day_auction_table(), nullptr));
    EXPECT_TRUE(legacy_table_compatible(legacy_commodity_auction_table(), nullptr));
    EXPECT_TRUE(legacy_table_compatible(break_table(), nullptr));
}

/// @brief 日内周期差分：固定 seed 随机干净流 × 1/3/5/15/60 分钟 × 多日，逐根逐字段一致
TEST(DifferentialAcceptanceTest, IntradayRandomCleanStreamMatchesLegacyOracle) {
    constexpr TradingSessionTable TABLE = break_table();
    constexpr int32_t PERIODS[] = {60, 180, 300, 900, 3600};
    constexpr uint32_t SEEDS[] = {1U, 7U, 20231U};
    constexpr int32_t TRADING_DAYS = 2;
    constexpr std::size_t TICKS_PER_SESSION = 40;

    for (const uint32_t seed : SEEDS) {
        const DiffStream stream =
            make_random_clean_stream(seed, TABLE, TRADING_DAYS, TICKS_PER_SESSION);
        ASSERT_EQ(stream.out_of_session, 0U)
            << "clean stream must have no dropped tick | seed=" << seed;
        ASSERT_EQ(stream.out_of_order, 0U)
            << "clean stream must have no dropped tick | seed=" << seed;
        ASSERT_EQ(stream.bad_volume, 0U)
            << "clean stream must have no dropped tick | seed=" << seed;
        const std::vector<DzTick> ticks = stream.accepted_ticks();
        ASSERT_FALSE(ticks.empty());

        for (const int32_t period_sec : PERIODS) {
            const std::string scenario = "random_clean seed=" + std::to_string(seed) +
                                         " period_sec=" + std::to_string(period_sec);
            LegacyBarOracle oracle(TABLE, period_sec);
            RecordingSink sink;
            IntradayBarBuilder<RecordingSink> builder(TABLE, period_sec, sink);
            for (const DzTick& tick : ticks) {
                oracle.on_tick(tick);
                builder.on_tick(tick);
            }
            expect_stats_match(builder.stats(), stream, scenario);
            expect_sequences_equal(builder_bars(sink, builder), oracle.bars(), scenario);
        }
    }
}

/// @brief 守卫差分：注入乱序/坏量/非交易时段/同秒重复后，stats 精确相等且 bar 序列仍一致
TEST(DifferentialAcceptanceTest, IntradayGuardedStreamMatchesLegacyOracle) {
    constexpr TradingSessionTable TABLE = flat_table();
    constexpr int32_t PERIODS[] = {60, 300, 900};
    constexpr uint32_t SEEDS[] = {11U, 99U};
    constexpr int32_t TRADING_DAYS = 2;
    constexpr std::size_t TICKS_PER_SESSION = 30;

    for (const uint32_t seed : SEEDS) {
        const DiffStream stream =
            make_random_guarded_stream(seed, TABLE, TRADING_DAYS, TICKS_PER_SESSION);
        // 守卫流的价值在于三种丢弃都真实出现；缺一说明生成器退化，断言并继续
        EXPECT_GT(stream.out_of_session, 0U)
            << "guarded stream lost out_of_session cases | seed=" << seed;
        EXPECT_GT(stream.out_of_order, 0U)
            << "guarded stream lost out_of_order cases | seed=" << seed;
        EXPECT_GT(stream.bad_volume, 0U) << "guarded stream lost bad_volume cases | seed=" << seed;

        for (const int32_t period_sec : PERIODS) {
            const std::string scenario = "random_guarded seed=" + std::to_string(seed) +
                                         " period_sec=" + std::to_string(period_sec);
            LegacyBarOracle oracle(TABLE, period_sec);
            RecordingSink sink;
            IntradayBarBuilder<RecordingSink> builder(TABLE, period_sec, sink);
            for (const DiffTick& tick : stream.ticks) {
                if (tick.expect == DiffDrop::None) {
                    oracle.on_tick(tick.tick);
                }
                builder.on_tick(tick.tick);
            }
            expect_stats_match(builder.stats(), stream, scenario);
            expect_sequences_equal(builder_bars(sink, builder), oracle.bars(), scenario);
        }
    }
}

/// @brief 竞价差分：Open 窗口 tick 归后一小节首根、O=H=L=C 覆盖语义与旧版一致
TEST(DifferentialAcceptanceTest, IntradayAuctionStreamMatchesLegacyOracle) {
    constexpr int32_t PERIODS[] = {60, 300, 3600};
    constexpr uint32_t SEEDS[] = {5U, 42U};
    constexpr int32_t TRADING_DAYS = 2;
    constexpr std::size_t TICKS_PER_SESSION = 25;

    constexpr TradingSessionTable TABLES[] = {legacy_day_auction_table(),
                                              legacy_commodity_auction_table()};
    for (const TradingSessionTable& table : TABLES) {
        for (const uint32_t seed : SEEDS) {
            const DiffStream stream =
                make_auction_clean_stream(seed, table, TRADING_DAYS, TICKS_PER_SESSION);
            ASSERT_EQ(stream.out_of_session, 0U);
            ASSERT_EQ(stream.out_of_order, 0U);
            ASSERT_EQ(stream.bad_volume, 0U);
            const std::vector<DzTick> ticks = stream.accepted_ticks();
            ASSERT_FALSE(ticks.empty());

            for (const int32_t period_sec : PERIODS) {
                const std::string scenario =
                    "auction sessions=" + std::to_string(table.session_count) +
                    " auctions=" + std::to_string(table.auction_count) +
                    " seed=" + std::to_string(seed) + " period_sec=" + std::to_string(period_sec);
                LegacyBarOracle oracle(table, period_sec);
                RecordingSink sink;
                IntradayBarBuilder<RecordingSink> builder(table, period_sec, sink);
                for (const DzTick& tick : ticks) {
                    oracle.on_tick(tick);
                    builder.on_tick(tick);
                }
                expect_stats_match(builder.stats(), stream, scenario);
                expect_sequences_equal(builder_bars(sink, builder), oracle.bars(), scenario);
            }
        }
    }
}

/// @brief 边界差分：小节 begin/begin+1s/end-1s/end、同秒重复、休息后恢复，逐根一致
TEST(DifferentialAcceptanceTest, IntradayEdgeStreamMatchesLegacyOracle) {
    constexpr int32_t PERIODS[] = {60, 180, 300, 900, 3600};
    constexpr TradingSessionTable TABLES[] = {flat_table(), break_table()};
    constexpr int32_t TRADING_DAYS = 2;

    for (const TradingSessionTable& table : TABLES) {
        const DiffStream stream = make_edge_clean_stream(table, TRADING_DAYS);
        ASSERT_GT(stream.ticks.size(), 0U);
        const std::vector<DzTick> ticks = stream.accepted_ticks();
        for (const int32_t period_sec : PERIODS) {
            const std::string scenario = "edge sessions=" + std::to_string(table.session_count) +
                                         " period_sec=" + std::to_string(period_sec);
            LegacyBarOracle oracle(table, period_sec);
            RecordingSink sink;
            IntradayBarBuilder<RecordingSink> builder(table, period_sec, sink);
            for (const DzTick& tick : ticks) {
                oracle.on_tick(tick);
                builder.on_tick(tick);
            }
            expect_stats_match(builder.stats(), stream, scenario);
            expect_sequences_equal(builder_bars(sink, builder), oracle.bars(), scenario);
        }
    }
}

/// @brief 日线差分：固定 seed 随机流，逐根逐字段一致（含交易日切换与惰性收盘）
TEST(DifferentialAcceptanceTest, DailyRandomStreamMatchesLegacyOracle) {
    constexpr uint32_t SEEDS[] = {3U, 17U, 202321U};
    constexpr int32_t TRADING_DAYS = 4;
    constexpr std::size_t TICKS_PER_DAY = 30;

    for (const uint32_t seed : SEEDS) {
        const DiffStream stream = make_daily_random_stream(seed, TRADING_DAYS, TICKS_PER_DAY);
        ASSERT_EQ(stream.out_of_session, 0U);
        ASSERT_EQ(stream.out_of_order, 0U);
        ASSERT_EQ(stream.bad_volume, 0U);
        const std::vector<DzTick> ticks = stream.accepted_ticks();
        ASSERT_FALSE(ticks.empty());

        const std::string scenario = "daily_random seed=" + std::to_string(seed);
        LegacyDailyBarOracle oracle;
        RecordingSink sink;
        DailyBarBuilder<RecordingSink> builder(sink);
        for (const DzTick& tick : ticks) {
            oracle.on_tick(tick);
            builder.on_tick(tick);
        }
        expect_stats_match(builder.stats(), stream, scenario);
        expect_sequences_equal(builder_bars(sink, builder), oracle.bars(), scenario);
    }
}

/// @brief 日线守卫差分：时间越界/倒退、累计量额倒退、同秒重复后 stats 精确相等且日线一致
TEST(DifferentialAcceptanceTest, DailyGuardedStreamMatchesLegacyOracle) {
    constexpr uint32_t SEEDS[] = {8U, 123U};
    constexpr int32_t TRADING_DAYS = 3;
    constexpr std::size_t TICKS_PER_DAY = 25;

    for (const uint32_t seed : SEEDS) {
        const DiffStream stream = make_daily_guarded_stream(seed, TRADING_DAYS, TICKS_PER_DAY);
        EXPECT_GT(stream.out_of_session, 0U)
            << "guarded daily stream lost out_of_session cases | seed=" << seed;
        EXPECT_GT(stream.out_of_order, 0U)
            << "guarded daily stream lost out_of_order cases | seed=" << seed;
        EXPECT_GT(stream.bad_volume, 0U)
            << "guarded daily stream lost bad_volume cases | seed=" << seed;

        const std::string scenario = "daily_guarded seed=" + std::to_string(seed);
        LegacyDailyBarOracle oracle;
        RecordingSink sink;
        DailyBarBuilder<RecordingSink> builder(sink);
        for (const DiffTick& tick : stream.ticks) {
            if (tick.expect == DiffDrop::None) {
                oracle.on_tick(tick.tick);
            }
            builder.on_tick(tick.tick);
        }
        expect_stats_match(builder.stats(), stream, scenario);
        expect_sequences_equal(builder_bars(sink, builder), oracle.bars(), scenario);
    }
}

/**
 * @brief 人工场景：竞价 + 休息 + 日切 + 乱序 + 坏量 + 同秒重复混合流的差分一致性
 *
 * 与上面的纯随机流互补：手工构造一条「一个交易日包含夜盘竞价、夜盘、日盘竞价、日盘休息、
 * 收盘、次日夜盘」的完整流，逐根核对，覆盖随机流不易命中的组合边界。
 */
TEST(DifferentialAcceptanceTest, HandcraftedMultiScenarioStreamMatchesLegacyOracle) {
    constexpr TradingSessionTable TABLE = legacy_commodity_auction_table();
    constexpr DzDate DAY1 = TRADING_DAY;
    constexpr DzDate DAY2 = TRADING_DAY + 1;
    /// 人工流的一笔 tick + 期望归宿（expect != None 的 tick 不喂给 oracle）
    struct HandTick {
        DzTime clock;                      ///< 墙钟秒
        DzDate day;                        ///< 交易日
        double price;                      ///< last_price
        DzVolume volume;                   ///< 当日累计成交量
        double turnover;                   ///< 当日累计成交额
        DzLargeVolume open_interest;       ///< 持仓量
        DiffDrop expect = DiffDrop::None;  ///< 期望归宿
    };
    // 一个完整交易日：夜盘竞价 → 夜盘（含同秒重复、夜盘末端）→ 早盘竞价 → 日盘（含休息）
    // → 收盘 → 次日夜盘；再补一笔坏量与一笔非交易时段 tick。
    const std::array<HandTick, 18> hand_ticks = {{
        // DAY1 夜盘开盘竞价（20:59:10 / 20:59:40，窗口 20:59:00–20:59:59）
        {20 * 3600 + 59 * 60 + 10, DAY1, 100.0, 10, 1000.0, 5, DiffDrop::None},
        {20 * 3600 + 59 * 60 + 40, DAY1, 101.0, 20, 2010.0, 6, DiffDrop::None},
        // DAY1 夜盘：21:00、21:00 同秒重复、21:04:59、22:00、02:30（夜盘末端）
        {21 * 3600, DAY1, 102.0, 30, 3030.0, 7, DiffDrop::None},
        {21 * 3600, DAY1, 103.0, 30, 3030.0, 7, DiffDrop::None},
        {21 * 3600 + 4 * 60 + 59, DAY1, 104.0, 40, 4040.0, 8, DiffDrop::None},
        {22 * 3600, DAY1, 105.0, 50, 5050.0, 9, DiffDrop::None},
        {2 * 3600 + 30 * 60, DAY1, 106.0, 60, 6060.0, 10, DiffDrop::None},
        // DAY1 早盘竞价（09:29:20）与日盘（09:30、10:00、10:15 休息、13:00、15:00 收盘）
        {9 * 3600 + 29 * 60 + 20, DAY1, 107.0, 70, 7070.0, 11, DiffDrop::None},
        {9 * 3600 + 30 * 60, DAY1, 108.0, 80, 8080.0, 12, DiffDrop::None},
        {10 * 3600, DAY1, 109.0, 90, 9090.0, 13, DiffDrop::None},
        {10 * 3600 + 15 * 60, DAY1, 110.0, 100, 10099.0, 14, DiffDrop::None},
        {13 * 3600, DAY1, 111.0, 110, 11110.0, 15, DiffDrop::None},
        {15 * 3600, DAY1, 112.0, 120, 12120.0, 16, DiffDrop::None},
        // DAY2 夜盘：新交易日首笔（21:00）切日；随后同秒重复与 23:00
        {21 * 3600, DAY2, 200.0, 10, 2000.0, 20, DiffDrop::None},
        {21 * 3600, DAY2, 201.0, 10, 2000.0, 20, DiffDrop::None},
        {23 * 3600, DAY2, 202.0, 20, 4010.0, 21, DiffDrop::None},
        // 坏量：同一交易日内累计成交量倒退（丢弃并计 bad_volume）
        {23 * 3600 + 60, DAY2, 203.0, 15, 5000.0, 22, DiffDrop::BadVolume},
        // 非交易时段：12:00 落在 11:30–13:00 休息时段（丢弃并计 out_of_session）
        {12 * 3600, DAY2, 204.0, 25, 6000.0, 23, DiffDrop::OutOfSession},
    }};

    DiffStream stream;
    for (const HandTick& item : hand_ticks) {
        DiffTick tick;
        tick.tick = make_tick(item.clock, item.price, item.volume, item.turnover,
                              item.open_interest, item.day);
        tick.expect = item.expect;
        if (tick.expect == DiffDrop::BadVolume) {
            ++stream.bad_volume;
        } else if (tick.expect == DiffDrop::OutOfSession) {
            ++stream.out_of_session;
        }
        stream.ticks.push_back(tick);
    }

    constexpr int32_t PERIODS[] = {60, 180, 300, 900, 3600};
    for (const int32_t period_sec : PERIODS) {
        const std::string scenario = "handcrafted period_sec=" + std::to_string(period_sec);
        LegacyBarOracle oracle(TABLE, period_sec);
        RecordingSink sink;
        IntradayBarBuilder<RecordingSink> builder(TABLE, period_sec, sink);
        for (const DiffTick& tick : stream.ticks) {
            if (tick.expect == DiffDrop::None) {
                oracle.on_tick(tick.tick);
            }
            builder.on_tick(tick.tick);
        }
        expect_stats_match(builder.stats(), stream, scenario);
        expect_sequences_equal(builder_bars(sink, builder), oracle.bars(), scenario);
    }
}
