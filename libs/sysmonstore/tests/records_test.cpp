/**
 * @file records_test.cpp
 * @brief 聚合语义、时间桶对齐与时钟跳变检测（#78）。
 *
 * 这些是纯函数，是「一条记录保存窗口 min/max/avg/last」与「记录 tick 带相位偏移、
 * 不落在整数分钟边界」两条验收标准的确定性验证点。
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>

namespace dztrader::sysmonstore {
namespace {

// ===== MetricAggregate =====

TEST(MetricAggregateTest, ObserveTracksMinMaxAvgLast) {
    MetricAggregate aggregate;
    EXPECT_TRUE(aggregate.empty());

    aggregate.observe(10.0);
    EXPECT_EQ(aggregate.count, 1);
    EXPECT_DOUBLE_EQ(aggregate.min, 10.0);
    EXPECT_DOUBLE_EQ(aggregate.max, 10.0);
    EXPECT_DOUBLE_EQ(aggregate.avg, 10.0);
    EXPECT_DOUBLE_EQ(aggregate.last, 10.0);

    aggregate.observe(30.0);
    aggregate.observe(20.0);
    EXPECT_EQ(aggregate.count, 3);
    EXPECT_DOUBLE_EQ(aggregate.min, 10.0);
    EXPECT_DOUBLE_EQ(aggregate.max, 30.0);
    EXPECT_DOUBLE_EQ(aggregate.avg, 20.0);
    EXPECT_DOUBLE_EQ(aggregate.last, 20.0);  // last 是最后观测到的值，不是最大值
}

TEST(MetricAggregateTest, PeakIsPreservedByMaxNotAverage) {
    // 「尖峰由 max 保留」：一个 5% 的均值掩盖不了单次 95% 的尖峰。
    MetricAggregate aggregate;
    for (int i = 0; i < 19; ++i) {
        aggregate.observe(0.01);
    }
    aggregate.observe(0.95);
    EXPECT_DOUBLE_EQ(aggregate.max, 0.95);
    EXPECT_LT(aggregate.avg, 0.10);
}

TEST(MetricAggregateTest, EmptyAggregateIsEmpty) {
    const MetricAggregate aggregate;
    EXPECT_TRUE(aggregate.empty());
    EXPECT_EQ(aggregate.count, 0);
}

TEST(MetricAggregateTest, MergeWeightsAverageBySampleCount) {
    // 压缩两个桶时 avg 必须按有效样本数加权——直接平均会把均值算偏。
    MetricAggregate lhs;
    lhs.observe(10.0);  // count=1, avg=10
    MetricAggregate rhs;
    rhs.observe(20.0);  // count=1, avg=20
    lhs.merge(rhs);
    EXPECT_EQ(lhs.count, 2);
    EXPECT_DOUBLE_EQ(lhs.avg, 15.0);

    // 再合并一个大样本桶：加权后应偏向样本多的一侧。
    MetricAggregate heavy;
    for (int i = 0; i < 8; ++i) {
        heavy.observe(100.0);
    }
    lhs.merge(heavy);  // (10+20)/2*2 样本 + 8 样本的 100
    EXPECT_EQ(lhs.count, 10);
    EXPECT_DOUBLE_EQ(lhs.avg, (15.0 * 2 + 100.0 * 8) / 10.0);
}

TEST(MetricAggregateTest, MergeTakesExtremesAndLaterLast) {
    MetricAggregate earlier;
    earlier.observe(5.0);
    earlier.observe(8.0);
    MetricAggregate later;
    later.observe(3.0);
    later.observe(9.0);
    earlier.merge(later);
    EXPECT_DOUBLE_EQ(earlier.min, 3.0);
    EXPECT_DOUBLE_EQ(earlier.max, 9.0);
    EXPECT_DOUBLE_EQ(earlier.last, 9.0);  // 调用方保证 later 时间更晚
    EXPECT_EQ(earlier.count, 4);
}

TEST(MetricAggregateTest, MergeIntoEmptyTakesOther) {
    MetricAggregate empty;
    MetricAggregate other;
    other.observe(42.0);
    empty.merge(other);
    EXPECT_EQ(empty.count, 1);
    EXPECT_DOUBLE_EQ(empty.max, 42.0);
}

TEST(MetricAggregateTest, MergeEmptyIsNoOp) {
    MetricAggregate aggregate;
    aggregate.observe(7.0);
    aggregate.merge(MetricAggregate{});
    EXPECT_EQ(aggregate.count, 1);
    EXPECT_DOUBLE_EQ(aggregate.avg, 7.0);
}

// ===== 时间桶对齐 =====

TEST(BucketStartTest, PhaseOffsetMovesBoundaryOffIntegerMinute) {
    // 验收标准：「记录 tick 带相位偏移，不落在整数分钟边界」。
    // 默认相位 = 3 秒 ⇒ 60 秒窗口的起点是每分钟第 3 秒，而不是 :00。
    const int64_t now = 1'790'000'000;  // 任意基准时刻
    const int64_t bucket = bucket_start(now, 60, 3);
    EXPECT_EQ(bucket % 60, 3) << "bucket must start at phase offset, not integer minute";
    EXPECT_LE(bucket, now);
    EXPECT_LT(now - bucket, 60);
}

TEST(BucketStartTest, SameBucketForAllTimesWithinInterval) {
    const int64_t base = bucket_start(1'790'000'000, 60, 3);
    for (int64_t offset = 0; offset < 60; ++offset) {
        EXPECT_EQ(bucket_start(base + offset, 60, 3), base)
            << "offset " << offset << " must stay in the same bucket";
    }
    // 下一格恰好是 base + interval。
    EXPECT_EQ(bucket_start(base + 60, 60, 3), base + 60);
}

TEST(BucketStartTest, PhaseClampedWhenLargerThanInterval) {
    // 配置允许 phase ∈ [0, 86400] 而 interval 可能只有 1 秒：相位必须钳制到
    // [0, interval)，否则会得到跨多个桶的偏移，桶边界失去意义。
    const int64_t bucket = bucket_start(1'790'000'000, 60, 90);
    EXPECT_EQ(bucket % 60, 90 % 60);
}

TEST(BucketStartTest, ZeroPhaseAlignsToIntervalGrid) {
    const int64_t now = 1'790'000'123;
    const int64_t bucket = bucket_start(now, 60, 0);
    EXPECT_EQ(bucket % 60, 0);
    EXPECT_EQ(bucket, now - now % 60);
}

TEST(BucketStartTest, NonPositiveIntervalReturnsInput) {
    // 防御：非法间隔不推进、不除零（配置校验已保证 > 0，但纯函数不能因此崩）。
    EXPECT_EQ(bucket_start(1'790'000'000, 0, 3), 1'790'000'000);
    EXPECT_EQ(bucket_start(1'790'000'000, -60, 3), 1'790'000'000);
}

TEST(BucketStartTest, NegativeTimestampsFloorRatherThanTruncate) {
    // 时钟被回拨到 1970 前（罕见但真实）：截断除法会让桶边界跳一格，
    // 用 floor 保证相邻时刻仍落在同一桶。
    EXPECT_EQ(bucket_start(-1, 60, 0), -60);
    EXPECT_EQ(bucket_start(-59, 60, 0), -60);
    EXPECT_EQ(bucket_start(-60, 60, 0), -60);
}

TEST(MinuteBucketStartTest, AlignsToIntegerMinute) {
    // 压缩桶按墙钟整分钟对齐：便于与交易事件对照（这是契约的明文要求）。
    // 取一个整分钟基准：1_790_000_040 能被 60 整除。
    constexpr int64_t kMinute = 1'790'000'040;
    ASSERT_EQ(kMinute % 60, 0);
    EXPECT_EQ(minute_bucket_start(kMinute), kMinute);
    EXPECT_EQ(minute_bucket_start(kMinute + 59), kMinute);
    EXPECT_EQ(minute_bucket_start(kMinute + 60), kMinute + 60);
    // 任意时刻的返回值本身必须是整分钟。
    EXPECT_EQ(minute_bucket_start(1'790'000'000) % 60, 0);
}

// ===== 时钟跳变检测 =====

TEST(ClockJumpTest, NoJumpWhenWallAndSteadyAdvanceTogether) {
    // 正常情形：墙钟与单调时钟走过同样的时间。
    EXPECT_FALSE(detect_clock_jump(1'000'000, 500'000, 1'001'000, 501'000, 5000).has_value());
}

TEST(ClockJumpTest, ForwardJumpIsDetectedWithSignedDrift) {
    // 墙钟前跳 30 秒而单调时钟只走 1 秒 ⇒ 漂移 +29 秒。
    const auto drift = detect_clock_jump(1'000'000, 500'000, 1'031'000, 501'000, 5000);
    ASSERT_TRUE(drift.has_value());
    EXPECT_EQ(*drift, 30'000);  // 墙钟走 31s、单调走 1s ⇒ 漂移 30s
}

TEST(ClockJumpTest, BackwardJumpIsDetected) {
    // 墙钟回拨 30 秒 ⇒ 漂移 -30 秒（负值，调用方据此知道桶会倒退）。
    const auto drift = detect_clock_jump(1'030'000, 500'000, 1'000'000, 501'000, 5000);
    ASSERT_TRUE(drift.has_value());
    EXPECT_EQ(*drift, -31'000);
}

TEST(ClockJumpTest, SmallDriftBelowThresholdIsIgnored) {
    // 抖动/舍入在阈值内不算跳变（否则每 tick 都会打标记，标记失去意义）。
    EXPECT_FALSE(detect_clock_jump(1'000'000, 500'000, 1'004'000, 501'000, 5000).has_value());
}

TEST(ClockJumpTest, DriftExactlyAtThresholdIsNotAJump) {
    // 边界：恰好等于阈值不算超阈值（严格大于才判跳变）。
    EXPECT_FALSE(detect_clock_jump(1'000'000, 500'000, 1'001'000, 500'000, 1000).has_value());
    EXPECT_TRUE(detect_clock_jump(1'000'000, 500'000, 1'002'000, 500'000, 1000).has_value());
}

// ===== HistoryBatch =====

TEST(HistoryBatchTest, EmptyAndRecordCount) {
    HistoryBatch batch;
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(batch.record_count(), 0U);

    batch.scalars.push_back(ScalarRecord{});
    EXPECT_FALSE(batch.empty());
    EXPECT_EQ(batch.record_count(), 1U);

    batch.marks.push_back(HistoryMark{});
    EXPECT_EQ(batch.record_count(), 2U);

    batch.dimensions.push_back(DimensionRecord{});
    EXPECT_EQ(batch.record_count(), 3U);

    batch.clear();
    EXPECT_TRUE(batch.empty());
}

}  // namespace
}  // namespace dztrader::sysmonstore
