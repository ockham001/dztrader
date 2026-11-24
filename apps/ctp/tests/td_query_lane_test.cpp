// QueryLane 单元测试（ADR 0055 决策 2）：纯逻辑，注入时间点，不 sleep。
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

#include "td/td_query_lane.h"

namespace dztrader::ctp {
namespace {

using std::chrono::milliseconds;

QueryLane::TimePoint at(int64_t ms) { return QueryLane::TimePoint{} + milliseconds(ms); }

constexpr QueryKind kFunds = QueryKind::TradingAccount;

class QueryLaneTest : public ::testing::Test {
protected:
    QueryPacer pacer_{milliseconds(1100)};
    QueryLane lane_{pacer_, /*max_retries=*/3};
};

TEST_F(QueryLaneTest, DirtyBecomesReadyAndClearsAfterResponse) {
    EXPECT_FALSE(lane_.pending(kFunds));
    EXPECT_EQ(lane_.take_ready(at(0)), std::nullopt);

    lane_.mark_dirty(kFunds);
    EXPECT_TRUE(lane_.pending(kFunds));
    ASSERT_TRUE(lane_.take_ready(at(0)).has_value());
    EXPECT_EQ(*lane_.take_ready(at(0)), kFunds);

    lane_.note_sent(at(0), kFunds);
    EXPECT_TRUE(lane_.in_flight());
    EXPECT_FALSE(lane_.pending(kFunds));
    EXPECT_EQ(lane_.take_ready(at(5000)), std::nullopt);  // 在途

    lane_.note_response(at(10), kFunds);
    EXPECT_FALSE(lane_.in_flight());
    EXPECT_EQ(lane_.attempts(kFunds), 0);
    EXPECT_EQ(lane_.take_ready(at(1100)), std::nullopt);  // 无新置脏
}

TEST_F(QueryLaneTest, EventAfterSendIsNotLost) {
    lane_.mark_dirty(kFunds);
    ASSERT_TRUE(lane_.take_ready(at(0)).has_value());
    lane_.note_sent(at(0), kFunds);

    // 查询在途期间又来事件：只置脏，不额外发查询。
    lane_.mark_dirty(kFunds);
    EXPECT_TRUE(lane_.pending(kFunds));
    EXPECT_EQ(lane_.take_ready(at(1200)), std::nullopt);  // 在途

    lane_.note_response(at(1500), kFunds);
    // 收口后仍需等最小间隔（上次发送 at(0)），且由于发送后置脏，还要补一笔。
    EXPECT_EQ(lane_.take_ready(at(1099)), std::nullopt);
    ASSERT_TRUE(lane_.take_ready(at(1100)).has_value());
    lane_.note_sent(at(1100), kFunds);
    EXPECT_FALSE(lane_.pending(kFunds));
}

TEST_F(QueryLaneTest, PacerGapBlocksReadyUntilEligible) {
    lane_.mark_dirty(kFunds);
    ASSERT_TRUE(lane_.take_ready(at(0)).has_value());
    lane_.note_sent(at(0), kFunds);
    lane_.note_response(at(10), kFunds);

    lane_.mark_dirty(kFunds);
    EXPECT_EQ(lane_.take_ready(at(1099)), std::nullopt);
    EXPECT_EQ(lane_.next_wake(at(1099)), at(1100));
    ASSERT_TRUE(lane_.take_ready(at(1100)).has_value());
}

TEST_F(QueryLaneTest, FailureBackoffThenRetry) {
    lane_.mark_dirty(kFunds);
    ASSERT_TRUE(lane_.take_ready(at(0)).has_value());
    lane_.note_sent(at(0), kFunds);

    // 第 1 次失败：退避 1 个 min_gap（1100ms）。
    lane_.note_failure(at(100), kFunds);
    EXPECT_FALSE(lane_.in_flight());
    EXPECT_EQ(lane_.attempts(kFunds), 1);
    EXPECT_TRUE(lane_.pending(kFunds));
    EXPECT_EQ(lane_.take_ready(at(100)), std::nullopt);
    EXPECT_EQ(lane_.next_wake(at(100)), at(1200));
    ASSERT_TRUE(lane_.take_ready(at(1200)).has_value());

    // 第 2 次失败：退避从失败时刻起翻倍（1230 + 2200）。
    lane_.note_sent(at(1200), kFunds);
    lane_.note_failure(at(1230), kFunds);
    EXPECT_EQ(lane_.attempts(kFunds), 2);
    EXPECT_EQ(lane_.next_wake(at(1230)), at(1230 + 2200));
    EXPECT_EQ(lane_.take_ready(at(3000)), std::nullopt);

    // 成功收口：清尝试次数与退避。
    ASSERT_TRUE(lane_.take_ready(at(3430)).has_value());
    lane_.note_sent(at(3430), kFunds);
    lane_.note_response(at(3440), kFunds);
    EXPECT_EQ(lane_.attempts(kFunds), 0);
    EXPECT_EQ(lane_.next_wake(at(3410)), std::nullopt);  // 无新置脏
}

TEST_F(QueryLaneTest, ExhaustionDropsPendingAndNewEventRearms) {
    lane_.mark_dirty(kFunds);
    // 每次失败都进入退避；用递增的时间点跨过退避窗口（第 n 次等待 min_gap * 2^(n-1)）。
    for (int i = 0; i <= QueryLane::kDefaultMaxRetries; ++i) {
        const auto step = at(static_cast<int64_t>(i) * 10000);
        ASSERT_TRUE(lane_.take_ready(step).has_value()) << "attempt " << i;
        lane_.note_sent(step, kFunds);
        lane_.note_failure(step, kFunds);
    }
    EXPECT_TRUE(lane_.exhausted(kFunds));
    EXPECT_FALSE(lane_.pending(kFunds));
    EXPECT_EQ(lane_.take_ready(at(60000)), std::nullopt);
    EXPECT_EQ(lane_.next_wake(at(60000)), std::nullopt);

    lane_.clear_exhausted(kFunds);
    EXPECT_FALSE(lane_.exhausted(kFunds));

    // 新事件重新置脏 → 立即可发（pacer 未在途，间隔按上次发送算；这里用足够晚的时刻）。
    lane_.mark_dirty(kFunds);
    EXPECT_TRUE(lane_.pending(kFunds));
    ASSERT_TRUE(lane_.take_ready(at(60000)).has_value());
}

TEST_F(QueryLaneTest, NextWakeIsImmediateWhenReady) {
    lane_.mark_dirty(kFunds);
    const auto wake = lane_.next_wake(at(500));
    ASSERT_TRUE(wake.has_value());
    EXPECT_LE(*wake, at(500));
}

TEST_F(QueryLaneTest, BackoffIsCappedAtFiveSeconds) {
    QueryPacer pacer{milliseconds(1100)};
    QueryLane lane{pacer, /*max_retries=*/6};  // 允许第 4 次失败（理论退避 8800ms > 5s 封顶）
    lane.mark_dirty(kFunds);

    int64_t t = 0;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(lane.take_ready(at(t)).has_value()) << "attempt " << i;
        lane.note_sent(at(t), kFunds);
        lane.note_failure(at(t), kFunds);
        t += 10000;
    }
    // 第 4 次失败发生在 30000：min(1100 * 8, 5000) = 5000 → 35000 才可重试。
    EXPECT_EQ(lane.attempts(kFunds), 4);
    EXPECT_EQ(lane.next_wake(at(30000)), at(35000));
}

TEST_F(QueryLaneTest, InFlightHasNoWake) {
    lane_.mark_dirty(kFunds);
    ASSERT_TRUE(lane_.take_ready(at(0)).has_value());
    lane_.note_sent(at(0), kFunds);
    EXPECT_EQ(lane_.next_wake(at(0)), std::nullopt);
}

TEST_F(QueryLaneTest, ResetClearsEverything) {
    lane_.mark_dirty(kFunds);
    ASSERT_TRUE(lane_.take_ready(at(0)).has_value());
    lane_.note_sent(at(0), kFunds);
    lane_.note_failure(at(10), kFunds);
    lane_.reset();

    EXPECT_FALSE(lane_.pending(kFunds));
    EXPECT_FALSE(lane_.in_flight());
    EXPECT_FALSE(lane_.exhausted(kFunds));
    EXPECT_EQ(lane_.attempts(kFunds), 0);
    EXPECT_EQ(lane_.next_wake(at(0)), std::nullopt);
    // 复位后 pacer 立即允许，再置脏可马上发。
    lane_.mark_dirty(kFunds);
    EXPECT_TRUE(lane_.take_ready(at(0)).has_value());
}

TEST_F(QueryLaneTest, EventDuringBackoffDoesNotResetBudget) {
    lane_.mark_dirty(kFunds);
    ASSERT_TRUE(lane_.take_ready(at(0)).has_value());
    lane_.note_sent(at(0), kFunds);
    lane_.note_failure(at(0), kFunds);  // 第 1 次失败：退避到 1100
    ASSERT_EQ(lane_.attempts(kFunds), 1);

    // 退避期间的新事件不重置尝试次数/退避时刻（持续事件流不得吞掉失败预算）。
    lane_.mark_dirty(kFunds);
    EXPECT_EQ(lane_.attempts(kFunds), 1);
    EXPECT_EQ(lane_.take_ready(at(0)), std::nullopt);
    EXPECT_EQ(lane_.next_wake(at(0)), at(1100));

    // 第 2 次失败按翻倍退避（1100 + 2200 = 3300）。
    ASSERT_TRUE(lane_.take_ready(at(1100)).has_value());
    lane_.note_sent(at(1100), kFunds);
    lane_.note_failure(at(1100), kFunds);
    EXPECT_EQ(lane_.attempts(kFunds), 2);
    EXPECT_EQ(lane_.next_wake(at(1100)), at(3300));
}

TEST_F(QueryLaneTest, SendFailureCountsWithoutInFlight) {
    lane_.mark_dirty(kFunds);
    ASSERT_TRUE(lane_.take_ready(at(0)).has_value());
    // 发送失败：ReqQry* 返回非 0，未进入在途，仍须计入退避预算。
    lane_.note_send_failure(at(0), kFunds);
    EXPECT_EQ(lane_.attempts(kFunds), 1);
    EXPECT_TRUE(lane_.pending(kFunds));
    EXPECT_EQ(lane_.take_ready(at(0)), std::nullopt);
    EXPECT_EQ(lane_.next_wake(at(0)), at(1100));
    ASSERT_TRUE(lane_.take_ready(at(1100)).has_value());
}

TEST_F(QueryLaneTest, LateResponseAfterResetIsIgnored) {
    lane_.mark_dirty(kFunds);
    ASSERT_TRUE(lane_.take_ready(at(0)).has_value());
    lane_.note_sent(at(0), kFunds);
    lane_.reset();

    // 断线复位后迟到的失败/成功响应不得复活待查或推进退避预算。
    lane_.note_failure(at(10), kFunds);
    lane_.note_response(at(10), kFunds);
    EXPECT_FALSE(lane_.in_flight());
    EXPECT_FALSE(lane_.pending(kFunds));
    EXPECT_FALSE(lane_.exhausted(kFunds));
    EXPECT_EQ(lane_.attempts(kFunds), 0);
}

TEST_F(QueryLaneTest, TakeReadyDoesNotMutateInFlight) {
    lane_.mark_dirty(kFunds);
    ASSERT_TRUE(lane_.take_ready(at(0)).has_value());
    // 取 ready 后、note_sent 前再次询问仍返回同一笔（调用方单线程串行，不允许并发取）。
    EXPECT_EQ(lane_.take_ready(at(0)), kFunds);
    EXPECT_FALSE(lane_.in_flight());
}

}  // namespace
}  // namespace dztrader::ctp
