/**
 * @file history_recorder_test.cpp
 * @brief 记录窗口聚合与缺口/跳变标记（#78）。
 *
 * 这是「每条记录保存该窗口 min/max/avg/last」「记录 tick 带相位偏移，不落在整数分钟
 * 边界」「采集中断标记写入历史，绝不伪造连续数据」三条验收标准的验证点。
 *
 * 全部用假快照确定性驱动：不读真实时钟、不碰数据库、不起线程。
 */

#include "history_recorder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <dztrader/platform/sysmon.h>
#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>

namespace dztrader::sysmon {
namespace {

/// 构造一个最小合法快照：只填要断言的域，其余留空/不可用（真实进程的行为一致）。
platform::SysmonSnapshot make_snapshot(double cpu_usage) {
    platform::SysmonSnapshot snapshot;
    snapshot.source = platform::SYSMON_INSTANCE_NAME;
    snapshot.cpu.total.usage_ratio = cpu_usage;
    return snapshot;
}

/// 取某个标量指标的聚合量（按逻辑键，避免测试依赖注册表下标）。
sysmonstore::MetricAggregate scalar_of(const sysmonstore::HistoryBatch& batch,
                                       std::string_view key) {
    const auto* spec = sysmonstore::find_scalar_metric(key);
    if (spec == nullptr) {
        return {};
    }
    const auto index = static_cast<size_t>(spec - sysmonstore::kScalarMetrics.data());
    if (batch.scalars.empty()) {
        return {};
    }
    return batch.scalars[0].metrics[index];
}

/// 计数某个 kind 的标记。
int count_marks(const sysmonstore::HistoryBatch& batch, std::string_view kind) {
    int count = 0;
    for (const auto& mark : batch.marks) {
        if (mark.kind == kind) {
            ++count;
        }
    }
    return count;
}

/// 一个整分钟起点的基准时刻（便于推演窗口边界）。
constexpr int64_t kBase = 1'790'000'040;  // 能被 60 整除

class HistoryRecorderTest : public ::testing::Test {
protected:
    // 默认配置：60 秒窗口 + 3 秒相位 + 1 秒计量 ⇒ 满窗 60 个样本。
    HistoryRecorder recorder_{
        RecordWindowConfig{.interval_sec = 60, .phase_sec = 3, .measure_interval_ms = 1000}};
};

// ===== 窗口与批次产生 =====

TEST_F(HistoryRecorderTest, FirstObservationOnlyOpensWindow) {
    // 首个观测只建立当前窗口，不产生批次（此时还没有「已结束的窗口」）。
    // 外部可观察承诺：第一次 observe 返回空批次，且该样本会被计入随后结算的窗口。
    const auto first = recorder_.observe(make_snapshot(0.5), kBase + 3);
    EXPECT_TRUE(first.empty()) << "first observation must not emit a batch";
    const auto settled = recorder_.observe(make_snapshot(0.5), kBase + 63);
    ASSERT_FALSE(settled.empty());
    EXPECT_EQ(scalar_of(settled, "cpu.usage_ratio").count, 1)
        << "the first observation must be part of the first settled window";
}

TEST_F(HistoryRecorderTest, WindowBoundaryStartsAtPhaseOffset) {
    // 验收标准：记录 tick 带相位偏移，不落在整数分钟边界。
    (void)recorder_.observe(make_snapshot(0.5), kBase + 3);
    ASSERT_TRUE(recorder_.current_bucket_ts().has_value());
    EXPECT_EQ(*recorder_.current_bucket_ts() % 60, 3)
        << "window boundary must be offset from integer minute";
}

TEST_F(HistoryRecorderTest, BatchEmittedWhenWindowChanges) {
    // 在 kBase+3 开窗，跨到下一窗口（kBase+63）时产出上一窗口批次。
    (void)recorder_.observe(make_snapshot(0.5), kBase + 3);
    const auto batch = recorder_.observe(make_snapshot(0.5), kBase + 63);
    ASSERT_FALSE(batch.empty());
    ASSERT_EQ(batch.scalars.size(), 1U);
    EXPECT_EQ(batch.scalars[0].bucket_ts, kBase + 3);
    EXPECT_EQ(batch.scalars[0].level, sysmonstore::kLevelRaw);
    EXPECT_EQ(batch.scalars[0].interval_sec, 60);
}

TEST_F(HistoryRecorderTest, SameWindowAccumulatesAllSamples) {
    // 同一窗口内的样本全部并入聚合（不是只取最后一次）。
    (void)recorder_.observe(make_snapshot(0.10), kBase + 3);
    (void)recorder_.observe(make_snapshot(0.50), kBase + 4);
    (void)recorder_.observe(make_snapshot(0.30), kBase + 5);
    EXPECT_EQ(recorder_.sample_count(), 3);
    const auto batch = recorder_.observe(make_snapshot(0.30), kBase + 63);
    ASSERT_FALSE(batch.empty());
    const auto aggregate = scalar_of(batch, "cpu.usage_ratio");
    EXPECT_EQ(aggregate.count, 3);
    EXPECT_DOUBLE_EQ(aggregate.min, 0.10);
    EXPECT_DOUBLE_EQ(aggregate.max, 0.50);
    EXPECT_DOUBLE_EQ(aggregate.avg, 0.30);
    EXPECT_DOUBLE_EQ(aggregate.last, 0.30);
}

TEST_F(HistoryRecorderTest, PeakPreservedByMax) {
    // 尖峰由 max 保留：19 个低值 + 1 个尖峰，max 必须是尖峰。
    for (int i = 0; i < 19; ++i) {
        (void)recorder_.observe(make_snapshot(0.01), kBase + 3 + i);
    }
    (void)recorder_.observe(make_snapshot(0.99), kBase + 22);
    const auto batch = recorder_.observe(make_snapshot(0.01), kBase + 63);
    const auto aggregate = scalar_of(batch, "cpu.usage_ratio");
    EXPECT_DOUBLE_EQ(aggregate.max, 0.99);
    EXPECT_LT(aggregate.avg, 0.1);
}

TEST_F(HistoryRecorderTest, NewWindowStartsEmptyNotReusingPreviousValues) {
    // 窗口切换后累加状态清零：绝不把上一窗口的数据带进新窗口。
    (void)recorder_.observe(make_snapshot(0.9), kBase + 3);
    (void)recorder_.observe(make_snapshot(0.9), kBase + 63);
    EXPECT_EQ(recorder_.sample_count(), 1);  // 新窗口只有刚观测的这一条
    const auto next = recorder_.observe(make_snapshot(0.1), kBase + 123);
    const auto aggregate = scalar_of(next, "cpu.usage_ratio");
    EXPECT_EQ(aggregate.count, 1);
    EXPECT_DOUBLE_EQ(aggregate.max, 0.9);  // 上一窗口的 0.9 没有串进来
}

// ===== 不可用值不参与聚合 =====

TEST_F(HistoryRecorderTest, UnavailableValuesAreNotObserved) {
    // 契约：不可用（null）不得用 0 冒充——不观测，因此窗口内该指标可能为空。
    auto snapshot = make_snapshot(0.0);
    snapshot.cpu.total.usage_ratio = std::nullopt;
    snapshot.memory = std::nullopt;
    (void)recorder_.observe(snapshot, kBase + 3);
    const auto batch = recorder_.observe(snapshot, kBase + 63);
    EXPECT_TRUE(scalar_of(batch, "cpu.usage_ratio").empty())
        << "null usage_ratio must not be recorded as 0";
    EXPECT_TRUE(scalar_of(batch, "memory.used_bytes").empty())
        << "absent memory domain must stay empty";
}

TEST_F(HistoryRecorderTest, MixedAvailableAndUnavailableOnlyCountsAvailable) {
    auto missing = make_snapshot(0.0);
    missing.cpu.total.usage_ratio = std::nullopt;
    (void)recorder_.observe(make_snapshot(0.4), kBase + 3);
    (void)recorder_.observe(missing, kBase + 4);
    (void)recorder_.observe(make_snapshot(0.6), kBase + 5);
    const auto batch = recorder_.observe(make_snapshot(0.6), kBase + 63);
    const auto aggregate = scalar_of(batch, "cpu.usage_ratio");
    EXPECT_EQ(aggregate.count, 2) << "only available samples count";
    EXPECT_DOUBLE_EQ(aggregate.avg, 0.5) << "average must not be pulled down by the gap";
}

// ===== 缺口标记 =====

TEST_F(HistoryRecorderTest, IncompleteWindowIsMarkedAsGap) {
    // 验收标准：采集中断标记（错过 tick）写入历史，绝不伪造连续数据。
    // 一个满窗应 60 个样本，这里只观测 2 个就跨窗 ⇒ 必须带 sample_gap。
    (void)recorder_.observe(make_snapshot(0.5), kBase + 3);
    (void)recorder_.observe(make_snapshot(0.5), kBase + 4);
    const auto batch = recorder_.observe(make_snapshot(0.5), kBase + 63);
    EXPECT_EQ(count_marks(batch, sysmonstore::kMarkSampleGap), 1)
        << "incomplete window must be marked, not silently padded";
    // detail 记录实际样本数，读取方据此判断窗口可信度。
    for (const auto& mark : batch.marks) {
        if (mark.kind == sysmonstore::kMarkSampleGap) {
            // detail 形式 `<实际样本数>/<明确跳过的 tick 数>`。
            EXPECT_EQ(mark.detail, "2/0");
            EXPECT_EQ(mark.ts, kBase + 3);
        }
    }
}

TEST_F(HistoryRecorderTest, CompleteWindowHasNoGapMark) {
    // 满窗（60 个样本）不应有缺口标记：正常窗口不该被标记污染。
    auto snapshot = make_snapshot(0.5);
    // 先开窗，再补满 60 个样本（含开窗那条）。
    (void)recorder_.observe(snapshot, kBase + 3);
    for (int i = 1; i < 60; ++i) {
        (void)recorder_.observe(snapshot, kBase + 3 + i);
    }
    EXPECT_EQ(recorder_.sample_count(), 60);
    const auto batch = recorder_.observe(snapshot, kBase + 63);
    ASSERT_FALSE(batch.empty());
    EXPECT_EQ(count_marks(batch, sysmonstore::kMarkSampleGap), 0)
        << "complete window must not be marked as gap";
}

TEST_F(HistoryRecorderTest, MissingTicksInWindowAreMarked) {
    // 窗口内缺 tick（进程停顿）：样本数不足满窗 ⇒ 标记缺口。
    auto snapshot = make_snapshot(0.5);
    (void)recorder_.observe(snapshot, kBase + 3);  // 只有 1 个样本
    const auto batch = recorder_.observe(snapshot, kBase + 63);
    EXPECT_EQ(count_marks(batch, sysmonstore::kMarkSampleGap), 1);
}

TEST_F(HistoryRecorderTest, MissedTicksAloneProduceGapMark) {
    // 验收标准：「采集中断标记（…错过 tick）」——计量定时器明确报告跳过 tick 时，
    // 即便窗口样本数看起来够，也必须记缺口。这是与「样本数不足」独立的证据来源。
    auto snapshot = make_snapshot(0.5);
    (void)recorder_.observe(snapshot, kBase + 3);
    for (int i = 1; i < 60; ++i) {
        (void)recorder_.observe(snapshot, kBase + 3 + i);
    }
    EXPECT_TRUE(recorder_.window_is_full(recorder_.sample_count()));
    // 定时器报告跳过了 2 个 tick（进程停顿后被 advance_deadline 检出）。
    recorder_.note_missed_ticks(2);
    const auto batch = recorder_.observe(snapshot, kBase + 63);
    ASSERT_FALSE(batch.empty());
    EXPECT_EQ(count_marks(batch, sysmonstore::kMarkSampleGap), 1)
        << "explicitly reported missed ticks must be marked even if samples look sufficient";
    for (const auto& mark : batch.marks) {
        if (mark.kind == sysmonstore::kMarkSampleGap) {
            EXPECT_EQ(mark.detail, "60/2")
                << "detail must carry both sample count and missed ticks";
        }
    }
}

TEST_F(HistoryRecorderTest, MissedTicksCounterResetsPerWindow) {
    // 缺口计数按窗口结算后清零，否则一次停顿会让之后每个窗口都被误标。
    auto snapshot = make_snapshot(0.5);
    (void)recorder_.observe(snapshot, kBase + 3);
    recorder_.note_missed_ticks(3);
    const auto first = recorder_.observe(snapshot, kBase + 63);
    EXPECT_EQ(count_marks(first, sysmonstore::kMarkSampleGap), 1);
    // 下一个窗口：没有新的 missed tick，且样本数不足仍会标（两个来源独立）。
    const auto second = recorder_.observe(snapshot, kBase + 123);
    for (const auto& mark : second.marks) {
        if (mark.kind == sysmonstore::kMarkSampleGap) {
            EXPECT_EQ(mark.detail, "1/0") << "missed counter must reset after settling a window";
        }
    }
}

// ===== 时钟跳变标记 =====

TEST_F(HistoryRecorderTest, InjectedClockJumpMarkTravelsWithNextBatch) {
    // 调用方检测到跳变后注入标记；标记随下一批次落库（与所描述的数据同事务）。
    (void)recorder_.observe(make_snapshot(0.5), kBase + 3);
    sysmonstore::HistoryMark jump;
    jump.ts = kBase + 30;
    jump.kind = std::string(sysmonstore::kMarkClockJump);
    jump.detail = "30000";
    recorder_.add_mark(jump);
    const auto batch = recorder_.observe(make_snapshot(0.5), kBase + 63);
    EXPECT_EQ(count_marks(batch, sysmonstore::kMarkClockJump), 1);
    // 标记不应重复出现在后续批次里（已随上一批消费掉）。
    const auto next = recorder_.observe(make_snapshot(0.5), kBase + 123);
    EXPECT_EQ(count_marks(next, sysmonstore::kMarkClockJump), 0);
}

TEST_F(HistoryRecorderTest, RestartMarkTravelsWithFlush) {
    // 重启标记由 init_history 注入；退出前 flush 时必须随批次落库
    // （否则「重启窗口」这条事实会丢）。
    (void)recorder_.observe(make_snapshot(0.5), kBase + 3);
    sysmonstore::HistoryMark restart;
    restart.ts = kBase + 3;
    restart.kind = std::string(sysmonstore::kMarkRestart);
    restart.detail = "recover";
    recorder_.add_mark(restart);
    const auto batch = recorder_.flush(kBase + 10);
    EXPECT_EQ(count_marks(batch, sysmonstore::kMarkRestart), 1);
}

// ===== 优雅退出 flush =====

TEST_F(HistoryRecorderTest, FlushEmitsCurrentWindowWithSamples) {
    // 验收标准：优雅退出时 flush 队列并干净关库。退出时窗口通常不满，
    // 但已采到的样本必须落盘（否则「退出前发生了什么」永久缺失）。
    (void)recorder_.observe(make_snapshot(0.7), kBase + 3);
    (void)recorder_.observe(make_snapshot(0.9), kBase + 4);
    const auto batch = recorder_.flush(kBase + 5);
    ASSERT_FALSE(batch.empty());
    ASSERT_EQ(batch.scalars.size(), 1U);
    EXPECT_EQ(batch.scalars[0].bucket_ts, kBase + 3);
    const auto aggregate = scalar_of(batch, "cpu.usage_ratio");
    EXPECT_EQ(aggregate.count, 2);
    EXPECT_DOUBLE_EQ(aggregate.max, 0.9);
    // 不完整窗口必须标记，读取方据此知道均值只基于部分样本。
    EXPECT_EQ(count_marks(batch, sysmonstore::kMarkSampleGap), 1);
}

TEST_F(HistoryRecorderTest, FlushWithNoObservationsEmitsNoWindowData) {
    // 从未观测到样本：不产出空窗口冒充「有数据」。
    const auto batch = recorder_.flush(kBase);
    EXPECT_TRUE(batch.scalars.empty());
    EXPECT_TRUE(batch.dimensions.empty());
}

TEST_F(HistoryRecorderTest, RestartMarkSurvivesFlushWithoutSamples) {
    // 重启标记在「还没采到任何样本就退出」时也必须落库——「重启了」是事实，
    // 不能因为没有窗口数据就被丢掉（否则重启窗口这条记录静默消失）。
    sysmonstore::HistoryMark restart;
    restart.ts = kBase;
    restart.kind = std::string(sysmonstore::kMarkRestart);
    restart.detail = "recover/120";
    recorder_.add_mark(restart);
    const auto batch = recorder_.flush(kBase + 1);
    EXPECT_EQ(count_marks(batch, sysmonstore::kMarkRestart), 1)
        << "restart mark must not be dropped just because no samples were collected";
    EXPECT_TRUE(batch.scalars.empty()) << "still must not fabricate a window";
}

TEST_F(HistoryRecorderTest, FlushTwiceDoesNotDuplicate) {
    (void)recorder_.observe(make_snapshot(0.5), kBase + 3);
    EXPECT_FALSE(recorder_.flush(kBase + 5).empty());
    EXPECT_TRUE(recorder_.flush(kBase + 6).empty()) << "already flushed window must not repeat";
}

// ===== 维度记录 =====

TEST_F(HistoryRecorderTest, CpuCoreUsageIsRecordedPerCore) {
    auto snapshot = make_snapshot(0.5);
    platform::SysmonCpuCore core0;
    core0.index = 0;
    core0.usage_ratio = 0.25;
    core0.saturated = false;
    platform::SysmonCpuCore core1;
    core1.index = 1;
    core1.usage_ratio = 0.95;
    core1.saturated = true;
    snapshot.cpu.cores = {core0, core1};

    (void)recorder_.observe(snapshot, kBase + 3);
    const auto batch = recorder_.observe(snapshot, kBase + 63);
    ASSERT_FALSE(batch.empty());

    int core_rows = 0;
    for (const auto& dimension : batch.dimensions) {
        if (dimension.domain != sysmonstore::kCpuCoreDomain) {
            continue;
        }
        ++core_rows;
        if (dimension.dimension == "0" && dimension.metric == "usage_ratio") {
            EXPECT_DOUBLE_EQ(dimension.aggregate.last, 0.25);
        }
        if (dimension.dimension == "1" && dimension.metric == "saturated") {
            EXPECT_DOUBLE_EQ(dimension.aggregate.max, 1.0) << "saturation must be recorded";
        }
    }
    // 每核 2 个指标（usage_ratio + saturated）。
    EXPECT_EQ(core_rows, 4);
}

TEST_F(HistoryRecorderTest, NetworkCountersRecordedPerInterface) {
    auto snapshot = make_snapshot(0.5);
    platform::SysmonNetworkInfo network;
    network.interface_name = "eno1";
    network.rx_bytes_total = 12345;
    network.tx_bytes_total = 6789;
    network.rx_bytes_per_sec = 100.0;
    network.is_virtual = false;
    snapshot.network = std::vector<platform::SysmonNetworkInfo>{network};

    (void)recorder_.observe(snapshot, kBase + 3);
    const auto batch = recorder_.observe(snapshot, kBase + 63);
    ASSERT_FALSE(batch.empty());

    bool found_total = false;
    bool found_rate = false;
    for (const auto& dimension : batch.dimensions) {
        if (dimension.domain != sysmonstore::kNetworkDomain || dimension.dimension != "eno1") {
            continue;
        }
        if (dimension.metric == "rx_bytes_total") {
            found_total = true;
            EXPECT_DOUBLE_EQ(dimension.aggregate.last, 12345.0);
        }
        if (dimension.metric == "rx_bytes_per_sec") {
            found_rate = true;
            EXPECT_DOUBLE_EQ(dimension.aggregate.max, 100.0);
        }
    }
    EXPECT_TRUE(found_total);
    EXPECT_TRUE(found_rate);
}

TEST_F(HistoryRecorderTest, UnavailableVolumeIsSkippedNotZeroed) {
    // 卷存在但容量读不到：不得记成 0（「读不到」与「已满」是不同事实）。
    auto snapshot = make_snapshot(0.5);
    platform::SysmonVolumeInfo volume;
    volume.mount = "/data";
    volume.available = false;
    volume.reason = "no_permission";
    snapshot.volumes = std::vector<platform::SysmonVolumeInfo>{volume};

    (void)recorder_.observe(snapshot, kBase + 3);
    const auto batch = recorder_.observe(snapshot, kBase + 63);
    for (const auto& dimension : batch.dimensions) {
        EXPECT_NE(dimension.domain, sysmonstore::kVolumeDomain)
            << "unreadable volume must not be recorded at all";
    }
}

TEST_F(HistoryRecorderTest, DynamicDimensionsAppearAndDisappearWithoutIndexAssumption) {
    // 网卡热插拔：维度集合帧间变化，不得按数组下标假设稳定。
    auto snapshot = make_snapshot(0.5);
    platform::SysmonNetworkInfo first;
    first.interface_name = "eno1";
    snapshot.network = std::vector<platform::SysmonNetworkInfo>{first};
    (void)recorder_.observe(snapshot, kBase + 3);

    platform::SysmonNetworkInfo second;
    second.interface_name = "eno2";
    snapshot.network = std::vector<platform::SysmonNetworkInfo>{second};
    (void)recorder_.observe(snapshot, kBase + 4);

    const auto batch = recorder_.observe(make_snapshot(0.5), kBase + 63);
    bool has_eno1 = false;
    bool has_eno2 = false;
    for (const auto& dimension : batch.dimensions) {
        if (dimension.domain != sysmonstore::kNetworkDomain) {
            continue;
        }
        has_eno1 = has_eno1 || dimension.dimension == "eno1";
        has_eno2 = has_eno2 || dimension.dimension == "eno2";
    }
    EXPECT_TRUE(has_eno1);
    EXPECT_TRUE(has_eno2);
}

// ===== 配置 =====

TEST(HistoryRecorderConfigTest, ExpectedSamplesPerWindowFollowsMeasureInterval) {
    HistoryRecorder one_second(
        RecordWindowConfig{.interval_sec = 60, .phase_sec = 0, .measure_interval_ms = 1000});
    EXPECT_EQ(one_second.expected_samples_per_window(), 60);

    HistoryRecorder five_seconds(
        RecordWindowConfig{.interval_sec = 60, .phase_sec = 0, .measure_interval_ms = 5000});
    EXPECT_EQ(five_seconds.expected_samples_per_window(), 12);
}

TEST(HistoryRecorderConfigTest, OneSecondRecordWindowIsSupported) {
    // 验收标准：「记录间隔小于 1 分钟时」需要压缩，因此 1 秒记录窗口必须能正常工作。
    HistoryRecorder recorder(
        RecordWindowConfig{.interval_sec = 1, .phase_sec = 0, .measure_interval_ms = 1000});
    (void)recorder.observe(make_snapshot(0.5), kBase);
    const auto batch = recorder.observe(make_snapshot(0.6), kBase + 1);
    ASSERT_FALSE(batch.empty());
    EXPECT_EQ(batch.scalars[0].interval_sec, 1);
    // 1 秒窗口 + 1 秒计量 = 满窗 1 个样本 ⇒ 无缺口标记。
    EXPECT_EQ(count_marks(batch, sysmonstore::kMarkSampleGap), 0);
}

TEST_F(HistoryRecorderTest, ReconfigureAffectsSubsequentlySettledWindows) {
    // 验收标准（配置票语义）：运行期改记录间隔必须生效。外部可观察承诺是
    // 「此后落库的窗口带新宽度」，且**已累计的样本不丢**（不因改配置而清空窗口）。
    auto snapshot = make_snapshot(0.5);
    (void)recorder_.observe(snapshot, kBase + 3);  // 旧配置（60s）窗口内的样本
    recorder_.reconfigure(
        RecordWindowConfig{.interval_sec = 120, .phase_sec = 3, .measure_interval_ms = 1000});
    // 推进足够远，让新旧网格必然分叉并结算出至少一个窗口。
    std::optional<sysmonstore::HistoryBatch> settled;
    for (int64_t offset = 63; offset <= 63 + 240; offset += 60) {
        auto batch = recorder_.observe(snapshot, kBase + offset);
        if (!batch.empty()) {
            settled = std::move(batch);
            break;
        }
    }
    ASSERT_TRUE(settled.has_value()) << "a window must settle after reconfiguring";
    // 首个结算的窗口是旧配置的窗口（60 秒），它没有被改配置丢掉。
    EXPECT_EQ(settled->scalars[0].interval_sec, 60)
        << "the in-progress window must settle with its original width, not be discarded";
    // 之后的窗口用新宽度（120 秒）。
    bool saw_new_width = false;
    for (int64_t offset = 63 + 60; offset <= 63 + 600; offset += 60) {
        auto batch = recorder_.observe(snapshot, kBase + offset);
        if (!batch.empty() && batch.scalars[0].interval_sec == 120) {
            saw_new_width = true;
            break;
        }
    }
    EXPECT_TRUE(saw_new_width) << "new windows must use the reconfigured interval";
}

TEST(HistoryRecorderConfigTest, InvalidIntervalFallsBackToDefaultSixtySeconds) {
    // 防御：非法间隔（配置校验已拦，但纯逻辑不能因此除零/死循环）。
    // 断言回退值本身（60 秒），而不只是「有个桶」——回退成 0 也会让
    // `current_bucket_ts().has_value()` 为真，那样测不出缺陷。
    HistoryRecorder recorder(
        RecordWindowConfig{.interval_sec = 0, .phase_sec = 0, .measure_interval_ms = 1000});
    EXPECT_EQ(recorder.expected_samples_per_window(), 60) << "fallback interval must be 60s";
    (void)recorder.observe(make_snapshot(0.5), kBase);
    ASSERT_TRUE(recorder.current_bucket_ts().has_value());
    EXPECT_EQ(*recorder.current_bucket_ts() % 60, 0);
}

}  // namespace
}  // namespace dztrader::sysmon
