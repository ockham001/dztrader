/**
 * @file thresholds_test.cpp
 * @brief 阈值越限状态机与输入构造（契约 sysmon §2.8；#123）。
 *
 * 覆盖票面验收标准：进入 / 周期重记 / 恢复三态的语义完整保留，原通知冷却与恢复限流机制
 * 删除后由 `log_repeat_sec` 重记间隔取代；以及「读不到不伪造恢复」「配置关闭静默清除」
 * 「卷为全部卷、逐卷一条」「CPU 饱和不按空集判恢复」等边界语义。
 *
 * 全部用例确定性驱动（注入墙钟与单调时钟），不依赖真实时钟、不 sleep、不写日志。
 * 断言对象是**外部可观察的日志记录列表**（级别 / 判定对象 / 状态 / 当前值 / 阈值 /
 * 活动期持续秒数），不触碰私有状态。
 */

#include "thresholds.h"

#include <dztrader/platform/sysmon.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;
constexpr int64_t kWallSec = 1'790'000'000;  // 占位墙钟（只用于活动期起点）
constexpr int64_t kRepeat600s = 600'000;     // 默认 log_repeat_sec=600 → 毫秒

platform::SysmonThresholdsConfig make_config() {
    return platform::SysmonThresholdsConfig{};  // 默认 90/10/90/50/10% / 600s
}

ThresholdInputs memory_input(std::optional<double> ratio, int64_t monotonic_ms, int64_t wall_sec) {
    ThresholdInputs inputs;
    inputs.memory_applicable = true;
    inputs.memory_used_ratio = ratio;
    inputs.now_monotonic_ms = monotonic_ms;
    inputs.now_unix_sec = wall_sec;
    return inputs;
}

ThresholdInputs swap_input(std::optional<double> ratio, int64_t monotonic_ms, int64_t wall_sec) {
    ThresholdInputs inputs;
    inputs.swap_applicable = true;
    inputs.swap_used_ratio = ratio;
    inputs.now_monotonic_ms = monotonic_ms;
    inputs.now_unix_sec = wall_sec;
    return inputs;
}

VolumeUsage volume_usage(std::string mount, uint64_t total, uint64_t free) {
    VolumeUsage usage;
    usage.mount = std::move(mount);
    usage.total_bytes = total;
    usage.free_bytes = free;
    return usage;
}

/// 卷输入：`volumes` 是全部容量可读的卷，`known_mounts` 是同帧枚举到的全部挂载点。
ThresholdInputs volume_input(std::vector<VolumeUsage> usages,
                             int64_t monotonic_ms,
                             int64_t wall_sec) {
    ThresholdInputs inputs;
    inputs.volumes_applicable = true;
    std::vector<std::string> mounts;
    mounts.reserve(usages.size());
    for (const auto& usage : usages) {
        mounts.push_back(usage.mount);
    }
    inputs.known_mounts = std::move(mounts);
    inputs.volumes = std::move(usages);
    inputs.now_monotonic_ms = monotonic_ms;
    inputs.now_unix_sec = wall_sec;
    return inputs;
}

ThresholdInputs cpu_input(bool readable,
                          std::vector<uint32_t> saturated,
                          int64_t monotonic_ms,
                          int64_t wall_sec) {
    ThresholdInputs inputs;
    inputs.cpu_applicable = true;
    inputs.cpu_cores_readable = readable;
    inputs.saturated_cores = std::move(saturated);
    inputs.now_monotonic_ms = monotonic_ms;
    inputs.now_unix_sec = wall_sec;
    return inputs;
}

// ===== 内存：进入 / 恢复 =====

TEST(ThresholdEvaluatorTest, MemoryReachingThresholdLogsEnteredOnce) {
    ThresholdEvaluator evaluator;
    const auto result = evaluator.evaluate(memory_input(0.90, 1000, kWallSec), make_config());

    ASSERT_EQ(result.records.size(), 1U);
    const auto& record = result.records[0];
    EXPECT_EQ(record.state, ThresholdState::Entered);
    EXPECT_EQ(record.level, spdlog::level::warn);  // 进入越限必须是 warning
    EXPECT_EQ(record.key.kind, platform::SYSMON_THRESHOLD_MEMORY_USED);
    EXPECT_TRUE(record.key.subject.empty());
    EXPECT_EQ(record.active_sec, 0);
    EXPECT_DOUBLE_EQ(record.current_value, 0.90);
    ASSERT_TRUE(record.threshold_value.has_value());
    EXPECT_DOUBLE_EQ(*record.threshold_value, 0.90);
    // 日志文本是英文、小写开头、含当前值与阈值。
    EXPECT_NE(record.message.find("threshold exceeded"), std::string::npos);
    EXPECT_NE(record.message.find("current_pct=90.0"), std::string::npos);
    EXPECT_NE(record.message.find("threshold_pct=90.0"), std::string::npos);
}

TEST(ThresholdEvaluatorTest, MemoryStrictlyBelowThresholdLogsNothing) {
    ThresholdEvaluator evaluator;
    const auto result = evaluator.evaluate(memory_input(0.899, 1000, kWallSec), make_config());
    EXPECT_TRUE(result.records.empty());
}

// 契约 sysmon §2.8：进入与恢复各记一条；持续期只在**满间隔**时重记，不是每帧重记。
TEST(ThresholdEvaluatorTest, ContinuousBreachRepeatsOnlyAtInterval) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 0, kWallSec), config).records.size(), 1U);

    // 间隔内（重记边界 600s 之前）：不重记。
    for (const auto monotonic_ms : {1'000, 100'000, 599'999}) {
        EXPECT_TRUE(
            evaluator.evaluate(memory_input(0.99, monotonic_ms, kWallSec), config).records.empty())
            << "monotonic_ms=" << monotonic_ms;
    }

    // 恰好满 600s：记一条「仍越限」，级别 info，带活动期已持续秒数。
    const auto repeated =
        evaluator.evaluate(memory_input(0.99, kRepeat600s, kWallSec + 600), config);
    ASSERT_EQ(repeated.records.size(), 1U);
    EXPECT_EQ(repeated.records[0].state, ThresholdState::Ongoing);
    EXPECT_EQ(repeated.records[0].level, spdlog::level::info);
    EXPECT_EQ(repeated.records[0].active_sec, 600);
    EXPECT_NE(repeated.records[0].message.find("state=ongoing"), std::string::npos);

    // 重记基准是上次记录时刻：下一个 600s 之后再记一条，而不是立刻接着记。
    EXPECT_TRUE(
        evaluator.evaluate(memory_input(0.99, kRepeat600s + 599'999, kWallSec + 1200), config)
            .records.empty());
    const auto second =
        evaluator.evaluate(memory_input(0.99, kRepeat600s * 2, kWallSec + 1200), config);
    ASSERT_EQ(second.records.size(), 1U);
    EXPECT_EQ(second.records[0].state, ThresholdState::Ongoing);
    EXPECT_EQ(second.records[0].active_sec, 1200);
}

// `log_repeat_sec = 0` 表示不重记：一次活动期只有进入与恢复两条（契约 §2.8）。
TEST(ThresholdEvaluatorTest, ZeroRepeatDisablesOngoingRecords) {
    ThresholdEvaluator evaluator;
    auto config = make_config();
    config.log_repeat_sec = 0;
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 0, kWallSec), config).records.size(), 1U);
    for (const auto monotonic_ms : {kRepeat600s, kRepeat600s * 10, kRepeat600s * 100}) {
        EXPECT_TRUE(
            evaluator.evaluate(memory_input(0.99, monotonic_ms, kWallSec), config).records.empty());
    }
    const auto recovered =
        evaluator.evaluate(memory_input(0.10, kRepeat600s * 101, kWallSec + 1), config);
    ASSERT_EQ(recovered.records.size(), 1U);
    EXPECT_EQ(recovered.records[0].state, ThresholdState::Recovered);
}

TEST(ThresholdEvaluatorTest, RecoveryLogsOnceWithActiveDuration) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 0, kWallSec), config).records.size(), 1U);

    const auto recovered = evaluator.evaluate(memory_input(0.40, 42'000, kWallSec + 42), config);
    ASSERT_EQ(recovered.records.size(), 1U);
    EXPECT_EQ(recovered.records[0].state, ThresholdState::Recovered);
    EXPECT_EQ(recovered.records[0].level, spdlog::level::info);
    EXPECT_EQ(recovered.records[0].active_sec, 42);
    EXPECT_NE(recovered.records[0].message.find("threshold recovered"), std::string::npos);

    // 恢复后再低也不重复记恢复（活动期已结束）。
    EXPECT_TRUE(
        evaluator.evaluate(memory_input(0.10, 43'000, kWallSec + 43), config).records.empty());
}

TEST(ThresholdEvaluatorTest, RecoveryWithoutPriorBreachStaysSilent) {
    ThresholdEvaluator evaluator;
    EXPECT_TRUE(
        evaluator.evaluate(memory_input(0.20, 1000, kWallSec), make_config()).records.empty());
    // 从未越限过的对象不产生恢复记录（没人见过它"坏过"）。
    EXPECT_TRUE(
        evaluator.evaluate(memory_input(0.10, 2000, kWallSec + 1), make_config()).records.empty());
}

// 原通知冷却与恢复限流已删除（不再有通知可限流）：抖动不再被"冷却"吞掉，
// 每一次真实的活动期都有进入与恢复两条记录——这正是日志形态想要的（契约 §2.8）。
TEST(ThresholdEvaluatorTest, EveryEpisodeIsLoggedWithoutCooldownSuppression) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    int64_t monotonic_ms = 0;
    for (int episode = 0; episode < 3; ++episode) {
        const auto entered = evaluator.evaluate(memory_input(0.97, monotonic_ms, kWallSec), config);
        ASSERT_EQ(entered.records.size(), 1U) << "episode=" << episode;
        EXPECT_EQ(entered.records[0].state, ThresholdState::Entered);
        monotonic_ms += 1'000;
        const auto recovered =
            evaluator.evaluate(memory_input(0.10, monotonic_ms, kWallSec + 1), config);
        ASSERT_EQ(recovered.records.size(), 1U) << "episode=" << episode;
        EXPECT_EQ(recovered.records[0].state, ThresholdState::Recovered);
        monotonic_ms += 1'000;
    }
}

// 重记计时用单调时钟：墙钟回拨/前移都不改变重记节奏（与旧冷却同一纪律）。
TEST(ThresholdEvaluatorTest, RepeatUsesMonotonicClockNotWallClock) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 0, kWallSec), config).records.size(), 1U);

    // 墙钟回拨一整天：单调时钟只走了 1s → 不该重记。
    EXPECT_TRUE(
        evaluator.evaluate(memory_input(0.97, 1'000, kWallSec - 86'400), config).records.empty());
    // 墙钟前移一整天但单调时钟仍不足间隔：同样不重记。
    EXPECT_TRUE(
        evaluator.evaluate(memory_input(0.97, 2'000, kWallSec + 86'400), config).records.empty());
    // 单调时钟满间隔：重记，活动期秒数按单调时钟算（不受墙钟跳变污染）。
    const auto repeated =
        evaluator.evaluate(memory_input(0.97, kRepeat600s, kWallSec - 86'400), config);
    ASSERT_EQ(repeated.records.size(), 1U);
    EXPECT_EQ(repeated.records[0].active_sec, 600);
}

// 运行期改 `log_repeat_sec`：下一次重记从**新间隔**起算（基准是上次记录时刻，不是固定网格）。
TEST(ThresholdEvaluatorTest, RepeatIntervalChangeTakesEffectNextRecord) {
    ThresholdEvaluator evaluator;
    auto config = make_config();
    config.log_repeat_sec = 100;
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 0, kWallSec), config).records.size(), 1U);
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 100'000, kWallSec), config).records.size(), 1U);

    // 调大到 600s：距上次记录 100s 时不该重记（新间隔从上次记录起算）。
    config.log_repeat_sec = 600;
    EXPECT_TRUE(evaluator.evaluate(memory_input(0.95, 200'000, kWallSec), config).records.empty());
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 700'000, kWallSec), config).records.size(), 1U);
}

// ===== 读不到 / 能力关闭 =====

TEST(ThresholdEvaluatorTest, UnavailableReadingKeepsActiveWithoutRecovery) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 0, kWallSec), config).records.size(), 1U);

    // 本帧读不到（能力仍在）：保留活动状态、不写恢复记录。
    const auto unavailable =
        evaluator.evaluate(memory_input(std::nullopt, 2'000, kWallSec + 1), config);
    EXPECT_TRUE(unavailable.records.empty());

    // 恢复读数后仍很高：同一活动期，不重记进入。
    const auto again = evaluator.evaluate(memory_input(0.96, 3'000, kWallSec + 2), config);
    EXPECT_TRUE(again.records.empty());

    // 真正回落后恢复记录的持续秒数按活动期起点算（跨越采集缺口也如实反映总时长）。
    const auto recovered = evaluator.evaluate(memory_input(0.10, 90'000, kWallSec + 90), config);
    ASSERT_EQ(recovered.records.size(), 1U);
    EXPECT_EQ(recovered.records[0].state, ThresholdState::Recovered);
    EXPECT_EQ(recovered.records[0].active_sec, 90);
}

TEST(ThresholdEvaluatorTest, DisabledCapabilityClearsSilently) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 1'000, kWallSec), config).records.size(), 1U);

    ThresholdInputs disabled = memory_input(std::nullopt, 2'000, kWallSec + 1);
    disabled.memory_applicable = false;
    const auto result = evaluator.evaluate(disabled, config);
    EXPECT_TRUE(result.records.empty());  // 配置关闭不是恢复，不写恢复记录

    // 重新开启后是全新活动期：再次越限要重新记一条进入（不是"接着上次"）。
    const auto reenabled = evaluator.evaluate(memory_input(0.95, 3'000, kWallSec + 2), config);
    ASSERT_EQ(reenabled.records.size(), 1U);
    EXPECT_EQ(reenabled.records[0].state, ThresholdState::Entered);
}

TEST(ThresholdEvaluatorTest, SwapEntersAndClearsWhenSwapRemoved) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    const auto entered = evaluator.evaluate(swap_input(0.50, 1'000, kWallSec), config);
    ASSERT_EQ(entered.records.size(), 1U);
    EXPECT_EQ(entered.records[0].key.kind, platform::SYSMON_THRESHOLD_SWAP_USED);
    EXPECT_EQ(entered.records[0].state, ThresholdState::Entered);

    ThresholdInputs no_swap = swap_input(std::nullopt, 2'000, kWallSec + 1);
    no_swap.swap_applicable = false;  // 交换被移除 / 配置关闭
    EXPECT_TRUE(evaluator.evaluate(no_swap, config).records.empty());
}

// ===== 卷：全部卷、逐卷一条、只按百分比 =====

TEST(ThresholdEvaluatorTest, VolumeFreePercentTriggersPerMount) {
    ThresholdEvaluator evaluator;
    // 100 GiB 卷剩 5 GiB = 5% 空闲 < 10% → 越限。
    const auto result = evaluator.evaluate(
        volume_input({volume_usage("/data", 100 * kGiB, 5 * kGiB)}, 1'000, kWallSec),
        make_config());
    ASSERT_EQ(result.records.size(), 1U);
    EXPECT_EQ(result.records[0].key.kind, platform::SYSMON_THRESHOLD_VOLUME_FREE);
    EXPECT_EQ(result.records[0].key.subject, "/data");
    EXPECT_NE(result.records[0].message.find("subject=/data"), std::string::npos);
    EXPECT_DOUBLE_EQ(result.records[0].current_value, 0.05);
    ASSERT_TRUE(result.records[0].threshold_value.has_value());
    EXPECT_DOUBLE_EQ(*result.records[0].threshold_value, 0.10);
}

// 契约 §2.8 边界：卷"恰好等于阈值"不算越限（`free_pct < volume_free_pct`），且满足恢复。
TEST(ThresholdEvaluatorTest, VolumeExactlyAtThresholdIsNotBreached) {
    ThresholdEvaluator evaluator;
    const auto result = evaluator.evaluate(
        volume_input({volume_usage("/data", 100 * kGiB, 10 * kGiB)}, 1'000, kWallSec),
        make_config());
    EXPECT_TRUE(result.records.empty());
}

// ADR 0034 的核心动机：绝对容量形态已删除，小分区（ESP / /boot）空闲时不得永久越限。
TEST(ThresholdEvaluatorTest, SmallVolumeWithPlentyOfFreeSpaceIsNotBreached) {
    ThresholdEvaluator evaluator;
    // 512 MiB 的 ESP 剩 508 MiB（99.2% 空闲）：旧实现下 508 MiB < 1 GiB 会永久越限。
    const uint64_t esp_total = 512 * 1024 * 1024;
    const uint64_t esp_free = 508 * 1024 * 1024;
    EXPECT_TRUE(evaluator
                    .evaluate(volume_input({volume_usage("/boot/efi", esp_total, esp_free)}, 1'000,
                                           kWallSec),
                              make_config())
                    .records.empty());
}

// 判定范围恒为本机**全部卷**：没有"关注卷"这一层，任何卷越限都要记。
TEST(ThresholdEvaluatorTest, AllVolumesAreJudgedNotJustASubset) {
    ThresholdEvaluator evaluator;
    const auto result = evaluator.evaluate(
        volume_input(
            {volume_usage("/", 100 * kGiB, 50 * kGiB), volume_usage("/data", 100 * kGiB, 1 * kGiB),
             volume_usage("/boot/efi", 512 * 1024 * 1024, 1 * 1024 * 1024)},
            1'000, kWallSec),
        make_config());
    ASSERT_EQ(result.records.size(), 2U);
    EXPECT_EQ(result.records[0].key.subject, "/boot/efi");  // 按挂载点升序
    EXPECT_EQ(result.records[1].key.subject, "/data");
}

TEST(ThresholdEvaluatorTest, VolumeRecoversOnlyWhenFreeRisesAboveThreshold) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(
        evaluator
            .evaluate(volume_input({volume_usage("/data", 100 * kGiB, 5 * kGiB)}, 0, kWallSec),
                      config)
            .records.size(),
        1U);
    // 空闲升到 20% ≥ 10% → 恢复。
    const auto recovered = evaluator.evaluate(
        volume_input({volume_usage("/data", 100 * kGiB, 20 * kGiB)}, 1'000, kWallSec + 1), config);
    ASSERT_EQ(recovered.records.size(), 1U);
    EXPECT_EQ(recovered.records[0].state, ThresholdState::Recovered);
    EXPECT_EQ(recovered.records[0].key.subject, "/data");
}

// 卷从 volumes[] 消失（卸载/热拔）是**静默清除**，不是恢复（判定对象不再存在）。
TEST(ThresholdEvaluatorTest, VolumeDisappearingIsClearedSilently) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(
        evaluator.evaluate(volume_input({volume_usage("/data", 10 * kGiB, 0)}, 0, kWallSec), config)
            .records.size(),
        1U);

    ThresholdInputs removed = volume_input({}, 1'000, kWallSec + 1);
    removed.known_mounts = std::vector<std::string>{};  // 本帧枚举成功但该卷已不在
    EXPECT_TRUE(evaluator.evaluate(removed, config).records.empty());

    // 卷重新出现：全新活动期，重新记进入。
    const auto reappeared = evaluator.evaluate(
        volume_input({volume_usage("/data", 10 * kGiB, 0)}, 2'000, kWallSec + 2), config);
    ASSERT_EQ(reappeared.records.size(), 1U);
    EXPECT_EQ(reappeared.records[0].state, ThresholdState::Entered);
}

// 卷域整体不可读（known_mounts = nullopt）时**不得**据此静默清除：
// "本帧枚举不到"与"卷已消失"是两件事（契约 §2.8 的"读不到不伪造恢复"同源纪律）。
TEST(ThresholdEvaluatorTest, UnreadableVolumeDomainKeepsActive) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(
        evaluator.evaluate(volume_input({volume_usage("/data", 10 * kGiB, 0)}, 0, kWallSec), config)
            .records.size(),
        1U);

    ThresholdInputs unreadable;
    unreadable.volumes_applicable = true;
    unreadable.known_mounts = std::nullopt;  // 本帧卷域整体读不到
    unreadable.now_monotonic_ms = 1'000;
    unreadable.now_unix_sec = kWallSec + 1;
    EXPECT_TRUE(evaluator.evaluate(unreadable, config).records.empty());

    // 恢复后仍越限：同一活动期，不重记进入。
    const auto still = evaluator.evaluate(
        volume_input({volume_usage("/data", 10 * kGiB, 0)}, 2'000, kWallSec + 2), config);
    EXPECT_TRUE(still.records.empty());
}

// ===== CPU 饱和：整机一个判定对象 =====

TEST(ThresholdEvaluatorTest, CpuSaturationEntersAndRecovers) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    const auto entered = evaluator.evaluate(cpu_input(true, {1, 3}, 0, kWallSec), config);
    ASSERT_EQ(entered.records.size(), 1U);
    EXPECT_EQ(entered.records[0].key.kind, platform::SYSMON_THRESHOLD_CPU_SATURATED);
    EXPECT_EQ(entered.records[0].state, ThresholdState::Entered);
    EXPECT_EQ(entered.records[0].level, spdlog::level::warn);

    // 整机不再有饱和核（且每核使用率确实可读）→ 恢复。
    const auto recovered = evaluator.evaluate(cpu_input(true, {}, 1'000, kWallSec + 1), config);
    ASSERT_EQ(recovered.records.size(), 1U);
    EXPECT_EQ(recovered.records[0].state, ThresholdState::Recovered);
}

// CPU 饱和没有"百分比阈值"语义（饱和判定在采集阶段完成）：记录如实报饱和核数，
// 不编一个 current_pct/threshold_pct（那会让日志读起来像"CPU 用了多少"）。
TEST(ThresholdEvaluatorTest, CpuSaturationRecordReportsSaturatedCoreCount) {
    ThresholdEvaluator evaluator;
    const auto result = evaluator.evaluate(cpu_input(true, {0, 2, 5}, 0, kWallSec), make_config());
    ASSERT_EQ(result.records.size(), 1U);
    const auto& record = result.records[0];
    EXPECT_DOUBLE_EQ(record.current_value, 3.0);  // 3 个饱和核
    EXPECT_FALSE(record.threshold_value.has_value());
    EXPECT_NE(record.message.find("saturated_cores=3"), std::string::npos);
    EXPECT_EQ(record.message.find("current_pct"), std::string::npos);
}

// 契约 §2.8 明文：`cpu_saturated` **不得**按「saturated_cores 为空」判恢复——
// 每核序列整体未采到时 saturated 全部为 false，那只是"本帧无信息"。
TEST(ThresholdEvaluatorTest, CpuSaturationDoesNotRecoverWhenCoresUnreadable) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(cpu_input(true, {2}, 0, kWallSec), config).records.size(), 1U);

    // CPU 采集失败：每核使用率全部不可读、saturated 为空 → 保留活动状态，不写恢复。
    EXPECT_TRUE(
        evaluator.evaluate(cpu_input(false, {}, 1'000, kWallSec + 1), config).records.empty());

    // 采集恢复且确实没有饱和核 → 此时才算恢复。
    const auto recovered = evaluator.evaluate(cpu_input(true, {}, 2'000, kWallSec + 2), config);
    ASSERT_EQ(recovered.records.size(), 1U);
    EXPECT_EQ(recovered.records[0].state, ThresholdState::Recovered);
}

TEST(ThresholdEvaluatorTest, CpuGroupDisabledClearsSilently) {
    ThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(cpu_input(true, {0}, 0, kWallSec), config).records.size(), 1U);

    ThresholdInputs disabled = cpu_input(true, {}, 1'000, kWallSec + 1);
    disabled.cpu_applicable = false;  // sampling.groups.cpu = false
    EXPECT_TRUE(evaluator.evaluate(disabled, config).records.empty());
}

// ===== make_threshold_inputs：口径与可判定性 =====

platform::RawVolumeInfo raw_volume(std::string mount,
                                   std::optional<uint64_t> total,
                                   std::optional<uint64_t> free) {
    platform::RawVolumeInfo volume;
    volume.mount = std::move(mount);
    volume.total_bytes = total;
    volume.free_bytes = free;
    return volume;
}

TEST(MakeThresholdInputsTest, MemoryAndSwapUseTotalMinusAvailableFree) {
    MemorySwapMetrics memory;
    memory.collected = true;
    memory.memory = platform::RawMemoryInfo{.total_bytes = 32 * kGiB,
                                            .available_bytes = 8 * kGiB,
                                            .cached_bytes = std::nullopt,
                                            .buffers_bytes = std::nullopt};
    memory.swap_collected = true;
    memory.swap_supported = true;
    memory.swap = platform::RawSwapInfo{.total_bytes = 8 * kGiB, .free_bytes = 2 * kGiB};

    const auto inputs = make_threshold_inputs(CpuMetrics{}, memory, DiskMetrics{}, kWallSec, 1'000);
    EXPECT_TRUE(inputs.memory_applicable);
    ASSERT_TRUE(inputs.memory_used_ratio.has_value());
    EXPECT_DOUBLE_EQ(*inputs.memory_used_ratio, 0.75);  // 已用 = 32 − 8
    EXPECT_TRUE(inputs.swap_applicable);
    ASSERT_TRUE(inputs.swap_used_ratio.has_value());
    EXPECT_DOUBLE_EQ(*inputs.swap_used_ratio, 0.75);  // 已用 = 8 − 2
    EXPECT_EQ(inputs.now_unix_sec, kWallSec);
    EXPECT_EQ(inputs.now_monotonic_ms, 1'000);
}

TEST(MakeThresholdInputsTest, DisabledGroupsAndMissingSwapAreNotApplicable) {
    MemorySwapMetrics memory;
    memory.disabled = true;
    memory.memory = platform::RawMemoryInfo{.total_bytes = 32 * kGiB,
                                            .available_bytes = 1 * kGiB,
                                            .cached_bytes = std::nullopt,
                                            .buffers_bytes = std::nullopt};
    memory.swap_supported = false;
    memory.swap = platform::RawSwapInfo{.total_bytes = 0, .free_bytes = 0};

    const auto inputs = make_threshold_inputs(CpuMetrics{}, memory, DiskMetrics{}, kWallSec, 1'000);
    EXPECT_FALSE(inputs.memory_applicable);
    EXPECT_FALSE(inputs.memory_used_ratio.has_value());
    EXPECT_FALSE(inputs.swap_applicable);
    EXPECT_FALSE(inputs.swap_used_ratio.has_value());
}

TEST(MakeThresholdInputsTest, AvailableBytesAboveTotalAreClamped) {
    MemorySwapMetrics memory;
    memory.collected = true;
    memory.memory = platform::RawMemoryInfo{.total_bytes = 1024,
                                            .available_bytes = 4096,
                                            .cached_bytes = std::nullopt,
                                            .buffers_bytes = std::nullopt};
    const auto inputs = make_threshold_inputs(CpuMetrics{}, memory, DiskMetrics{}, kWallSec, 1'000);
    ASSERT_TRUE(inputs.memory_used_ratio.has_value());
    EXPECT_DOUBLE_EQ(*inputs.memory_used_ratio, 0.0);  // 可用钳到总量 → 已用 0，不出现负数
}

// 卷输入：全部可读卷都进来（不再有"关注卷"筛选）；总量为 0 的卷不参与判定但进 known_mounts。
TEST(MakeThresholdInputsTest, AllReadableVolumesEnterAndZeroTotalIsSkipped) {
    DiskMetrics disk;
    disk.volumes_collected = true;
    disk.volumes = std::vector<platform::RawVolumeInfo>{
        raw_volume("/", 100 * kGiB, 1 * kGiB),
        raw_volume("/data", 100 * kGiB, 90 * kGiB),
        raw_volume("/zero", 0, 0),  // 总量为 0：无法算百分比，不参与判定
        raw_volume("/unreadable", std::nullopt, std::nullopt),  // 容量读不到：不参与判定
    };

    const auto inputs =
        make_threshold_inputs(CpuMetrics{}, MemorySwapMetrics{}, disk, kWallSec, 1'000);
    ASSERT_TRUE(inputs.volumes_applicable);
    ASSERT_EQ(inputs.volumes.size(), 2U);
    EXPECT_EQ(inputs.volumes[0].mount, "/");  // 按挂载点升序
    EXPECT_EQ(inputs.volumes[1].mount, "/data");
    // 不可判定的挂载点仍算"本帧存在"，否则它们会被误判成"卷消失"而静默清除活动状态。
    ASSERT_TRUE(inputs.known_mounts.has_value());
    EXPECT_EQ(inputs.known_mounts->size(), 4U);
}

TEST(MakeThresholdInputsTest, UncollectedVolumeDomainLeavesKnownMountsUnset) {
    DiskMetrics disk;
    disk.volumes_disabled = true;
    disk.volumes = std::nullopt;
    const auto inputs =
        make_threshold_inputs(CpuMetrics{}, MemorySwapMetrics{}, disk, kWallSec, 1'000);
    EXPECT_FALSE(inputs.volumes_applicable);
    EXPECT_TRUE(inputs.volumes.empty());
    EXPECT_FALSE(inputs.known_mounts.has_value());
}

TEST(MakeThresholdInputsTest, VolumesDisabledButEnumeratedStillMarksApplicabilityFalse) {
    DiskMetrics disk;
    disk.volumes_disabled = true;
    disk.volumes = std::vector<platform::RawVolumeInfo>{raw_volume("/data", 10 * kGiB, 0)};
    const auto inputs =
        make_threshold_inputs(CpuMetrics{}, MemorySwapMetrics{}, disk, kWallSec, 1'000);
    EXPECT_FALSE(inputs.volumes_applicable);
}

// CPU：`cpu_cores_readable` 必须反映"本帧每核使用率确实有值"，而不是 saturated 是否为空。
TEST(MakeThresholdInputsTest, CpuReadabilityTracksCoreUsageNotSaturation) {
    CpuMetrics cpu;
    cpu.cores = {
        platform::SysmonCpuCore{.index = 0, .usage_ratio = std::nullopt, .saturated = false},
        platform::SysmonCpuCore{.index = 1, .usage_ratio = 0.4, .saturated = false}};
    const auto readable =
        make_threshold_inputs(cpu, MemorySwapMetrics{}, DiskMetrics{}, kWallSec, 1'000);
    EXPECT_TRUE(readable.cpu_applicable);
    EXPECT_TRUE(readable.cpu_cores_readable);

    // 每核全不可读：不是"已恢复"，是"本帧无信息"。
    cpu.cores = {
        platform::SysmonCpuCore{.index = 0, .usage_ratio = std::nullopt, .saturated = false}};
    const auto unreadable =
        make_threshold_inputs(cpu, MemorySwapMetrics{}, DiskMetrics{}, kWallSec, 1'000);
    EXPECT_FALSE(unreadable.cpu_cores_readable);

    // 配置关闭：applicable=false（静默清除），与"读不到"区分。
    cpu.disabled = true;
    const auto disabled =
        make_threshold_inputs(cpu, MemorySwapMetrics{}, DiskMetrics{}, kWallSec, 1'000);
    EXPECT_FALSE(disabled.cpu_applicable);
}

// 端到端（纯逻辑）：把阈值调到必触发，全部卷都产生记录——不再只有"关注卷"。
TEST(MakeThresholdInputsTest, AllVolumesProduceRecordsUnderTightThreshold) {
    DiskMetrics disk;
    disk.volumes_collected = true;
    disk.volumes = std::vector<platform::RawVolumeInfo>{raw_volume("/", 100 * kGiB, 60 * kGiB),
                                                        raw_volume("/data", 100 * kGiB, 50 * kGiB)};
    const auto inputs =
        make_threshold_inputs(CpuMetrics{}, MemorySwapMetrics{}, disk, kWallSec, 1'000);

    auto config = make_config();
    config.volume_free_pct = 100.0;  // 任何卷都"空闲不足 100%"，必然触发
    ThresholdEvaluator evaluator;
    const auto result = evaluator.evaluate(inputs, config);
    ASSERT_EQ(result.records.size(), 2U);
    EXPECT_EQ(result.records[0].key.subject, "/");
    EXPECT_EQ(result.records[1].key.subject, "/data");
}

}  // namespace
}  // namespace dztrader::sysmon
