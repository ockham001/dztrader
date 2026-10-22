#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

namespace {

// 交易小节表构造辅助（tsec 坐标）。表容量小，测试内用 constexpr 函数拼装。
constexpr TradingSessionTable single_session(int32_t begin_sec, int32_t end_sec) {
    TradingSessionTable table{};
    table.session_count = 1;
    table.sessions[0] = TradingSession{begin_sec, end_sec};
    return table;
}

constexpr TradingSessionTable two_sessions(TradingSession first, TradingSession second) {
    TradingSessionTable table{};
    table.session_count = 2;
    table.sessions[0] = first;
    table.sessions[1] = second;
    return table;
}

constexpr TradingSessionTable with_auction(TradingSessionTable table, AuctionWindow auction) {
    table.auctions[table.auction_count] = auction;
    ++table.auction_count;
    return table;
}

/// @brief 一天的秒数（交易日内秒的有效区间上界 + 1）
constexpr int32_t SECS_PER_DAY = 86400;

constexpr TradingSessionTable DAY_SESSION = day_session_table();
constexpr TradingSessionTable DAY_AND_NIGHT =
    two_sessions(TradingSession{TSEC_21_00, TSEC_02_30}, TradingSession{TSEC_09_00, TSEC_15_00});

// 表校验是 constexpr 的（调用方可静态构造并校验）；合法与非法各取一个编译期例子。
static_assert(DAY_SESSION.valid());
static_assert(!TradingSessionTable{}.valid());
static_assert(!single_session(200, 100).valid());

// 交易日内秒换算是 constexpr 的；18:00 / 00:00 / 23:59:59 边界在此定型。
static_assert(TradingTime::ANCHOR_CLOCK_SEC == 64800);
static_assert(TradingTime::from_clock(64800) == 0);  // 18:00 是交易日起点
static_assert(TradingTime::to_clock(0) == 64800);
static_assert(TradingTime::from_clock(0) == 21600);  // 00:00
static_assert(TradingTime::to_clock(21600) == 0);
static_assert(TradingTime::from_clock(86399) == 21599);  // 23:59:59
static_assert(TradingTime::to_clock(21599) == 86399);

// 公开头只依赖标准库与 SDK 公开 POD：Bar 是定长 POD。
static_assert(std::is_trivially_copyable_v<Bar>);
static_assert(std::is_standard_layout_v<Bar>);

}  // namespace

TEST(TradingTimeTest, BoundariesRoundTrip) {
    // 18:00：交易日切换点
    EXPECT_EQ(TradingTime::from_clock(18 * 3600), 0);
    EXPECT_EQ(TradingTime::to_clock(0), 18 * 3600);

    // 00:00：夜盘跨过自然日午夜
    EXPECT_EQ(TradingTime::from_clock(0), 6 * 3600);
    EXPECT_EQ(TradingTime::to_clock(6 * 3600), 0);

    // 23:59:59：交易日最后一个墙钟秒
    EXPECT_EQ(TradingTime::from_clock(86399), 21599);
    EXPECT_EQ(TradingTime::to_clock(21599), 86399);

    // 09:00 / 15:00：日盘小节端点
    EXPECT_EQ(TradingTime::from_clock(9 * 3600), TSEC_09_00);
    EXPECT_EQ(TradingTime::from_clock(15 * 3600), TSEC_15_00);
    EXPECT_EQ(TradingTime::to_clock(TSEC_09_00), 9 * 3600);
    EXPECT_EQ(TradingTime::to_clock(TSEC_15_00), 15 * 3600);
}

TEST(TradingTimeTest, ClockToTradingSecRoundTripAllSeconds) {
    for (int32_t clock = 0; clock < SECS_PER_DAY; ++clock) {
        ASSERT_EQ(TradingTime::to_clock(TradingTime::from_clock(clock)), clock)
            << "clock=" << clock;
    }
}

TEST(TradingTimeTest, TradingSecToClockRoundTripAllSeconds) {
    for (int32_t tsec = 0; tsec < SECS_PER_DAY; ++tsec) {
        ASSERT_EQ(TradingTime::from_clock(TradingTime::to_clock(tsec)), tsec) << "tsec=" << tsec;
    }
}

TEST(BarTest, StandardEndSecAddsPeriod) {
    Bar bar{};
    bar.begin_sec = TSEC_09_00;
    bar.period_sec = 60;
    EXPECT_EQ(bar.standard_end_sec(), TSEC_09_00 + 60);

    bar.period_sec = 900;
    EXPECT_EQ(bar.standard_end_sec(), TSEC_09_00 + 900);
}

TEST(BarTest, DailyBarEndsAtZeroAndStartsAtAnchor) {
    Bar bar{};
    bar.begin_sec = 0;
    bar.period_sec = 0;
    EXPECT_EQ(bar.standard_end_sec(), 0);
    EXPECT_EQ(bar.begin_clock(), TradingTime::ANCHOR_CLOCK_SEC);
}

TEST(BarTest, BeginClockMapsTradingSecToWallClock) {
    Bar bar{};

    bar.begin_sec = 0;  // 18:00
    EXPECT_EQ(bar.begin_clock(), 64800);

    bar.begin_sec = 6 * 3600;  // 00:00
    EXPECT_EQ(bar.begin_clock(), 0);

    bar.begin_sec = TSEC_09_00;  // 09:00
    EXPECT_EQ(bar.begin_clock(), 9 * 3600);

    bar.begin_sec = 21599;  // 23:59:59
    EXPECT_EQ(bar.begin_clock(), 86399);

    bar.begin_sec = 86399;  // 17:59:59（交易日终点前 1 秒）
    EXPECT_EQ(bar.begin_clock(), 64800 - 1);
}

TEST(BarTest, LastTickClockMapsTradingSecToWallClock) {
    Bar bar{};

    bar.last_tick_sec = 21599;  // 23:59:59
    EXPECT_EQ(bar.last_tick_clock(), 86399);

    bar.last_tick_sec = 0;  // 18:00
    EXPECT_EQ(bar.last_tick_clock(), 64800);

    bar.last_tick_sec = TSEC_09_00;  // 09:00
    EXPECT_EQ(bar.last_tick_clock(), 9 * 3600);
}

TEST(TradingSessionTableTest, SingleSessionIsValid) { EXPECT_TRUE(DAY_SESSION.valid()); }

TEST(TradingSessionTableTest, MultiSessionIsValid) { EXPECT_TRUE(DAY_AND_NIGHT.valid()); }

TEST(TradingSessionTableTest, SessionsAtCapacityAreValid) {
    TradingSessionTable table{};
    table.session_count = TradingSessionTable::MAX_SESSIONS;
    for (int32_t i = 0; i < TradingSessionTable::MAX_SESSIONS; ++i) {
        table.sessions[i] = TradingSession{i * 1000, i * 1000 + 999};
    }
    EXPECT_TRUE(table.valid());
}

TEST(TradingSessionTableTest, SessionEndingAtLastTradingSecIsValid) {
    EXPECT_TRUE(single_session(0, 86399).valid());
}

TEST(TradingSessionTableTest, EmptyTableIsInvalid) { EXPECT_FALSE(TradingSessionTable{}.valid()); }

TEST(TradingSessionTableTest, CountsOutOfRangeAreInvalid) {
    TradingSessionTable no_sessions = DAY_SESSION;
    no_sessions.session_count = 0;
    EXPECT_FALSE(no_sessions.valid());

    TradingSessionTable negative_sessions = DAY_SESSION;
    negative_sessions.session_count = -1;
    EXPECT_FALSE(negative_sessions.valid());

    TradingSessionTable too_many_sessions = DAY_SESSION;
    too_many_sessions.session_count = TradingSessionTable::MAX_SESSIONS + 1;
    EXPECT_FALSE(too_many_sessions.valid());

    TradingSessionTable negative_auctions = DAY_SESSION;
    negative_auctions.auction_count = -1;
    EXPECT_FALSE(negative_auctions.valid());

    TradingSessionTable too_many_auctions = DAY_SESSION;
    too_many_auctions.auction_count = TradingSessionTable::MAX_AUCTIONS + 1;
    EXPECT_FALSE(too_many_auctions.valid());
}

TEST(TradingSessionTableTest, SessionOutOfOrderIsInvalid) {
    EXPECT_FALSE(
        two_sessions(TradingSession{TSEC_09_00, TSEC_15_00}, TradingSession{TSEC_21_00, TSEC_02_30})
            .valid());
}

TEST(TradingSessionTableTest, OverlappingSessionsAreInvalid) {
    EXPECT_FALSE(two_sessions(TradingSession{TSEC_21_00, TSEC_02_30},
                              TradingSession{TSEC_02_30 - 60, TSEC_02_30 + 3600})
                     .valid());
}

TEST(TradingSessionTableTest, TouchingSessionsAreInvalid) {
    // 端点含：前一小节 end_sec == 后一小节 begin_sec 即同一秒属于两个小节。
    EXPECT_FALSE(two_sessions(TradingSession{TSEC_21_00, TSEC_02_30},
                              TradingSession{TSEC_02_30, TSEC_02_30 + 3600})
                     .valid());
}

TEST(TradingSessionTableTest, SessionBoundsAreChecked) {
    EXPECT_FALSE(single_session(-1, TSEC_15_00).valid());          // begin < 0
    EXPECT_FALSE(single_session(0, SECS_PER_DAY).valid());         // end > 86399
    EXPECT_FALSE(single_session(TSEC_09_00, TSEC_09_00).valid());  // begin == end
    EXPECT_FALSE(single_session(TSEC_15_00, TSEC_09_00).valid());  // begin > end
}

TEST(TradingSessionTableTest, AuctionsWithinRulesAreValid) {
    const TradingSessionTable table = with_auction(
        with_auction(DAY_SESSION,
                     AuctionWindow{AuctionKind::Open, 0, TSEC_09_00 - 60, TSEC_09_00 - 1}),
        AuctionWindow{AuctionKind::Close, 0, TSEC_15_00 - 60, TSEC_15_00});
    EXPECT_TRUE(table.valid());
}

TEST(TradingSessionTableTest, AuctionsAtCapacityAreValid) {
    TradingSessionTable table = DAY_SESSION;
    table.auction_count = TradingSessionTable::MAX_AUCTIONS;
    for (int32_t i = 0; i < TradingSessionTable::MAX_AUCTIONS; ++i) {
        table.auctions[i] = AuctionWindow{AuctionKind::Open, 0, i * 1000, i * 1000 + 999};
    }
    EXPECT_TRUE(table.valid());
}

TEST(TradingSessionTableTest, MultipleAuctionWindowsForDifferentSessionsAreValid) {
    const TradingSessionTable table = with_auction(
        with_auction(DAY_AND_NIGHT,
                     AuctionWindow{AuctionKind::Open, 0, TSEC_21_00 - 60, TSEC_21_00 - 1}),
        AuctionWindow{AuctionKind::Open, 1, TSEC_09_00 - 120, TSEC_09_00 - 1});
    EXPECT_TRUE(table.valid());
}

TEST(TradingSessionTableTest, AuctionSessionIndexOutOfRangeIsInvalid) {
    // session_count == 1，下标 1 越界
    const TradingSessionTable bad_index = with_auction(
        DAY_SESSION, AuctionWindow{AuctionKind::Open, 1, TSEC_09_00 - 60, TSEC_09_00 - 1});
    EXPECT_FALSE(bad_index.valid());

    const TradingSessionTable negative_index = with_auction(
        DAY_SESSION, AuctionWindow{AuctionKind::Open, -1, TSEC_09_00 - 60, TSEC_09_00 - 1});
    EXPECT_FALSE(negative_index.valid());
}

TEST(TradingSessionTableTest, OpenAuctionMustBeBeforeTargetSession) {
    // end == session.begin_sec：窗口与目标小节相接，未完全位于其之前
    const TradingSessionTable touching =
        with_auction(DAY_SESSION, AuctionWindow{AuctionKind::Open, 0, TSEC_09_00 - 60, TSEC_09_00});
    EXPECT_FALSE(touching.valid());

    // 窗口落在目标小节内部
    const TradingSessionTable inside = with_auction(
        DAY_SESSION, AuctionWindow{AuctionKind::Open, 0, TSEC_09_00 + 30, TSEC_09_00 + 90});
    EXPECT_FALSE(inside.valid());
}

TEST(TradingSessionTableTest, CloseAuctionMustBeWithinTargetSession) {
    const TradingSessionTable before_begin = with_auction(
        DAY_SESSION, AuctionWindow{AuctionKind::Close, 0, TSEC_09_00 - 60, TSEC_09_00 - 1});
    EXPECT_FALSE(before_begin.valid());

    const TradingSessionTable after_end = with_auction(
        DAY_SESSION, AuctionWindow{AuctionKind::Close, 0, TSEC_15_00 - 60, TSEC_15_00 + 1});
    EXPECT_FALSE(after_end.valid());
}

TEST(TradingSessionTableTest, AuctionsOutOfOrderOrOverlappingAreInvalid) {
    const TradingSessionTable out_of_order = with_auction(
        with_auction(DAY_SESSION,
                     AuctionWindow{AuctionKind::Close, 0, TSEC_15_00 - 60, TSEC_15_00}),
        AuctionWindow{AuctionKind::Open, 0, TSEC_09_00 - 60, TSEC_09_00 - 1});
    EXPECT_FALSE(out_of_order.valid());

    const TradingSessionTable overlapping = with_auction(
        with_auction(DAY_SESSION,
                     AuctionWindow{AuctionKind::Close, 0, TSEC_15_00 - 120, TSEC_15_00 - 60}),
        AuctionWindow{AuctionKind::Close, 0, TSEC_15_00 - 120, TSEC_15_00});
    EXPECT_FALSE(overlapping.valid());
}

TEST(TradingSessionTableTest, AuctionBoundsAreChecked) {
    const TradingSessionTable negative_begin =
        with_auction(DAY_SESSION, AuctionWindow{AuctionKind::Open, 0, -1, TSEC_09_00 - 1});
    EXPECT_FALSE(negative_begin.valid());

    const TradingSessionTable begin_after_end = with_auction(
        DAY_SESSION, AuctionWindow{AuctionKind::Close, 0, TSEC_15_00 - 1, TSEC_15_00 - 60});
    EXPECT_FALSE(begin_after_end.valid());

    const TradingSessionTable over_max = with_auction(
        DAY_SESSION, AuctionWindow{AuctionKind::Close, 0, TSEC_15_00 - 60, SECS_PER_DAY});
    EXPECT_FALSE(over_max.valid());
}

TEST(TradingSessionTableTest, UnknownAuctionKindIsInvalid) {
    const TradingSessionTable table = with_auction(
        DAY_SESSION,
        AuctionWindow{static_cast<AuctionKind>(2), 0, TSEC_09_00 - 60, TSEC_09_00 - 1});
    EXPECT_FALSE(table.valid());
}
