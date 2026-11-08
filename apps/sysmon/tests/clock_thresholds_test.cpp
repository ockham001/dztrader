/**
 * @file clock_thresholds_test.cpp
 * @brief 时间判定对象 `clock_offset` / `clock_bound` 的判定（契约 sysmon §2.8；#301）。
 *
 * 覆盖票面验收标准：双档偏差判定（warn 黄 / error 红 + `NOTIFY_UI`）、独立质量判据、
 * 进入/持续重记/恢复、读不到不伪造恢复、配置关闭静默清除、边界值语义（等于阈值算越限）。
 *
 * 全部用例确定性驱动（注入墙钟与单调时钟、注入假探测结果），不依赖真实时钟与真实网络、
 * 不 sleep、不写日志。断言的**外部可观察产物**是「待写日志记录列表 + 是否要求弹窗」，
 * 不触碰私有状态。
 */

#include "clock_thresholds.h"

#include <dztrader/platform/sysmon.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace dztrader::sysmon {
namespace {

constexpr int64_t kWallSec = 1'790'000'000;  // 占位墙钟（只用于活动期起点）
constexpr int64_t kRepeat600s = 600'000;     // 默认 log_repeat_sec=600 → 毫秒

/// 默认配置：warn 100 / error 1000 / bound_alarm 200 / repeat 600s（契约 §3.1 默认值）。
ClockThresholdConfig make_config() { return ClockThresholdConfig{}; }

ClockThresholdInputs probe_input(std::optional<double> offset_ms,
                                 std::optional<double> bound_ms,
                                 int64_t monotonic_ms,
                                 int64_t wall_sec) {
    ClockThresholdInputs inputs;
    inputs.applicable = true;
    inputs.offset_ms = offset_ms;
    inputs.bound_ms = bound_ms;
    inputs.now_monotonic_ms = monotonic_ms;
    inputs.now_unix_sec = wall_sec;
    return inputs;
}

/// 只带误差界、偏差在阈值内：用于单独验证质量判据（不触发偏差判定）。
ClockThresholdInputs bound_only(double bound_ms, int64_t monotonic_ms, int64_t wall_sec) {
    return probe_input(0.0, bound_ms, monotonic_ms, wall_sec);
}

// ===== clock_offset：双档判定 =====

// AC1：越过 warn 阈值 → warning 日志 + 黄色标记（不弹窗）。
TEST(ClockThresholdEvaluatorTest, OffsetAtWarnThresholdLogsWarningWithoutPopup) {
    ClockThresholdEvaluator evaluator;
    // 恰好等于 warn 阈值算越限（契约 §2.8「相等按表处理」：偏差 ≥ 阈值）。
    const auto result =
        evaluator.evaluate(probe_input(100.0, 10.0, 1'000, kWallSec), make_config());

    ASSERT_EQ(result.records.size(), 1U);
    const auto& record = result.records[0];
    EXPECT_EQ(record.key.kind, platform::SYSMON_THRESHOLD_CLOCK_OFFSET);
    EXPECT_EQ(record.transition, ClockThresholdTransition::EnteredWarning);
    EXPECT_EQ(record.level, spdlog::level::warn);
    EXPECT_FALSE(record.notify_popup) << "warn tier must never pop up (ADR 0047)";
    EXPECT_TRUE(record.notify_message.empty());
    EXPECT_EQ(record.active_sec, 0);
    EXPECT_EQ(record.since_unix_sec, kWallSec);
    ASSERT_TRUE(record.offset_ms.has_value());
    EXPECT_DOUBLE_EQ(*record.offset_ms, 100.0);
    // 阈值义务含两档（契约 §2.8）。
    EXPECT_EQ(record.offset_warn_ms, 100);
    EXPECT_EQ(record.offset_error_ms, 1000);
    EXPECT_NE(record.message.find("clock offset threshold exceeded"), std::string::npos);
    EXPECT_NE(record.message.find("offset_ms=+100.0"), std::string::npos);
    EXPECT_NE(record.message.find("warn_ms=100"), std::string::npos);
    EXPECT_NE(record.message.find("error_ms=1000"), std::string::npos);
    EXPECT_NE(record.message.find("since=1790000000"), std::string::npos);
}

// 低于 warn 阈值：不产生记录（未越限，不建状态）。
TEST(ClockThresholdEvaluatorTest, OffsetBelowWarnThresholdLogsNothing) {
    ClockThresholdEvaluator evaluator;
    EXPECT_TRUE(evaluator.evaluate(probe_input(99.9, 10.0, 1'000, kWallSec), make_config())
                    .records.empty());
    // 负向同理：判定取绝对值。
    EXPECT_TRUE(evaluator.evaluate(probe_input(-99.9, 10.0, 2'000, kWallSec), make_config())
                    .records.empty());
}

// AC1：越过 error 阈值 → error 日志 + NOTIFY_UI（popup=true）。
TEST(ClockThresholdEvaluatorTest, OffsetAtErrorThresholdLogsErrorAndPopsUp) {
    ClockThresholdEvaluator evaluator;
    // 恰好等于 error 阈值算越限（≥）；负向偏差同样按绝对值判档。
    const auto result =
        evaluator.evaluate(probe_input(-1000.0, 10.0, 1'000, kWallSec), make_config());

    ASSERT_EQ(result.records.size(), 1U);
    const auto& record = result.records[0];
    EXPECT_EQ(record.transition, ClockThresholdTransition::EnteredError);
    EXPECT_EQ(record.level, spdlog::level::err);
    ASSERT_TRUE(record.notify_popup) << "error tier is the only popup source (ADR 0047)";
    EXPECT_FALSE(record.notify_message.empty());
    EXPECT_NE(record.notify_message.find("clock offset exceeds error threshold"),
              std::string::npos);
    EXPECT_NE(record.notify_message.find("offset_ms=-1000.0"), std::string::npos);
    EXPECT_NE(record.notify_message.find("error_ms=1000"), std::string::npos);
    EXPECT_NE(record.message.find("state=entered"), std::string::npos);
}

// AC1：warn 与 error 之间是 warning 档，不是 error 档（两档边界）。
TEST(ClockThresholdEvaluatorTest, OffsetBetweenTiersStaysWarning) {
    ClockThresholdEvaluator evaluator;
    const auto result =
        evaluator.evaluate(probe_input(999.9, 10.0, 1'000, kWallSec), make_config());
    ASSERT_EQ(result.records.size(), 1U);
    EXPECT_EQ(result.records[0].transition, ClockThresholdTransition::EnteredWarning);
    EXPECT_EQ(result.records[0].level, spdlog::level::warn);
    EXPECT_FALSE(result.records[0].notify_popup);
}

// AC4 / 契约 §2.8：warning → error 升级记一条 error 并弹窗（活动期不重新开始）。
TEST(ClockThresholdEvaluatorTest, EscalationFromWarningToErrorLogsErrorAndPopsUp) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(probe_input(150.0, 10.0, 0, kWallSec), config).records.size(), 1U);

    const auto escalated =
        evaluator.evaluate(probe_input(1200.0, 10.0, 30'000, kWallSec + 30), config);
    ASSERT_EQ(escalated.records.size(), 1U);
    const auto& record = escalated.records[0];
    EXPECT_EQ(record.transition, ClockThresholdTransition::Escalated);
    EXPECT_EQ(record.level, spdlog::level::err);
    EXPECT_TRUE(record.notify_popup);
    // 活动期不重新开始：起点仍是首次进入时刻，持续秒数按起点算。
    EXPECT_EQ(record.since_unix_sec, kWallSec);
    EXPECT_EQ(record.active_sec, 30);
}

// 契约 §2.8：error → warning 降级记一条 info（活动期不结束）。
TEST(ClockThresholdEvaluatorTest, DowngradeFromErrorToWarningLogsInfoAndKeepsEpisode) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(probe_input(1200.0, 10.0, 0, kWallSec), config).records.size(),
              1U);

    const auto downgraded =
        evaluator.evaluate(probe_input(300.0, 10.0, 45'000, kWallSec + 45), config);
    ASSERT_EQ(downgraded.records.size(), 1U);
    const auto& record = downgraded.records[0];
    EXPECT_EQ(record.transition, ClockThresholdTransition::Downgraded);
    EXPECT_EQ(record.level, spdlog::level::info);
    EXPECT_FALSE(record.notify_popup) << "downgrade must not interrupt the user";
    EXPECT_EQ(record.since_unix_sec, kWallSec);
    EXPECT_EQ(record.active_sec, 45);

    // 活动期仍在：重新升级到 error 是**新的一次** error 跃迁（并再次弹窗）。
    const auto reescalated =
        evaluator.evaluate(probe_input(1500.0, 10.0, 60'000, kWallSec + 60), config);
    ASSERT_EQ(reescalated.records.size(), 1U);
    EXPECT_EQ(reescalated.records[0].transition, ClockThresholdTransition::Escalated);
    EXPECT_TRUE(reescalated.records[0].notify_popup);
}

// AC3：回到阈值以内产生 info 恢复日志，带活动期持续秒数。
TEST(ClockThresholdEvaluatorTest, RecoveryBackWithinWarnThresholdLogsInfo) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(probe_input(500.0, 10.0, 0, kWallSec), config).records.size(), 1U);

    const auto recovered =
        evaluator.evaluate(probe_input(50.0, 10.0, 120'000, kWallSec + 120), config);
    ASSERT_EQ(recovered.records.size(), 1U);
    const auto& record = recovered.records[0];
    EXPECT_EQ(record.transition, ClockThresholdTransition::Recovered);
    EXPECT_EQ(record.level, spdlog::level::info);
    EXPECT_FALSE(record.notify_popup);
    EXPECT_EQ(record.active_sec, 120);
    EXPECT_NE(record.message.find("clock offset threshold recovered"), std::string::npos);
    EXPECT_NE(record.message.find("active_sec=120"), std::string::npos);

    // 恢复后再低也不重复记恢复（活动期已结束）。
    EXPECT_TRUE(evaluator.evaluate(probe_input(1.0, 10.0, 121'000, kWallSec + 121), config)
                    .records.empty());
}

// 恰好等于 warn 阈值**不**算恢复（`< offset_warn_ms` 才是恢复，契约 §2.8）。
TEST(ClockThresholdEvaluatorTest, ExactlyAtWarnThresholdIsNotRecovery) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(probe_input(500.0, 10.0, 0, kWallSec), config).records.size(), 1U);
    // 仍在 warning 档：档位没变（没有降级记录），也不产生恢复记录。
    EXPECT_TRUE(
        evaluator.evaluate(probe_input(100.0, 10.0, 1'000, kWallSec + 1), config).records.empty());
}

// AC3：持续期按间隔重记（含已持续时长，单调时钟计）。
TEST(ClockThresholdEvaluatorTest, OngoingRepeatAtIntervalIncludesElapsedSeconds) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(probe_input(300.0, 10.0, 0, kWallSec), config).records.size(), 1U);

    // 间隔内不重记。
    EXPECT_TRUE(
        evaluator.evaluate(probe_input(300.0, 10.0, 599'999, kWallSec), config).records.empty());
    // 恰好满间隔：记一条仍越限。
    const auto repeated =
        evaluator.evaluate(probe_input(300.0, 10.0, kRepeat600s, kWallSec + 600), config);
    ASSERT_EQ(repeated.records.size(), 1U);
    EXPECT_EQ(repeated.records[0].transition, ClockThresholdTransition::Ongoing);
    EXPECT_EQ(repeated.records[0].level, spdlog::level::info);
    EXPECT_FALSE(repeated.records[0].notify_popup);
    EXPECT_EQ(repeated.records[0].active_sec, 600);
    // 同一活动期内进入只记一次——重记不是"再进来一次"。
    EXPECT_EQ(repeated.records[0].since_unix_sec, kWallSec);
}

// `log_repeat_sec = 0`：一次活动期只有进入与恢复两条（契约 §2.8，与资源类同口径）。
TEST(ClockThresholdEvaluatorTest, ZeroRepeatSecDisablesOngoingRecords) {
    ClockThresholdEvaluator evaluator;
    auto config = make_config();
    config.log_repeat_sec = 0;
    ASSERT_EQ(evaluator.evaluate(probe_input(300.0, 10.0, 0, kWallSec), config).records.size(), 1U);
    for (const int64_t monotonic_ms : {kRepeat600s, kRepeat600s * 10}) {
        EXPECT_TRUE(evaluator.evaluate(probe_input(300.0, 10.0, monotonic_ms, kWallSec), config)
                        .records.empty());
    }
    const auto recovered =
        evaluator.evaluate(probe_input(1.0, 10.0, kRepeat600s * 11, kWallSec + 1), config);
    ASSERT_EQ(recovered.records.size(), 1U);
    EXPECT_EQ(recovered.records[0].transition, ClockThresholdTransition::Recovered);
}

// 重记计时用单调时钟：墙钟跳变不改变节奏（与资源类同一纪律）。
TEST(ClockThresholdEvaluatorTest, RepeatUsesMonotonicClockNotWallClock) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(probe_input(300.0, 10.0, 0, kWallSec), config).records.size(), 1U);
    // 墙钟回拨一整天、单调时钟只走 1s → 不重记。
    EXPECT_TRUE(evaluator.evaluate(probe_input(300.0, 10.0, 1'000, kWallSec - 86'400), config)
                    .records.empty());
    // 单调时钟满间隔 → 重记，持续秒数按单调时钟算。
    const auto repeated =
        evaluator.evaluate(probe_input(300.0, 10.0, kRepeat600s, kWallSec - 86'400), config);
    ASSERT_EQ(repeated.records.size(), 1U);
    EXPECT_EQ(repeated.records[0].active_sec, 600);
}

// 运行期改阈值后按新档位判定；改变不追溯（当前值按新阈值重新判档）。
TEST(ClockThresholdEvaluatorTest, RuntimeThresholdChangeAppliesToNextJudgment) {
    ClockThresholdEvaluator evaluator;
    auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(probe_input(300.0, 10.0, 0, kWallSec), config).records.size(), 1U);

    // 把 warn 调高到 500：300ms 不再越限 → 恢复。
    config.offset_warn_ms = 500;
    config.offset_error_ms = 2000;
    const auto recovered =
        evaluator.evaluate(probe_input(300.0, 10.0, 1'000, kWallSec + 1), config);
    ASSERT_EQ(recovered.records.size(), 1U);
    EXPECT_EQ(recovered.records[0].transition, ClockThresholdTransition::Recovered);
    // 记录里的阈值是**当次生效**值，不是进入时的旧值。
    EXPECT_EQ(recovered.records[0].offset_warn_ms, 500);
    EXPECT_EQ(recovered.records[0].offset_error_ms, 2000);
}

// ===== clock_bound：独立质量判据 =====

// AC2：`bound_ms` 越界只产生质量提示（warning 日志），不弹窗。
TEST(ClockThresholdEvaluatorTest, BoundExceedingAlarmLogsWarningWithoutPopup) {
    ClockThresholdEvaluator evaluator;
    const auto result = evaluator.evaluate(bound_only(200.1, 1'000, kWallSec), make_config());

    ASSERT_EQ(result.records.size(), 1U);
    const auto& record = result.records[0];
    EXPECT_EQ(record.key.kind, platform::SYSMON_THRESHOLD_CLOCK_BOUND);
    EXPECT_EQ(record.transition, ClockThresholdTransition::EnteredWarning);
    EXPECT_EQ(record.level, spdlog::level::warn);
    EXPECT_FALSE(record.notify_popup) << "measurement quality must never pop up (contract §2.8)";
    ASSERT_TRUE(record.bound_ms.has_value());
    EXPECT_DOUBLE_EQ(*record.bound_ms, 200.1);
    EXPECT_FALSE(record.offset_ms.has_value()) << "bound record must not carry an offset value";
    EXPECT_NE(record.message.find("clock bound threshold exceeded"), std::string::npos);
    EXPECT_NE(record.message.find("bound_alarm_ms=200"), std::string::npos);
}

// 契约 §2.8 边界：误差界恰好等于阈值**不**算越限（`>`），且满足恢复（`≤`）。
TEST(ClockThresholdEvaluatorTest, BoundExactlyAtAlarmIsNotBreached) {
    ClockThresholdEvaluator evaluator;
    EXPECT_TRUE(
        evaluator.evaluate(bound_only(200.0, 1'000, kWallSec), make_config()).records.empty());
}

// AC2：质量判据**不改变**偏差判定结果——两个判定对象完全独立。
TEST(ClockThresholdEvaluatorTest, BoundBreachDoesNotAffectOffsetJudgment) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    // 偏差在阈值内、误差界越界：只有质量记录，偏差判定不产生任何记录。
    const auto first = evaluator.evaluate(probe_input(50.0, 900.0, 0, kWallSec), config);
    ASSERT_EQ(first.records.size(), 1U);
    EXPECT_EQ(first.records[0].key.kind, platform::SYSMON_THRESHOLD_CLOCK_BOUND);

    // 偏差随后越过 warn：产生偏差记录，**质量活动期仍在**（不因偏差跃迁重置）。
    const auto second = evaluator.evaluate(probe_input(300.0, 900.0, 1'000, kWallSec + 1), config);
    ASSERT_EQ(second.records.size(), 1U);
    EXPECT_EQ(second.records[0].key.kind, platform::SYSMON_THRESHOLD_CLOCK_OFFSET);

    // 误差界回落：恢复记录照常产生（质量活动期没被偏差判定清掉）。
    const auto third = evaluator.evaluate(probe_input(300.0, 10.0, 2'000, kWallSec + 2), config);
    ASSERT_EQ(third.records.size(), 1U);
    EXPECT_EQ(third.records[0].key.kind, platform::SYSMON_THRESHOLD_CLOCK_BOUND);
    EXPECT_EQ(third.records[0].transition, ClockThresholdTransition::Recovered);
}

// 反向：质量越界时偏差仍按自身阈值判定——「测不准」不能掩盖「真的偏」。
TEST(ClockThresholdEvaluatorTest, OffsetJudgmentUnaffectedByBoundBreach) {
    ClockThresholdEvaluator evaluator;
    const auto result = evaluator.evaluate(probe_input(1500.0, 5000.0, 0, kWallSec), make_config());

    // 两个判定对象各一条记录：偏差 error（弹窗）+ 质量 warning（不弹窗）。
    ASSERT_EQ(result.records.size(), 2U);
    EXPECT_EQ(result.records[0].key.kind, platform::SYSMON_THRESHOLD_CLOCK_OFFSET);
    EXPECT_EQ(result.records[0].transition, ClockThresholdTransition::EnteredError);
    EXPECT_TRUE(result.records[0].notify_popup);
    EXPECT_EQ(result.records[1].key.kind, platform::SYSMON_THRESHOLD_CLOCK_BOUND);
    EXPECT_EQ(result.records[1].transition, ClockThresholdTransition::EnteredWarning);
    EXPECT_FALSE(result.records[1].notify_popup);
}

// 质量判据的持续重记与恢复（与偏差同一套口径，但独立计时）。
TEST(ClockThresholdEvaluatorTest, BoundOngoingAndRecoveryFollowSameInterval) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(bound_only(500.0, 0, kWallSec), config).records.size(), 1U);

    const auto repeated =
        evaluator.evaluate(bound_only(500.0, kRepeat600s, kWallSec + 600), config);
    ASSERT_EQ(repeated.records.size(), 1U);
    EXPECT_EQ(repeated.records[0].transition, ClockThresholdTransition::Ongoing);
    EXPECT_EQ(repeated.records[0].active_sec, 600);

    const auto recovered =
        evaluator.evaluate(bound_only(10.0, kRepeat600s + 1'000, kWallSec + 601), config);
    ASSERT_EQ(recovered.records.size(), 1U);
    EXPECT_EQ(recovered.records[0].transition, ClockThresholdTransition::Recovered);
    EXPECT_EQ(recovered.records[0].active_sec, 601);
}

// ===== 读不到 / 配置关闭 =====

// AC4：探测失败期间保留活动状态、不写恢复日志。
TEST(ClockThresholdEvaluatorTest, ProbeFailureKeepsActiveWithoutRecovery) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(probe_input(1500.0, 5000.0, 0, kWallSec), config).records.size(),
              2U);

    // 本轮全部失败（两个聚合值均为 nullopt）：保留两个活动期，不写恢复。
    EXPECT_TRUE(
        evaluator.evaluate(probe_input(std::nullopt, std::nullopt, 1'000, kWallSec + 1), config)
            .records.empty());

    // 恢复探测且仍越限：同一活动期，不重记进入——偏差与质量都不产生记录。
    EXPECT_TRUE(evaluator.evaluate(probe_input(1500.0, 5000.0, 2'000, kWallSec + 2), config)
                    .records.empty());

    // 真正回落后恢复记录的持续秒数按活动期起点算（跨越失败缺口也如实反映总时长）。
    const auto recovered =
        evaluator.evaluate(probe_input(5.0, 10.0, 90'000, kWallSec + 90), config);
    ASSERT_EQ(recovered.records.size(), 2U);
    EXPECT_EQ(recovered.records[0].transition, ClockThresholdTransition::Recovered);
    EXPECT_EQ(recovered.records[0].active_sec, 90);
    EXPECT_EQ(recovered.records[1].transition, ClockThresholdTransition::Recovered);
    EXPECT_EQ(recovered.records[1].active_sec, 90);
}

// AC5：配置关闭时静默清除、不写恢复日志。
TEST(ClockThresholdEvaluatorTest, DisablingDomainClearsSilently) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(probe_input(1500.0, 5000.0, 0, kWallSec), config).records.size(),
              2U);

    ClockThresholdInputs disabled = probe_input(std::nullopt, std::nullopt, 1'000, kWallSec + 1);
    disabled.applicable = false;
    EXPECT_TRUE(evaluator.evaluate(disabled, config).records.empty());

    // 重新启用后是**全新活动期**：再次越限要重新记进入（而且按当前档重新弹窗）。
    const auto reenabled =
        evaluator.evaluate(probe_input(1500.0, 5000.0, 2'000, kWallSec + 2), config);
    ASSERT_EQ(reenabled.records.size(), 2U);
    EXPECT_EQ(reenabled.records[0].transition, ClockThresholdTransition::EnteredError);
    EXPECT_TRUE(reenabled.records[0].notify_popup) << "re-enabling must re-raise the popup";
    EXPECT_EQ(reenabled.records[1].transition, ClockThresholdTransition::EnteredWarning);
}

// 关闭域后即使数值仍然可读也不判定（applicable 是唯一开关，不靠数值推测）。
TEST(ClockThresholdEvaluatorTest, DisabledDomainIgnoresReadableValues) {
    ClockThresholdEvaluator evaluator;
    ClockThresholdInputs disabled = probe_input(5000.0, 5000.0, 1'000, kWallSec);
    disabled.applicable = false;
    EXPECT_TRUE(evaluator.evaluate(disabled, make_config()).records.empty());
}

// ===== 跨轮次活动期的完整性 =====

// 多个完整的活动期各自有进入与恢复：状态机不跨活动期泄漏档位。
TEST(ClockThresholdEvaluatorTest, RepeatedEpisodesAreIndependent) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    int64_t monotonic_ms = 0;
    for (int episode = 0; episode < 3; ++episode) {
        const auto entered =
            evaluator.evaluate(probe_input(1500.0, 10.0, monotonic_ms, kWallSec), config);
        ASSERT_EQ(entered.records.size(), 1U) << "episode=" << episode;
        EXPECT_EQ(entered.records[0].transition, ClockThresholdTransition::EnteredError);
        EXPECT_TRUE(entered.records[0].notify_popup) << "episode=" << episode;
        monotonic_ms += 1'000;
        const auto recovered =
            evaluator.evaluate(probe_input(1.0, 10.0, monotonic_ms, kWallSec + 1), config);
        ASSERT_EQ(recovered.records.size(), 1U) << "episode=" << episode;
        EXPECT_EQ(recovered.records[0].transition, ClockThresholdTransition::Recovered);
        monotonic_ms += 1'000;
    }
}

// warning 档进入不会弹窗，即使前后活动期都曾处于 error 档。
TEST(ClockThresholdEvaluatorTest, WarningEntryAfterErrorEpisodeDoesNotPopUp) {
    ClockThresholdEvaluator evaluator;
    const auto config = make_config();
    ASSERT_EQ(evaluator.evaluate(probe_input(1500.0, 10.0, 0, kWallSec), config).records.size(),
              1U);
    ASSERT_EQ(
        evaluator.evaluate(probe_input(1.0, 10.0, 1'000, kWallSec + 1), config).records.size(), 1U);

    const auto warn = evaluator.evaluate(probe_input(200.0, 10.0, 2'000, kWallSec + 2), config);
    ASSERT_EQ(warn.records.size(), 1U);
    EXPECT_EQ(warn.records[0].transition, ClockThresholdTransition::EnteredWarning);
    EXPECT_FALSE(warn.records[0].notify_popup);
}

// ===== ClockThresholdConfig::from：两段配置汇成一份 =====

TEST(ClockThresholdConfigTest, FromMergesClockAndThresholdSections) {
    platform::SysmonClockConfig clock;
    clock.offset_warn_ms = 250;
    clock.offset_error_ms = 3000;
    clock.bound_alarm_ms = 400;
    platform::SysmonThresholdsConfig thresholds;
    thresholds.log_repeat_sec = 120;

    const auto config = ClockThresholdConfig::from(clock, thresholds);
    EXPECT_EQ(config.offset_warn_ms, 250);
    EXPECT_EQ(config.offset_error_ms, 3000);
    EXPECT_EQ(config.bound_alarm_ms, 400);
    EXPECT_EQ(config.log_repeat_sec, 120);
}

TEST(ClockThresholdConfigTest, FromDefaultsMatchContractDefaults) {
    const auto config = ClockThresholdConfig::from(platform::SysmonClockConfig{},
                                                   platform::SysmonThresholdsConfig{});
    EXPECT_EQ(config.offset_warn_ms, 100);
    EXPECT_EQ(config.offset_error_ms, 1000);
    EXPECT_EQ(config.bound_alarm_ms, 200);
    EXPECT_EQ(config.log_repeat_sec, 600);
}

}  // namespace
}  // namespace dztrader::sysmon
