/**
 * @file bar_diff_exhaustive_test.cpp
 * @brief issue #92 差分验收测试 — slow 全量档
 *
 * 与 `bar_diff_test.cpp`（fast 抽样档）配对：用例逻辑全在 `bar_diff_cases.h`，本文件用
 * **多 seed / 多交易日 / 高 tick 密度**做全量覆盖，打 `slow` 标签、不进默认快速门禁。
 * 白名单与语义说明见 `bar_diff_test.cpp` 文件头（唯一真相源，本文件不重复）。
 */
#include "bar_diff_cases.h"
#include "bar_diff_compare.h"
#include "bar_diff_streams.h"
#include "bar_test_data.h"
#include "legacy_bar_oracle.h"

#include <dztrader/bar.h>

#include <array>
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

/// @brief 夜盘 + 日盘三小节表 + 两个可表达的 Open 竞价窗口（20:59、09:29 各一分钟）
constexpr TradingSessionTable commodity_auction_table() {
    TradingSessionTable table = commodity_session_table();
    table.auction_count = 2;
    table.auctions[0] = AuctionWindow{AuctionKind::Open, 0, TSEC_21_00 - 60, TSEC_21_00 - 1};
    table.auctions[1] = AuctionWindow{AuctionKind::Open, 1, TSEC_09_30 - 60, TSEC_09_30 - 1};
    return table;
}

/// @brief slow 全量档的规模矩阵：3 个 seed × 2/3 个交易日 × 较高 tick 密度
constexpr DiffScale FULL_SCALES[] = {
    {1U, 2, 12U},
    {7U, 3, 24U},
    {20231U, 2, 40U},
};

/// @brief 旧版查表最多可表达的周期档位（1/3/5/15 分钟与 1 小时）
constexpr int32_t LEGACY_PERIODS[] = {60, 180, 300, 900, 3600};

}  // namespace

/// @brief 全量：日内干净流 × 多 seed / 多日 / 多周期
TEST(DifferentialExhaustiveTest, IntradayCleanStreamMatchesLegacyOracle) {
    for (const DiffScale& scale : FULL_SCALES) {
        for (const int32_t period_sec : LEGACY_PERIODS) {
            SCOPED_TRACE(::testing::Message()
                         << "seed=" << scale.seed << " days=" << scale.trading_days
                         << " ticks=" << scale.ticks_per_slot << " period_sec=" << period_sec);
            run_intraday_clean_case(break_table(), period_sec, scale);
        }
    }
}

/// @brief 全量：日内守卫流 × 多 seed / 多日 / 多周期（三类丢弃计数精确相等）
TEST(DifferentialExhaustiveTest, IntradayGuardedStreamMatchesLegacyOracle) {
    for (const DiffScale& scale : FULL_SCALES) {
        for (const int32_t period_sec : LEGACY_PERIODS) {
            SCOPED_TRACE(::testing::Message()
                         << "seed=" << scale.seed << " days=" << scale.trading_days
                         << " ticks=" << scale.ticks_per_slot << " period_sec=" << period_sec);
            run_intraday_guarded_case(flat_table(), period_sec, scale);
        }
    }
}

/// @brief 全量：竞价流 × 两张竞价表 × 多 seed / 多日 / 多周期
TEST(DifferentialExhaustiveTest, IntradayAuctionStreamMatchesLegacyOracle) {
    constexpr TradingSessionTable TABLES[] = {day_auction_table(), commodity_auction_table()};
    for (const TradingSessionTable& table : TABLES) {
        for (const DiffScale& scale : FULL_SCALES) {
            for (const int32_t period_sec : LEGACY_PERIODS) {
                SCOPED_TRACE(::testing::Message()
                             << "sessions=" << table.session_count
                             << " auctions=" << table.auction_count << " seed=" << scale.seed
                             << " days=" << scale.trading_days << " ticks=" << scale.ticks_per_slot
                             << " period_sec=" << period_sec);
                run_intraday_auction_case(table, period_sec, scale);
            }
        }
    }
}

/// @brief 全量：边界流 × 两张小节表 × 多日 / 多周期
TEST(DifferentialExhaustiveTest, IntradayEdgeStreamMatchesLegacyOracle) {
    constexpr TradingSessionTable TABLES[] = {flat_table(), break_table()};
    for (const TradingSessionTable& table : TABLES) {
        for (const int32_t trading_days : {2, 3}) {
            for (const int32_t period_sec : LEGACY_PERIODS) {
                SCOPED_TRACE(::testing::Message() << "sessions=" << table.session_count << " days="
                                                  << trading_days << " period_sec=" << period_sec);
                run_intraday_edge_case(table, period_sec, trading_days);
            }
        }
    }
}

/// @brief 全量：日线干净流 × 多 seed / 多日 / 高密度
TEST(DifferentialExhaustiveTest, DailyCleanStreamMatchesLegacyOracle) {
    for (const DiffScale& scale : FULL_SCALES) {
        SCOPED_TRACE(::testing::Message() << "seed=" << scale.seed << " days=" << scale.trading_days
                                          << " ticks=" << scale.ticks_per_slot);
        run_daily_clean_case(scale);
    }
}

/// @brief 全量：日线守卫流 × 多 seed / 多日 / 高密度
TEST(DifferentialExhaustiveTest, DailyGuardedStreamMatchesLegacyOracle) {
    for (const DiffScale& scale : FULL_SCALES) {
        SCOPED_TRACE(::testing::Message() << "seed=" << scale.seed << " days=" << scale.trading_days
                                          << " ticks=" << scale.ticks_per_slot);
        run_daily_guarded_case(scale);
    }
}

/// @brief 全量：人工混合场景 × 多周期
TEST(DifferentialExhaustiveTest, HandcraftedMultiScenarioStreamMatchesLegacyOracle) {
    for (const int32_t period_sec : LEGACY_PERIODS) {
        SCOPED_TRACE(::testing::Message() << "period_sec=" << period_sec);
        run_handcrafted_case(commodity_auction_table(), period_sec);
    }
}
