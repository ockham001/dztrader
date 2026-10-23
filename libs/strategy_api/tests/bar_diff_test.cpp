/**
 * @file bar_diff_test.cpp
 * @brief issue #92 差分验收测试 — fast 抽样档
 *
 * 与 `bar_diff_exhaustive_test.cpp`（slow 全量档）配对，分层方式对齐仓库既有先例
 * （`date_test.cpp` fast 抽样 / `date_exhaustive_test.cpp` slow 全量）：用例逻辑只写在
 * `bar_diff_cases.h`，本文件用**最小规模**参数把每条路径各跑一次，进默认 fast 门禁；
 * slow 档用多 seed / 多日 / 高密度做全量覆盖。
 *
 * 硬验收目标：同一 tick 流分别喂入被测 builder 与本仓测试内的旧版参考实现
 * （`legacy_bar_oracle.h`，按 2023-08 前实盘版本的「预计算 18:00 分钟网格 + 字符串时间键」
 * 查表语义独立实现），逐根比较 bar 的
 * `trading_day/begin_sec/last_tick_sec/OHLC/volume/turnover/open_interest/cum_volume/cum_turnover`
 * （另加 `period_sec/is_final`，共 14 个字段），证明新实现没有改变旧版的数据语义。
 *
 * ## 允许差异白名单（显式列出；任何放宽都必须同步改本文件与 issue #92）
 *
 * 1. **新增字段**：旧版 `data::bar` 没有 `period_sec`/`is_final`/`turnover`/`cum_turnover`。
 *    oracle 按本仓 Bar 语义自行推导这四项后**仍然逐字段比较**，因此不构成豁免：
 *    字段级豁免数为 0（`bar_diff_compare.h` 的 `BAR_FIELDS` 覆盖全部 14 个字段，且
 *    `static_assert(sizeof(Bar) == 96)` 会在 Bar 增删字段时变红）。
 * 2. **非法数据守卫**：新 builder 的时间越界 / 交易日倒退 / tsec 倒退 / 累计量额倒退 / 非法价格
 *    守卫旧版没有。差分测试只把 tick 流中 `expect == DiffDrop::None` 的 tick 喂给 oracle
 *    （由 `DiffDrop` 标签显式排除），守卫本身由 #87 的用例覆盖；本文件另断言 `stats()` 的三个
 *    计数器与流的期望计数完全相等，确保被排除的 tick 恰好是被 builder 丢弃的那些。
 * 3. **事件回调时机**：只比较 bar 序列与字段，不比较 open/update/close 的触发时刻
 *    （竞价窗口内零回调、惰性收盘、无 flush 属新语义；见 ADR 0031 决策 5/8）。
 * 4. **旧库 bid/ask/涨跌停列**：不进入核心 Bar（ADR 0031 决策 9），不比较。
 * 5. **竞价窗口可表达范围**：旧版 `callAuction1` 只覆盖单个分钟键（`callAuction1` 分钟 →
 *    后一分钟的网格项），因此只有整分钟长度、目标分钟落在交易小节内、窗口本身不与小节重叠的
 *    Open 窗口可表达；超出范围由 `legacy_table_compatible()` 判否（有用例断言）。
 *    任意长度/多窗口竞价的新语义由 #89 用例覆盖，不在差分 oracle 的可表达域内。
 *    Close 窗口旧版会置 `isCallAuction` 覆盖 O=H=L=C，本仓定为普通 tick（ADR 0031 决策 5），
 *    故 oracle 不建模 Close 窗口映射。
 * 6. **夜盘跨午夜的表达**：旧版以两段墙钟小节（21:00–24:00 + 00:00–02:30）描述夜盘，
 *    oracle 用交易日内秒连续区间等价表达（18:00 锚点下 tsec 单调，语义相同）。
 * 7. **日线不查小节表**：旧版 dispatcher 对日线也先查小节表；ADR 0031 决策 7 明确日线不依赖
 *    小节，故 `LegacyDailyBarOracle` 不做小节过滤。
 */
#include "bar_diff_cases.h"
#include "bar_diff_compare.h"
#include "bar_diff_streams.h"
#include "bar_test_data.h"
#include "legacy_bar_oracle.h"

#include <dztrader/bar.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

namespace {

/// @brief 日盘三段小节表（10:15–10:30 休息落在 1 小时桶内部）
constexpr TradingSessionTable break_table() { return day_break_session_table(); }

/// @brief 日盘单小节表（09:00–15:00）
constexpr TradingSessionTable flat_table() { return day_session_table(); }

/// @brief 日盘单小节日盘表 + 可表达的开盘竞价窗口（08:59:00–08:59:59）
constexpr TradingSessionTable day_auction_table() {
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
constexpr TradingSessionTable commodity_auction_table() {
    TradingSessionTable table = commodity_session_table();
    table.auction_count = 2;
    table.auctions[0] = AuctionWindow{AuctionKind::Open, 0, TSEC_21_00 - 60, TSEC_21_00 - 1};
    table.auctions[1] = AuctionWindow{AuctionKind::Open, 1, TSEC_09_30 - 60, TSEC_09_30 - 1};
    return table;
}

/// @brief fast 档规模：单 seed、单交易日、每小节少量 tick（每条路径各覆盖一次）
constexpr DiffScale FAST_SCALE{1U, 1, 6U};

/// @brief 旧版查表最多可表达的周期档位（1/3/5/15 分钟与 1 小时）
constexpr int32_t LEGACY_PERIODS[] = {60, 180, 300, 900, 3600};

}  // namespace

/// @brief 白名单自身必须可强制：Bar 字段表覆盖全部公开字段（布局变化时 static_assert 变红）
TEST(DifferentialAcceptanceTest, BarFieldTableCoversEveryPublicField) {
    // 字段表的 static_assert 在 bar_diff_compare.h；这里再断言运行期可见的规模，
    // 使「新增字段忘记扩表」这件事在测试报告里也有一行明确产物。
    EXPECT_EQ(BAR_FIELDS.size(), static_cast<std::size_t>(14));
    EXPECT_EQ(sizeof(Bar), BAR_EXPECTED_SIZE);
}

/// @brief 旧版查表只能表达整分钟、单分钟键、目标在小节内的 Open 窗口
TEST(DifferentialAcceptanceTest, LegacyOracleRejectsUnexpressibleAuctionWindows) {
    // 90 秒窗口（09:28:30–09:29:59）跨分钟键，旧版 callAuction1 语义无法表达
    std::string reason;
    EXPECT_FALSE(legacy_table_compatible(commodity_auction_session_table(), &reason)) << reason;
    EXPECT_FALSE(reason.empty());

    // 整分钟、目标分钟落在小节内的窗口可表达
    EXPECT_TRUE(legacy_table_compatible(day_auction_table(), nullptr));
    EXPECT_TRUE(legacy_table_compatible(commodity_auction_table(), nullptr));
    EXPECT_TRUE(legacy_table_compatible(break_table(), nullptr));
}

/// @brief 旧版查表对非法表 fail-fast：小节不整分钟、竞价目标不在小节内、周期非档位
TEST(DifferentialAcceptanceTest, LegacyOracleFailFastOnUnexpressibleTables) {
    // 小节起点不整分钟：旧版按 HH:MM 建表，无法表达
    TradingSessionTable not_minute_aligned{};
    not_minute_aligned.session_count = 1;
    not_minute_aligned.sessions[0] = TradingSession{TSEC_09_00 + 30, TSEC_15_00};
    EXPECT_FALSE(legacy_table_compatible(not_minute_aligned, nullptr));
    EXPECT_THROW((LegacyBarOracle{not_minute_aligned, 60}), std::invalid_argument);

    // Open 竞价窗口的目标分钟不落在任何小节内
    TradingSessionTable target_outside{};
    target_outside.session_count = 1;
    target_outside.sessions[0] = TradingSession{TSEC_09_00, TSEC_15_00};
    target_outside.auction_count = 1;
    target_outside.auctions[0] =
        AuctionWindow{AuctionKind::Open, 0, TSEC_09_00 - 120, TSEC_09_00 - 61};
    EXPECT_FALSE(legacy_table_compatible(target_outside, nullptr));
    EXPECT_THROW((LegacyBarOracle{target_outside, 60}), std::invalid_argument);

    // 周期不是旧版预计算档位（如 120 秒）
    EXPECT_THROW((LegacyBarOracle{flat_table(), 120}), std::invalid_argument);
}

/// @brief oracle 标准网格必须与旧版写死的时刻一致（硬编码期望，不经 TradingTime 换算）
///
/// 若 oracle 与 builder 共用同一套换算函数且在「18:00 锚点」上一起错，差分测试会同时错、
/// 呈现假绿。这里用旧版源码里的墙钟时刻 + 手工算出的交易日内秒双重钉住：
/// 09:00 → 54000、09:30 → 55800、10:00 → 57600、14:59 → 75540（tsec = 墙钟秒 + 21600）。
TEST(DifferentialAcceptanceTest, LegacyStdGridMatchesHandDerivedLegacyTimes) {
    const auto grid = create_legacy_std_grid();
    ASSERT_EQ(grid.size(), static_cast<std::size_t>(1440));

    /// 9:00 / 9:30 / 10:00 / 14:59 四档在 1/3/5/15 分钟与 1 小时下的旧版桶起点（硬编码）
    struct GridCase {
        const char* key;                   ///< 旧版查表键 "HH:MM"
        std::array<int32_t, 5> begin_sec;  ///< 1/3/5/15 分钟 + 1 小时的桶起点（交易日内秒）
    };
    // 3/5/15 分钟的桶边界：09:30 是 3/5/15 分钟的共同边界；1 小时在 09:30 仍属 09:00 桶，
    // 10:00 是新整点桶，14:59 属 14:00–15:00 桶
    const std::array<GridCase, 4> cases = {{
        {"09:00", {54000, 54000, 54000, 54000, 54000}},
        {"09:30", {55800, 55800, 55800, 55800, 54000}},
        {"10:00", {57600, 57600, 57600, 57600, 57600}},
        {"14:59", {75540, 75420, 75300, 74700, 72000}},
    }};
    for (const GridCase& grid_case : cases) {
        SCOPED_TRACE(::testing::Message() << "key=" << grid_case.key);
        const auto found = grid.find(grid_case.key);
        ASSERT_NE(found, grid.end()) << "legacy std grid missing key | key=" << grid_case.key;
        for (std::size_t slot = 0; slot < grid_case.begin_sec.size(); ++slot) {
            EXPECT_EQ(found->second.begin_sec[slot], grid_case.begin_sec[slot])
                << "slot=" << slot << " key=" << grid_case.key;
        }
    }
}

/// @brief 日内干净流差分：fast 抽样覆盖 1/3/5/15/60 分钟各一次
TEST(DifferentialAcceptanceTest, IntradayCleanStreamMatchesLegacyOracle) {
    for (const int32_t period_sec : LEGACY_PERIODS) {
        SCOPED_TRACE(::testing::Message() << "period_sec=" << period_sec);
        run_intraday_clean_case(break_table(), period_sec, FAST_SCALE);
    }
}

/// @brief 日内守卫流差分：乱序 / 坏量 / 非交易时段 / 同秒重复，stats 精确相等
TEST(DifferentialAcceptanceTest, IntradayGuardedStreamMatchesLegacyOracle) {
    constexpr int32_t PERIODS[] = {60, 300, 900};
    for (const int32_t period_sec : PERIODS) {
        SCOPED_TRACE(::testing::Message() << "period_sec=" << period_sec);
        run_intraday_guarded_case(flat_table(), period_sec, FAST_SCALE);
    }
}

/// @brief 竞价差分：Open 窗口 tick 归后一小节首根、O=H=L=C 覆盖语义与旧版一致
TEST(DifferentialAcceptanceTest, IntradayAuctionStreamMatchesLegacyOracle) {
    constexpr int32_t PERIODS[] = {60, 3600};
    for (const int32_t period_sec : PERIODS) {
        SCOPED_TRACE(::testing::Message() << "period_sec=" << period_sec << " table=day_auction");
        run_intraday_auction_case(day_auction_table(), period_sec, FAST_SCALE);

        SCOPED_TRACE(::testing::Message()
                     << "period_sec=" << period_sec << " table=commodity_auction");
        run_intraday_auction_case(commodity_auction_table(), period_sec, FAST_SCALE);
    }
}

/// @brief 边界差分：小节 begin/begin+1s/end-1s/end、同秒重复、休息后恢复
TEST(DifferentialAcceptanceTest, IntradayEdgeStreamMatchesLegacyOracle) {
    constexpr TradingSessionTable TABLES[] = {flat_table(), break_table()};
    for (const TradingSessionTable& table : TABLES) {
        for (const int32_t period_sec : LEGACY_PERIODS) {
            SCOPED_TRACE(::testing::Message()
                         << "period_sec=" << period_sec << " sessions=" << table.session_count);
            run_intraday_edge_case(table, period_sec, 1);
        }
    }
}

/// @brief 日线干净流差分：交易日切换 + 惰性收盘 + 当日累计字段映射
TEST(DifferentialAcceptanceTest, DailyCleanStreamMatchesLegacyOracle) {
    run_daily_clean_case(FAST_SCALE);
}

/// @brief 日线守卫流差分：time 越界 / 倒退 / 累计量额倒退 / 同秒重复
TEST(DifferentialAcceptanceTest, DailyGuardedStreamMatchesLegacyOracle) {
    run_daily_guarded_case(FAST_SCALE);
}

/// @brief 人工混合场景差分：竞价 + 休息 + 日切 + 乱序 + 坏量 + 同秒重复六类场景混合流
TEST(DifferentialAcceptanceTest, HandcraftedMultiScenarioStreamMatchesLegacyOracle) {
    for (const int32_t period_sec : LEGACY_PERIODS) {
        SCOPED_TRACE(::testing::Message() << "period_sec=" << period_sec);
        run_handcrafted_case(commodity_auction_table(), period_sec);
    }
}

/// @brief 人工场景流本身必须真的含六类场景（防止用例退化成只测干净路径）
TEST(DifferentialAcceptanceTest, HandcraftedStreamCoversAllSixScenarioKinds) {
    const DiffStream stream = make_handcrafted_stream();
    EXPECT_GT(stream.out_of_session, 0U) << "missing 非交易时段 ticks";
    EXPECT_GT(stream.out_of_order, 0U) << "missing 乱序 ticks";
    EXPECT_GT(stream.bad_volume, 0U) << "missing 坏量 ticks";

    // 竞价 / 日切 / 同秒重复三类「应被接受」的场景各自必须有对应 tick
    constexpr TradingSessionTable TABLE = commodity_auction_table();
    std::size_t auction_ticks = 0;
    std::size_t same_second_dups = 0;
    std::size_t distinct_days = 0;
    DzDate last_day = 0;
    DzTime previous_clock = 0;
    bool has_previous = false;
    for (const DiffTick& item : stream.ticks) {
        if (item.expect != DiffDrop::None) {
            has_previous = false;
            continue;
        }
        const int32_t tsec = TradingTime::from_clock(item.tick.time);
        for (int32_t i = 0; i < TABLE.auction_count; ++i) {
            const AuctionWindow& window = TABLE.auctions[i];
            if (window.kind == AuctionKind::Open && tsec >= window.begin_sec &&
                tsec <= window.end_sec) {
                ++auction_ticks;
                break;
            }
        }
        if (item.tick.date != last_day) {
            ++distinct_days;
            last_day = item.tick.date;
            has_previous = false;
        }
        if (has_previous && item.tick.time == previous_clock) {
            ++same_second_dups;
        }
        previous_clock = item.tick.time;
        has_previous = true;
    }
    EXPECT_GT(auction_ticks, 0U) << "missing 竞价 ticks";
    EXPECT_GT(same_second_dups, 0U) << "missing 同秒重复 ticks";
    EXPECT_EQ(distinct_days, 2U) << "missing 日切（应跨两个交易日）";
}
