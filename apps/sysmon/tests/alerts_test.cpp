/**
 * @file alerts_test.cpp
 * @brief 阈值告警状态机与输入构造（契约 sysmon §2.8；#80）。
 *
 * 覆盖票面验收标准：触发 / 冷却 / 恢复、百分比与绝对容量两种形态、
 * 路径感知只命中关注卷；以及「持续满足不重弹」「被抑制活动期不补发」
 * 「读不到不伪造恢复」「配置关闭静默清除」等边界语义。
 *
 * 全部用例确定性驱动（注入墙钟与单调时钟），不依赖真实时钟、不 sleep。
 */

#include "alerts.h"
#include "snapshot_builder.h"

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
constexpr int64_t kWallSec = 1'790'000'000;  // 占位墙钟（只用于 since 断言）
constexpr int64_t kCooldown600s = 600'000;   // 默认 cooldown_sec=600 → 毫秒

platform::SysmonAlertsConfig make_config() {
    return platform::SysmonAlertsConfig{};  // 默认 90/10/90/50/10%/1 GiB/600s
}

AlertInputs memory_input(std::optional<double> ratio, int64_t monotonic_ms, int64_t wall_sec) {
    AlertInputs inputs;
    inputs.memory_applicable = true;
    inputs.memory_used_ratio = ratio;
    inputs.now_monotonic_ms = monotonic_ms;
    inputs.now_unix_sec = wall_sec;
    return inputs;
}

AlertInputs swap_input(std::optional<double> ratio, int64_t monotonic_ms, int64_t wall_sec) {
    AlertInputs inputs;
    inputs.swap_applicable = true;
    inputs.swap_used_ratio = ratio;
    inputs.now_monotonic_ms = monotonic_ms;
    inputs.now_unix_sec = wall_sec;
    return inputs;
}

WatchedVolumeUsage volume_usage(std::string mount, uint64_t total, uint64_t free) {
    WatchedVolumeUsage usage;
    usage.mount = std::move(mount);
    usage.total_bytes = total;
    usage.free_bytes = free;
    return usage;
}

AlertInputs volume_input(std::vector<WatchedVolumeUsage> usages,
                         int64_t monotonic_ms,
                         int64_t wall_sec) {
    AlertInputs inputs;
    inputs.volumes_applicable = true;
    inputs.watch_paths_collected = true;
    for (const auto& usage : usages) {
        inputs.watched_mounts.push_back(usage.mount);
    }
    inputs.watched_volumes = std::move(usages);
    inputs.now_monotonic_ms = monotonic_ms;
    inputs.now_unix_sec = wall_sec;
    return inputs;
}

// ===== 内存：触发 / 持续 / 恢复 =====

TEST(AlertEvaluatorTest, MemoryReachingThresholdTriggersOnce) {
    AlertEvaluator evaluator;
    const auto config = make_config();
    const auto result = evaluator.evaluate(memory_input(0.90, 1000, kWallSec), config);

    ASSERT_EQ(result.notifications.size(), 1U);
    EXPECT_FALSE(result.notifications[0].recovery);
    EXPECT_EQ(result.notifications[0].kind, platform::SYSMON_ALERT_MEMORY_USED);
    EXPECT_TRUE(result.notifications[0].subject.empty());
    EXPECT_NE(result.notifications[0].message.find("90.0%"), std::string::npos);
    ASSERT_EQ(result.active.size(), 1U);
    EXPECT_EQ(result.active[0].since_unix_sec, kWallSec);
}

TEST(AlertEvaluatorTest, MemoryStrictlyBelowThresholdDoesNotTrigger) {
    AlertEvaluator evaluator;
    const auto result = evaluator.evaluate(memory_input(0.899, 1000, kWallSec), make_config());
    EXPECT_TRUE(result.notifications.empty());
    EXPECT_TRUE(result.active.empty());
}

TEST(AlertEvaluatorTest, ContinuousBreachDoesNotRepeatAndKeepsSince) {
    AlertEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 1000, kWallSec), config).notifications.size(),
              1U);
    // 阈值持续满足：其后每个计量 tick 都不重弹，since 保持首次越限时刻。
    for (const auto& [mono, wall] :
         {std::pair<int64_t, int64_t>{2000, kWallSec + 1}, {3000, kWallSec + 2}}) {
        const auto result = evaluator.evaluate(memory_input(0.99, mono, wall), config);
        EXPECT_TRUE(result.notifications.empty());
        ASSERT_EQ(result.active.size(), 1U);
        EXPECT_EQ(result.active[0].since_unix_sec, kWallSec);
    }
}

TEST(AlertEvaluatorTest, RecoveryNotifiesAndClearsActive) {
    AlertEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 1000, kWallSec), config).notifications.size(),
              1U);

    const auto recovered = evaluator.evaluate(memory_input(0.40, 2000, kWallSec + 10), config);
    ASSERT_EQ(recovered.notifications.size(), 1U);
    EXPECT_TRUE(recovered.notifications[0].recovery);
    EXPECT_EQ(recovered.notifications[0].kind, platform::SYSMON_ALERT_MEMORY_USED);
    EXPECT_NE(recovered.notifications[0].message.find("已回到"), std::string::npos);
    EXPECT_TRUE(recovered.active.empty());
}

TEST(AlertEvaluatorTest, RecoveryWithoutPriorTriggerStaysSilent) {
    AlertEvaluator evaluator;
    ASSERT_TRUE(evaluator.evaluate(memory_input(0.20, 1000, kWallSec), make_config())
                    .notifications.empty());
    // 从未触发过的对象不产生恢复通知（没人见过它"坏过"）。
    const auto result = evaluator.evaluate(memory_input(0.10, 2000, kWallSec + 1), make_config());
    EXPECT_TRUE(result.notifications.empty());
    EXPECT_TRUE(result.active.empty());
}

// ===== 冷却防抖 =====

TEST(AlertEvaluatorTest, CooldownSuppressesRetriggerAndSuppressedEpisodeIsNotRetried) {
    AlertEvaluator evaluator;
    auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 0, kWallSec), config).notifications.size(), 1U);
    const auto recovered = evaluator.evaluate(memory_input(0.40, 1'000, kWallSec + 1), config);
    ASSERT_EQ(recovered.notifications.size(), 1U);
    ASSERT_TRUE(recovered.notifications[0].recovery);

    // 恢复后 1s 再次越限（< 600s 冷却）：不弹，但活动状态必须出现（持续状态与通知解耦）。
    const auto suppressed = evaluator.evaluate(memory_input(0.97, 2'000, kWallSec + 2), config);
    EXPECT_TRUE(suppressed.notifications.empty());
    ASSERT_EQ(suppressed.active.size(), 1U);
    EXPECT_EQ(suppressed.active[0].since_unix_sec, kWallSec + 2);

    // 被抑制的活动期即使持续到冷却结束也不补发（阈值持续满足不刷屏）。
    const auto after_cooldown =
        evaluator.evaluate(memory_input(0.97, kCooldown600s + 10'000, kWallSec + 700), config);
    EXPECT_TRUE(after_cooldown.notifications.empty());
    ASSERT_EQ(after_cooldown.active.size(), 1U);
    EXPECT_EQ(after_cooldown.active[0].since_unix_sec, kWallSec + 2);

    // 被抑制的活动期恢复：不发恢复通知（用户从未收到触发通知）。
    const auto silent_recovery =
        evaluator.evaluate(memory_input(0.10, kCooldown600s + 20'000, kWallSec + 800), config);
    EXPECT_TRUE(silent_recovery.notifications.empty());
    EXPECT_TRUE(silent_recovery.active.empty());
}

TEST(AlertEvaluatorTest, RetriggerAfterCooldownNotifiesAgain) {
    AlertEvaluator evaluator;
    auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 0, kWallSec), config).notifications.size(), 1U);
    ASSERT_EQ(
        evaluator.evaluate(memory_input(0.40, 1'000, kWallSec + 1), config).notifications.size(),
        1U);
    // 冷却结束后的再次越限：新活动期，重新通知。
    const auto again =
        evaluator.evaluate(memory_input(0.91, kCooldown600s + 1'000, kWallSec + 700), config);
    ASSERT_EQ(again.notifications.size(), 1U);
    EXPECT_FALSE(again.notifications[0].recovery);
}

TEST(AlertEvaluatorTest, CooldownUsesMonotonicClockNotWallClock) {
    AlertEvaluator evaluator;
    auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 0, kWallSec), config).notifications.size(), 1U);
    ASSERT_EQ(
        evaluator.evaluate(memory_input(0.40, 1'000, kWallSec + 1), config).notifications.size(),
        1U);
    // 墙钟回拨（NTP 步进）不影响冷却：距上次通知的单调时钟仅 1s → 仍应抑制。
    const auto suppressed = evaluator.evaluate(memory_input(0.97, 2'000, kWallSec - 3'600), config);
    EXPECT_TRUE(suppressed.notifications.empty());
    ASSERT_EQ(suppressed.active.size(), 1U);
    // 墙钟前移也不能伪造"冷却结束"：单调时钟才是判据。
    const auto still_suppressed =
        evaluator.evaluate(memory_input(0.97, 3'000, kWallSec + 100'000), config);
    EXPECT_TRUE(still_suppressed.notifications.empty());
}

// ===== 读不到 / 能力关闭 =====

TEST(AlertEvaluatorTest, UnavailableReadingKeepsActiveWithoutRecovery) {
    AlertEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 1'000, kWallSec), config).notifications.size(),
              1U);

    // 本帧读不到（capability 仍在）：保留活动状态、不发恢复通知。
    const auto unavailable =
        evaluator.evaluate(memory_input(std::nullopt, 2'000, kWallSec + 1), config);
    EXPECT_TRUE(unavailable.notifications.empty());
    ASSERT_EQ(unavailable.active.size(), 1U);

    // 恢复读数后仍很高：不重弹（同一活动期）。
    const auto again = evaluator.evaluate(memory_input(0.96, 3'000, kWallSec + 2), config);
    EXPECT_TRUE(again.notifications.empty());
    ASSERT_EQ(again.active.size(), 1U);
    EXPECT_EQ(again.active[0].since_unix_sec, kWallSec);
}

TEST(AlertEvaluatorTest, DisabledCapabilityClearsAlertSilently) {
    AlertEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(memory_input(0.95, 1'000, kWallSec), config).notifications.size(),
              1U);

    AlertInputs disabled = memory_input(std::nullopt, 2'000, kWallSec + 1);
    disabled.memory_applicable = false;
    const auto result = evaluator.evaluate(disabled, config);
    EXPECT_TRUE(result.notifications.empty());  // 配置关闭不是恢复，不发恢复通知
    EXPECT_TRUE(result.active.empty());
}

TEST(AlertEvaluatorTest, SwapTriggersAndClearsWhenSwapRemoved) {
    AlertEvaluator evaluator;
    const auto config = make_config();
    const auto triggered = evaluator.evaluate(swap_input(0.50, 1'000, kWallSec), config);
    ASSERT_EQ(triggered.notifications.size(), 1U);
    EXPECT_EQ(triggered.notifications[0].kind, platform::SYSMON_ALERT_SWAP_USED);

    AlertInputs no_swap = swap_input(std::nullopt, 2'000, kWallSec + 1);
    no_swap.swap_applicable = false;  // 交换被移除 / 配置关闭
    const auto cleared = evaluator.evaluate(no_swap, config);
    EXPECT_TRUE(cleared.notifications.empty());
    EXPECT_TRUE(cleared.active.empty());
}

// ===== 关注卷：两种阈值形态 + 路径感知 + 每个卷独立冷却 =====

TEST(AlertEvaluatorTest, VolumeFreePercentFormTriggers) {
    AlertEvaluator evaluator;
    auto config = make_config();
    config.volume_free_bytes = 0;  // 关闭绝对容量形态，只剩百分比形态
    // 100 GiB 卷剩 5 GiB = 5% < 10% → 触发。
    const auto result = evaluator.evaluate(
        volume_input({volume_usage("/data", 100 * kGiB, 5 * kGiB)}, 1'000, kWallSec), config);
    ASSERT_EQ(result.notifications.size(), 1U);
    EXPECT_EQ(result.notifications[0].kind, platform::SYSMON_ALERT_VOLUME_FREE);
    EXPECT_EQ(result.notifications[0].subject, "/data");
    EXPECT_NE(result.notifications[0].message.find("/data"), std::string::npos);
    ASSERT_EQ(result.active.size(), 1U);
    EXPECT_EQ(result.active[0].subject, "/data");
}

TEST(AlertEvaluatorTest, VolumeFreeAbsoluteFormTriggersEvenWhenPercentPasses) {
    AlertEvaluator evaluator;
    auto config = make_config();
    // 5 GiB 小卷剩 0.9 GiB：18% ≥ 10%（百分比形态不触发），但 0.9 GiB < 1 GiB → 绝对容量形态触发。
    const auto result = evaluator.evaluate(
        volume_input({volume_usage("/small", 5 * kGiB, 921 * 1024 * 1024)}, 1'000, kWallSec),
        config);
    ASSERT_EQ(result.notifications.size(), 1U);
    EXPECT_EQ(result.notifications[0].subject, "/small");
}

TEST(AlertEvaluatorTest, VolumeRecoversOnlyWhenBothFormsSatisfied) {
    AlertEvaluator evaluator;
    auto config = make_config();
    // 触发：100 GiB 卷剩 5 GiB。
    ASSERT_EQ(
        evaluator
            .evaluate(volume_input({volume_usage("/data", 100 * kGiB, 5 * kGiB)}, 1'000, kWallSec),
                      config)
            .notifications.size(),
        1U);
    // 恢复到 20 GiB：20% ≥ 10% 且 20 GiB ≥ 1 GiB → 恢复。
    const auto recovered = evaluator.evaluate(
        volume_input({volume_usage("/data", 100 * kGiB, 20 * kGiB)}, 2'000, kWallSec + 10), config);
    ASSERT_EQ(recovered.notifications.size(), 1U);
    EXPECT_TRUE(recovered.notifications[0].recovery);
    EXPECT_EQ(recovered.notifications[0].subject, "/data");
    EXPECT_TRUE(recovered.active.empty());

    // 只满足一种形态不算恢复：50 GiB 剩余但阈值绝对容量调到 100 GiB 时仍越限。
    config.volume_free_bytes = 100 * kGiB;
    ASSERT_EQ(evaluator
                  .evaluate(volume_input({volume_usage("/data", 100 * kGiB, 15 * kGiB)},
                                         kCooldown600s + 1'000, kWallSec + 20),
                            config)
                  .notifications.size(),
              1U);
    const auto still =
        evaluator.evaluate(volume_input({volume_usage("/data", 100 * kGiB, 50 * kGiB)},
                                        kCooldown600s + 2'000, kWallSec + 30),
                           config);
    EXPECT_TRUE(still.notifications.empty());
    ASSERT_EQ(still.active.size(), 1U);
}

TEST(AlertEvaluatorTest, VolumeAlertsArePerMountAndCooldownIsPerSubject) {
    AlertEvaluator evaluator;
    const auto config = make_config();
    const auto first =
        evaluator.evaluate(volume_input({volume_usage("/a", 10 * kGiB, 512 * 1024 * 1024),
                                         volume_usage("/b", 10 * kGiB, 512 * 1024 * 1024)},
                                        1'000, kWallSec),
                           config);
    ASSERT_EQ(first.notifications.size(), 2U);  // 两个卷各自独立
    EXPECT_EQ(first.active.size(), 2U);

    // 两个都恢复（各一条恢复通知）。
    const auto recovered = evaluator.evaluate(
        volume_input(
            {volume_usage("/a", 10 * kGiB, 5 * kGiB), volume_usage("/b", 10 * kGiB, 5 * kGiB)},
            2'000, kWallSec + 1),
        config);
    ASSERT_EQ(recovered.notifications.size(), 2U);
    EXPECT_TRUE(recovered.notifications[0].recovery);
    EXPECT_TRUE(recovered.notifications[1].recovery);

    // /a 冷区内重触发被抑制；/b 超过冷却后重触发照常通知（冷却按告警对象计，不是全局闸门）。
    // 两个卷都仍在关注集合里（watched_mounts 独立于本帧可读的 watched_volumes）。
    auto per_subject_input = [](WatchedVolumeUsage usage, int64_t monotonic_ms, int64_t wall_sec) {
        AlertInputs inputs;
        inputs.volumes_applicable = true;
        inputs.watch_paths_collected = true;
        inputs.watched_mounts = {"/a", "/b"};
        inputs.watched_volumes = {std::move(usage)};
        inputs.now_monotonic_ms = monotonic_ms;
        inputs.now_unix_sec = wall_sec;
        return inputs;
    };
    const auto suppressed = evaluator.evaluate(
        per_subject_input(volume_usage("/a", 10 * kGiB, 512 * 1024 * 1024), 3'000, kWallSec + 2),
        config);
    EXPECT_TRUE(suppressed.notifications.empty());
    const auto notified =
        evaluator.evaluate(per_subject_input(volume_usage("/b", 10 * kGiB, 512 * 1024 * 1024),
                                             kCooldown600s + 10'000, kWallSec + 700),
                           config);
    ASSERT_EQ(notified.notifications.size(), 1U);
    EXPECT_EQ(notified.notifications[0].subject, "/b");
}

TEST(AlertEvaluatorTest, VolumeNoLongerWatchedIsClearedSilently) {
    AlertEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(
        evaluator
            .evaluate(volume_input({volume_usage("/data", 10 * kGiB, 0)}, 1'000, kWallSec), config)
            .notifications.size(),
        1U);

    // 关注路径配置改动后 /data 不再被关注（落卷解析成功但集合为空）：静默清除、不发恢复。
    AlertInputs changed;
    changed.volumes_applicable = true;
    changed.watch_paths_collected = true;
    changed.now_monotonic_ms = 2'000;
    changed.now_unix_sec = kWallSec + 1;
    const auto result = evaluator.evaluate(changed, config);
    EXPECT_TRUE(result.notifications.empty());
    EXPECT_TRUE(result.active.empty());
}

TEST(AlertEvaluatorTest, VolumeUnreadableKeepsActive) {
    AlertEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(
        evaluator
            .evaluate(volume_input({volume_usage("/data", 10 * kGiB, 0)}, 1'000, kWallSec), config)
            .notifications.size(),
        1U);

    // 卷仍被关注，但本帧容量读不到（watched_mounts 有、watched_volumes 无）：保留活动状态。
    AlertInputs unreadable;
    unreadable.volumes_applicable = true;
    unreadable.watch_paths_collected = true;
    unreadable.watched_mounts = {"/data"};
    unreadable.now_monotonic_ms = 2'000;
    unreadable.now_unix_sec = kWallSec + 1;
    const auto result = evaluator.evaluate(unreadable, config);
    EXPECT_TRUE(result.notifications.empty());
    ASSERT_EQ(result.active.size(), 1U);
    EXPECT_EQ(result.active[0].subject, "/data");
}

// ===== make_alert_inputs：路径感知与口径 =====

platform::RawVolumeInfo raw_volume(std::string mount,
                                   std::optional<uint64_t> total,
                                   std::optional<uint64_t> free) {
    platform::RawVolumeInfo volume;
    volume.mount = std::move(mount);
    volume.total_bytes = total;
    volume.free_bytes = free;
    return volume;
}

platform::RawWatchPathInfo raw_watch_path(std::string path, std::optional<std::string> mount) {
    platform::RawWatchPathInfo info;
    info.path = std::move(path);
    info.mount = std::move(mount);
    if (!info.mount.has_value()) {
        info.reason = "path_not_found";
    }
    return info;
}

TEST(MakeAlertInputsTest, OnlyWatchedVolumesEnterThresholdEvaluation) {
    MemorySwapMetrics memory;
    DiskMetrics disk;
    disk.volumes_collected = true;
    disk.volumes = std::vector<platform::RawVolumeInfo>{
        raw_volume("/", 100 * kGiB, 1 * kGiB),       // 更满，但不是关注卷
        raw_volume("/data", 100 * kGiB, 50 * kGiB),  // 关注卷（默认阈值下不越限）
    };
    disk.watch_paths = std::vector<platform::RawWatchPathInfo>{
        raw_watch_path("/srv/dztrader/shm", "/data"),
        raw_watch_path("/srv/dztrader/db", "/data"),  // 同一卷的第二个关注路径：按 mount 去重
    };

    const auto inputs = make_alert_inputs(memory, disk, kWallSec, 1'000);
    ASSERT_TRUE(inputs.volumes_applicable);
    ASSERT_TRUE(inputs.watch_paths_collected);
    ASSERT_EQ(inputs.watched_mounts.size(), 1U);
    EXPECT_EQ(inputs.watched_mounts[0], "/data");
    ASSERT_EQ(inputs.watched_volumes.size(), 1U);
    EXPECT_EQ(inputs.watched_volumes[0].mount, "/data");

    // 端到端（纯逻辑）：把阈值调到必触发，只有关注卷产生告警——非关注卷 / 再满也不告警。
    auto config = make_config();
    config.volume_free_pct = 100.0;
    config.volume_free_bytes = 0;
    AlertEvaluator evaluator;
    const auto result = evaluator.evaluate(inputs, config);
    ASSERT_EQ(result.notifications.size(), 1U);
    EXPECT_EQ(result.notifications[0].subject, "/data");
    ASSERT_EQ(result.active.size(), 1U);
    EXPECT_EQ(result.active[0].subject, "/data");
}

TEST(MakeAlertInputsTest, UnresolvedWatchPathProducesNoWatchedMount) {
    MemorySwapMetrics memory;
    DiskMetrics disk;
    disk.volumes_collected = true;
    disk.volumes = std::vector<platform::RawVolumeInfo>{raw_volume("/", 100 * kGiB, 1 * kGiB)};
    disk.watch_paths = std::vector<platform::RawWatchPathInfo>{
        raw_watch_path("/srv/dztrader/shm", std::nullopt),  // 解析失败：不得猜卷
    };

    const auto inputs = make_alert_inputs(memory, disk, kWallSec, 1'000);
    EXPECT_TRUE(inputs.watched_mounts.empty());
    EXPECT_TRUE(inputs.watched_volumes.empty());
}

TEST(MakeAlertInputsTest, UnreadableWatchedVolumeKeepsMountButHasNoUsage) {
    MemorySwapMetrics memory;
    DiskMetrics disk;
    disk.volumes_collected = true;
    disk.volumes = std::vector<platform::RawVolumeInfo>{
        raw_volume("/data", std::nullopt, std::nullopt)};  // 卷存在但容量读不到
    disk.watch_paths =
        std::vector<platform::RawWatchPathInfo>{raw_watch_path("/srv/dztrader/shm", "/data")};

    const auto inputs = make_alert_inputs(memory, disk, kWallSec, 1'000);
    ASSERT_EQ(inputs.watched_mounts.size(), 1U);
    EXPECT_EQ(inputs.watched_mounts[0], "/data");
    EXPECT_TRUE(inputs.watched_volumes.empty());  // 不可判定 → 不伪造数值
}

TEST(MakeAlertInputsTest, VolumesDisabledStillCollectsWatchPathsButNoUsages) {
    MemorySwapMetrics memory;
    DiskMetrics disk;
    disk.volumes_disabled = true;
    disk.volumes = std::nullopt;
    disk.watch_paths =
        std::vector<platform::RawWatchPathInfo>{raw_watch_path("/srv/dztrader/shm", "/data")};

    const auto inputs = make_alert_inputs(memory, disk, kWallSec, 1'000);
    EXPECT_FALSE(inputs.volumes_applicable);
    // 落卷解析不受卷采集开关影响（契约 sysmon §2.5）：仍能给出关注卷，供告警恢复后复用。
    ASSERT_EQ(inputs.watched_mounts.size(), 1U);
    EXPECT_TRUE(inputs.watched_volumes.empty());
}

TEST(MakeAlertInputsTest, MemoryAndSwapUseTotalMinusAvailableFree) {
    MemorySwapMetrics memory;
    memory.collected = true;
    memory.memory = platform::RawMemoryInfo{.total_bytes = 32 * kGiB,
                                            .available_bytes = 8 * kGiB,
                                            .cached_bytes = std::nullopt,
                                            .buffers_bytes = std::nullopt};
    memory.swap_collected = true;
    memory.swap_supported = true;
    memory.swap = platform::RawSwapInfo{.total_bytes = 8 * kGiB, .free_bytes = 2 * kGiB};

    const auto inputs = make_alert_inputs(memory, DiskMetrics{}, kWallSec, 1'000);
    EXPECT_TRUE(inputs.memory_applicable);
    ASSERT_TRUE(inputs.memory_used_ratio.has_value());
    EXPECT_DOUBLE_EQ(*inputs.memory_used_ratio, 0.75);  // 已用 = 32 − 8
    EXPECT_TRUE(inputs.swap_applicable);
    ASSERT_TRUE(inputs.swap_used_ratio.has_value());
    EXPECT_DOUBLE_EQ(*inputs.swap_used_ratio, 0.75);  // 已用 = 8 − 2
    EXPECT_EQ(inputs.now_unix_sec, kWallSec);
    EXPECT_EQ(inputs.now_monotonic_ms, 1'000);
}

TEST(MakeAlertInputsTest, DisabledGroupsAndMissingSwapAreNotApplicable) {
    MemorySwapMetrics memory;
    memory.disabled = true;
    memory.memory = platform::RawMemoryInfo{.total_bytes = 32 * kGiB,
                                            .available_bytes = 1 * kGiB,
                                            .cached_bytes = std::nullopt,
                                            .buffers_bytes = std::nullopt};
    memory.swap_supported = false;
    memory.swap = platform::RawSwapInfo{.total_bytes = 0, .free_bytes = 0};

    const auto inputs = make_alert_inputs(memory, DiskMetrics{}, kWallSec, 1'000);
    EXPECT_FALSE(inputs.memory_applicable);
    EXPECT_FALSE(inputs.memory_used_ratio.has_value());
    EXPECT_FALSE(inputs.swap_applicable);
    EXPECT_FALSE(inputs.swap_used_ratio.has_value());
}

TEST(MakeAlertInputsTest, AvailableBytesAboveTotalAreClamped) {
    MemorySwapMetrics memory;
    memory.collected = true;
    memory.memory = platform::RawMemoryInfo{.total_bytes = 1024,
                                            .available_bytes = 4096,
                                            .cached_bytes = std::nullopt,
                                            .buffers_bytes = std::nullopt};
    const auto inputs = make_alert_inputs(memory, DiskMetrics{}, kWallSec, 1'000);
    ASSERT_TRUE(inputs.memory_used_ratio.has_value());
    EXPECT_DOUBLE_EQ(*inputs.memory_used_ratio, 0.0);  // 可用钳到总量 → 已用 0，不出现负数
}

}  // namespace
}  // namespace dztrader::sysmon
