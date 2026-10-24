/**
 * @file history_query_test.cpp
 * @brief 历史查询语义（契约 sysmon §6.1）的纯函数测试：指标键解析、层级选择、
 *        点数上限窗口裁剪、缺口归并。
 *
 * 测试准则（docs/unit-test-standard.md）：只断言外部可观察行为——给定输入字符串/时刻
 * 序列/标记，产出可预期的解析结果、层级、窗口与缺口；不断言内部实现。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <dztrader/sysmonstore/history_query.h>
#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>

namespace dztrader::sysmonstore {
namespace {

HistoryMark make_mark(int64_t ts, std::string_view kind) {
    HistoryMark mark;
    mark.ts = ts;
    mark.kind = std::string(kind);
    return mark;
}

// ===== 指标键解析（契约 sysmon §6.1 的 metric 参数）=====

TEST(HistoryQueryTest, ResolvesScalarMetricKeys) {
    const auto cpu = resolve_metric("cpu.usage_ratio");
    ASSERT_TRUE(cpu.valid());
    EXPECT_FALSE(cpu.is_dimension);
    ASSERT_NE(cpu.scalar, nullptr);
    EXPECT_EQ(cpu.scalar->key, "cpu.usage_ratio");

    // 宽表键本身含点，必须先按宽表匹配（否则会被误切成域 memory + 指标 used_bytes）
    const auto memory = resolve_metric("memory.used_bytes");
    ASSERT_TRUE(memory.valid());
    EXPECT_FALSE(memory.is_dimension);
    ASSERT_NE(memory.scalar, nullptr);
    EXPECT_EQ(memory.scalar->key, "memory.used_bytes");

    // 宽表里存在 `shm_dir.*`（下划线）+ 长表域用下划线的组合：仍以宽表优先
    const auto shm = resolve_metric("shm_dir.page_files");
    ASSERT_TRUE(shm.valid());
    EXPECT_FALSE(shm.is_dimension);
}

TEST(HistoryQueryTest, ResolvesDimensionMetricKeys) {
    const auto network = resolve_metric("network.rx_bytes_per_sec");
    ASSERT_TRUE(network.valid());
    EXPECT_TRUE(network.is_dimension);
    EXPECT_EQ(network.domain, "network");
    EXPECT_EQ(network.metric, "rx_bytes_per_sec");
    EXPECT_EQ(network.scalar, nullptr);

    const auto core = resolve_metric("cpu_core.usage_ratio");
    ASSERT_TRUE(core.valid());
    EXPECT_TRUE(core.is_dimension);
    EXPECT_EQ(core.domain, "cpu_core");
    EXPECT_EQ(core.metric, "usage_ratio");
}

TEST(HistoryQueryTest, RejectsUnknownMetricKeys) {
    EXPECT_FALSE(resolve_metric("").valid());
    EXPECT_FALSE(resolve_metric("no_such_metric").valid());
    EXPECT_FALSE(resolve_metric("network.no_such_metric").valid());
    EXPECT_FALSE(resolve_metric("no_such_domain.rx_bytes_total").valid());
    // 缺点 / 点在前 / 点在后都算非法
    EXPECT_FALSE(resolve_metric("network").valid());
    EXPECT_FALSE(resolve_metric(".rx_bytes_total").valid());
    EXPECT_FALSE(resolve_metric("network.").valid());
}

TEST(HistoryQueryTest, ListsEveryRegisteredMetricKey) {
    const auto keys = list_metric_keys();
    // 全部宽表指标 + 全部维度指标都在清单里，且单调（已排序）、无重复
    EXPECT_EQ(
        keys.size(), kScalarMetrics.size() + [&] {
            size_t count = 0;
            for (const auto& domain : kDimensionDomains) {
                count += domain.metrics.size();
            }
            return count;
        }());
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
    EXPECT_EQ(std::adjacent_find(keys.begin(), keys.end()), keys.end());
    // 每个清单项都必须能解析回来（清单与解析器不得漂移）
    for (const auto& key : keys) {
        EXPECT_TRUE(resolve_metric(key).valid()) << key;
    }
}

// ===== 层级参数解析与自动选择（契约 sysmon §6.1.1）=====

TEST(HistoryQueryTest, ParsesLevelParameter) {
    EXPECT_EQ(parse_query_level("auto"), QueryLevel::Auto);
    EXPECT_EQ(parse_query_level("raw"), QueryLevel::Raw);
    EXPECT_EQ(parse_query_level("minute"), QueryLevel::Minute);
    EXPECT_FALSE(parse_query_level("hour").has_value());
    EXPECT_FALSE(parse_query_level("").has_value());
    EXPECT_FALSE(parse_query_level("RAW").has_value());  // 大小写敏感，不做静默宽松
}

TEST(HistoryQueryTest, MapsLevelToStorageAndName) {
    EXPECT_EQ(query_level_name(QueryLevel::Raw), "raw");
    EXPECT_EQ(query_level_name(QueryLevel::Minute), "minute");
    ASSERT_TRUE(query_level_to_storage(QueryLevel::Raw).has_value());
    EXPECT_EQ(*query_level_to_storage(QueryLevel::Raw), kLevelRaw);
    ASSERT_TRUE(query_level_to_storage(QueryLevel::Minute).has_value());
    EXPECT_EQ(*query_level_to_storage(QueryLevel::Minute), kLevelMinute);
    // auto 不是具体层级：调用方必须先 select_auto_level
    EXPECT_FALSE(query_level_to_storage(QueryLevel::Auto).has_value());
}

TEST(HistoryQueryTest, AutoSelectsRawWithinOneHourAndMinuteBeyond) {
    constexpr int64_t kStart = 1'790'000'000;
    // 边界：恰好 1 小时（3600）仍用原始记录；多 1 秒切分钟层
    EXPECT_EQ(select_auto_level(kStart, kStart + 3600), QueryLevel::Raw);
    EXPECT_EQ(select_auto_level(kStart, kStart + 3601), QueryLevel::Minute);
    // 典型范围
    EXPECT_EQ(select_auto_level(kStart, kStart + 300), QueryLevel::Raw);
    EXPECT_EQ(select_auto_level(kStart, kStart + 6 * 3600), QueryLevel::Minute);
    EXPECT_EQ(select_auto_level(kStart, kStart + 30 * 86400), QueryLevel::Minute);
    // 非法范围（to <= from）不得选分钟层去掩盖错误：调用方已回 400，这里退化为 raw
    EXPECT_EQ(select_auto_level(kStart, kStart), QueryLevel::Raw);
    EXPECT_EQ(select_auto_level(kStart, kStart - 100), QueryLevel::Raw);
}

// ===== 点数上限裁剪（契约 sysmon §6.1.2）=====

TEST(HistoryQueryTest, PointLimitKeepsEverythingWhenUnderLimit) {
    // 取回行数 <= 上限：全留、不标截断（哪怕范围宽度远大于「上限 x 桶宽」——
    // 该段本来只有这么点数据，丢任何一行都是错的）
    const auto trim = trim_to_point_limit(7, 10);
    EXPECT_EQ(trim.keep, 7U);
    EXPECT_FALSE(trim.truncated);
    // 恰好等于上限同样不截断
    const auto exact = trim_to_point_limit(10, 10);
    EXPECT_EQ(exact.keep, 10U);
    EXPECT_FALSE(exact.truncated);
}

TEST(HistoryQueryTest, PointLimitTrimsOldestRowsWhenOverLimit) {
    // 降序多取一行（max_points + 1）是「还有更旧数据」的证据：留最新 max_points 行
    const auto trim = trim_to_point_limit(11, 10);
    EXPECT_EQ(trim.keep, 10U);
    EXPECT_TRUE(trim.truncated);
}

TEST(HistoryQueryTest, PointLimitDegenerateInputsKeepEverything) {
    // 上限非正（调用方已校验；这里防御性退化为不裁）
    const auto zero = trim_to_point_limit(5, 0);
    EXPECT_EQ(zero.keep, 5U);
    EXPECT_FALSE(zero.truncated);
    const auto negative = trim_to_point_limit(5, -1);
    EXPECT_EQ(negative.keep, 5U);
    EXPECT_FALSE(negative.truncated);
    // 空结果
    const auto empty = trim_to_point_limit(0, 10);
    EXPECT_EQ(empty.keep, 0U);
    EXPECT_FALSE(empty.truncated);
}

TEST(HistoryQueryTest, PointLimitDeclarationMatchesContractRange) {
    // 契约 §6.1 的 max_points 合法区间与默认值必须与头文件一致（防文档/代码漂移）
    EXPECT_EQ(kMinMaxPoints, 10);
    EXPECT_EQ(kMaxMaxPoints, 20000);
    EXPECT_EQ(kDefaultMaxPoints, 1500);
    // auto 层级的范围阈值：契约 §6.1.1 写「1 小时」
    EXPECT_EQ(kAutoRawMaxRangeSec, 3600);
}

// ===== 缺口归并（契约 sysmon §6.1.4）=====

TEST(HistoryQueryTest, NoGapsForContiguousBuckets) {
    const std::vector<int64_t> buckets{100, 160, 220, 280};
    EXPECT_TRUE(derive_gaps(buckets, 60, {}).empty());
}

TEST(HistoryQueryTest, GapForDiscontinuityWithoutMarkIsUnmarked) {
    // 100、160 之后跳到 400：缺口 [220, 400)，没有标记解释 ⇒ unmarked
    const std::vector<int64_t> buckets{100, 160, 400};
    const auto gaps = derive_gaps(buckets, 60, {});
    ASSERT_EQ(gaps.size(), 1U);
    EXPECT_EQ(gaps[0].from_ts, 220);
    EXPECT_EQ(gaps[0].to_ts, 400);
    EXPECT_EQ(gaps[0].kind, "unmarked");
}

TEST(HistoryQueryTest, GapKindFromMarkInsideInterval) {
    const std::vector<int64_t> buckets{100, 400};
    // 样本缺口标记落在 [160, 400) 内
    const auto gap_marks = derive_gaps(buckets, 60, {make_mark(300, kMarkSampleGap)});
    ASSERT_EQ(gap_marks.size(), 1U);
    EXPECT_EQ(gap_marks[0].kind, "sample_gap");

    // 重启标记优先于样本缺口（进程重启是更强的解释）
    const auto restart =
        derive_gaps(buckets, 60, {make_mark(300, kMarkSampleGap), make_mark(320, kMarkRestart)});
    ASSERT_EQ(restart.size(), 1U);
    EXPECT_EQ(restart[0].kind, "restart");

    // 标记顺序不影响结论
    const auto reversed =
        derive_gaps(buckets, 60, {make_mark(320, kMarkRestart), make_mark(300, kMarkSampleGap)});
    ASSERT_EQ(reversed.size(), 1U);
    EXPECT_EQ(reversed[0].kind, "restart");
}

TEST(HistoryQueryTest, ClockJumpMarkDoesNotExplainGap) {
    // 跳变标记不是缺口原因：没有 restart/sample_gap 时缺口仍是 unmarked
    const std::vector<int64_t> buckets{100, 400};
    const auto gaps = derive_gaps(buckets, 60, {make_mark(300, kMarkClockJump)});
    ASSERT_EQ(gaps.size(), 1U);
    EXPECT_EQ(gaps[0].kind, "unmarked");
}

TEST(HistoryQueryTest, MarkOutsideGapIntervalIsIgnored) {
    // 标记在缺口之外（落在有数据的区间内）不得解释该缺口
    const std::vector<int64_t> buckets{100, 160, 400};
    const auto gaps = derive_gaps(buckets, 60, {make_mark(100, kMarkRestart)});
    ASSERT_EQ(gaps.size(), 1U);
    EXPECT_EQ(gaps[0].kind, "unmarked");
}

TEST(HistoryQueryTest, MultipleGapsAreReportedInOrder) {
    // 100,160 | 缺 | 400,460 | 缺 | 700
    const std::vector<int64_t> buckets{100, 160, 400, 460, 700};
    const auto gaps =
        derive_gaps(buckets, 60, {make_mark(300, kMarkSampleGap), make_mark(600, kMarkRestart)});
    ASSERT_EQ(gaps.size(), 2U);
    EXPECT_EQ(gaps[0].from_ts, 220);
    EXPECT_EQ(gaps[0].to_ts, 400);
    EXPECT_EQ(gaps[0].kind, "sample_gap");
    EXPECT_EQ(gaps[1].from_ts, 520);
    EXPECT_EQ(gaps[1].to_ts, 700);
    EXPECT_EQ(gaps[1].kind, "restart");
    // 互不重叠、按 from 升序
    EXPECT_LT(gaps[0].to_ts, gaps[1].from_ts);
}

TEST(HistoryQueryTest, NoGapsForFewerThanTwoPoints) {
    EXPECT_TRUE(derive_gaps({}, 60, {}).empty());
    EXPECT_TRUE(derive_gaps({100}, 60, {}).empty());
    // 桶宽非正：无从判定连续性，不得编造缺口
    EXPECT_TRUE(derive_gaps({100, 400}, 0, {}).empty());
}

TEST(HistoryQueryTest, OverlappingBucketsAreNotGaps) {
    // 相邻重叠/逆序不产生缺口（防御手工改库的脏数据，不把它当采集中断）
    const std::vector<int64_t> buckets{100, 120, 130};
    EXPECT_TRUE(derive_gaps(buckets, 60, {}).empty());
}

}  // namespace
}  // namespace dztrader::sysmonstore
