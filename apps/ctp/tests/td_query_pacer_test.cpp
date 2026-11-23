// QueryPacer 单元测试（ADR 0055 决策 1）：纯逻辑，注入时间点，不 sleep。
#include <gtest/gtest.h>

#include "td/td_query_pacer.h"

namespace dztrader::ctp {
namespace {

using Clock = QueryPacer::Clock;
using std::chrono::milliseconds;

QueryPacer::TimePoint at(int64_t ms) { return QueryPacer::TimePoint{} + milliseconds(ms); }

TEST(QueryPacerTest, FreshPacerAllowsImmediateQuery) {
    QueryPacer pacer{milliseconds(1100)};
    EXPECT_FALSE(pacer.in_flight());
    EXPECT_FALSE(pacer.has_sent());
    EXPECT_TRUE(pacer.can_issue(at(0)));
    EXPECT_EQ(pacer.next_eligible(at(0)), at(0));
}

TEST(QueryPacerTest, InFlightBlocksNextQueryUntilDone) {
    QueryPacer pacer{milliseconds(1100)};
    pacer.note_sent(at(1000));
    EXPECT_TRUE(pacer.in_flight());
    EXPECT_FALSE(pacer.can_issue(at(5000)));  // 在途优先于间隔
    EXPECT_EQ(pacer.next_eligible(at(5000)), QueryPacer::TimePoint::max());

    pacer.note_done();
    EXPECT_FALSE(pacer.in_flight());
    EXPECT_TRUE(pacer.can_issue(at(1000 + 1100)));
}

TEST(QueryPacerTest, EnforcesMinimumGapBetweenSends) {
    QueryPacer pacer{milliseconds(1100)};
    pacer.note_sent(at(100));
    pacer.note_done();

    EXPECT_FALSE(pacer.can_issue(at(100)));
    EXPECT_FALSE(pacer.can_issue(at(100 + 1099)));
    EXPECT_TRUE(pacer.can_issue(at(100 + 1100)));
    EXPECT_EQ(pacer.next_eligible(at(200)), at(100 + 1100));
    // 已过合法时刻时 next_eligible 不早于 now。
    EXPECT_EQ(pacer.next_eligible(at(5000)), at(5000));
}

TEST(QueryPacerTest, NoteDoneIsIdempotent) {
    QueryPacer pacer{milliseconds(1100)};
    pacer.note_sent(at(0));
    pacer.note_done();
    pacer.note_done();
    EXPECT_FALSE(pacer.in_flight());
}

TEST(QueryPacerTest, ResetAllowsImmediateQueryAgain) {
    QueryPacer pacer{milliseconds(1100)};
    pacer.note_sent(at(0));
    pacer.reset();
    EXPECT_FALSE(pacer.in_flight());
    EXPECT_FALSE(pacer.has_sent());
    EXPECT_TRUE(pacer.can_issue(at(0)));
}

TEST(QueryPacerTest, MinGapHotUpdateTakesEffect) {
    QueryPacer pacer{milliseconds(1100)};
    pacer.note_sent(at(0));
    pacer.note_done();
    pacer.set_min_gap(milliseconds(200));
    EXPECT_FALSE(pacer.can_issue(at(199)));
    EXPECT_TRUE(pacer.can_issue(at(200)));
    // 非法值夹到 1ms，保持可用（配置校验层已拒绝 <= 0）。
    pacer.set_min_gap(milliseconds(0));
    EXPECT_EQ(pacer.min_gap(), milliseconds(1));
}

}  // namespace
}  // namespace dztrader::ctp
