#include "clock_thresholds.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dztrader::sysmon {
namespace {

/// 毫秒文案：1 位小数、带符号（偏差方向对使用者是信息，`+` 必须显式出现）。
std::string format_signed_ms(double millis) { return std::format("{:+.1f}", millis); }

/// 阈值文案：整数毫秒，不带符号。
std::string format_limit_ms(int32_t millis) { return std::to_string(millis); }

/// 当前值是否可判定：`nullopt` / 非有限值按「本轮不可判定」处理（保留活动状态）。
bool is_valid_ms(const std::optional<double>& millis) {
    return millis.has_value() && std::isfinite(*millis);
}

/// 跃迁在日志里的 `state=` 取值（契约 §2.8 的 `state` 词汇；英文、小写、无空格）。
constexpr std::string_view to_state_text(ClockThresholdTransition transition) noexcept {
    switch (transition) {
        case ClockThresholdTransition::EnteredWarning:
        case ClockThresholdTransition::EnteredError:
            return "entered";
        case ClockThresholdTransition::Escalated:
            return "escalated";
        case ClockThresholdTransition::Downgraded:
            return "downgraded";
        case ClockThresholdTransition::Ongoing:
            return "ongoing";
        case ClockThresholdTransition::Recovered:
            return "recovered";
    }
    return "unknown";  // 穷举所有跃迁；不可达
}

}  // namespace

ClockThresholdConfig ClockThresholdConfig::from(
    const platform::SysmonClockConfig& clock,
    const platform::SysmonThresholdsConfig& thresholds) noexcept {
    ClockThresholdConfig config;
    config.offset_warn_ms = clock.offset_warn_ms;
    config.offset_error_ms = clock.offset_error_ms;
    config.bound_alarm_ms = clock.bound_alarm_ms;
    config.log_repeat_sec = thresholds.log_repeat_sec;
    return config;
}

ClockOffsetTier ClockThresholdEvaluator::judge_tier(double offset_ms,
                                                    int32_t warn_ms,
                                                    int32_t error_ms) noexcept {
    // 绝对值判档：本地快/慢都算偏离，方向只影响「该往哪边修」而不影响「是否超限」。
    // 契约 §2.8「相等按表处理」：恰好等于阈值算进入该档（`≥`）。
    const double magnitude = std::fabs(offset_ms);
    if (magnitude >= static_cast<double>(error_ms)) {
        return ClockOffsetTier::Error;
    }
    if (magnitude >= static_cast<double>(warn_ms)) {
        return ClockOffsetTier::Warning;
    }
    return ClockOffsetTier::None;
}

void ClockThresholdEvaluator::evaluate_offset(const ClockThresholdInputs& inputs,
                                              const ClockThresholdConfig& config,
                                              int64_t repeat_ms,
                                              Result& out) {
    if (!is_valid_ms(inputs.offset_ms)) {
        return;  // 本轮未测到：保留活动状态，不伪造恢复（契约 §2.8）
    }
    const double offset_ms = *inputs.offset_ms;
    const ClockOffsetTier tier =
        judge_tier(offset_ms, config.offset_warn_ms, config.offset_error_ms);
    const auto push = [&](ClockThresholdTransition transition, spdlog::level::level_enum level,
                          bool notify_popup, int64_t active_sec) {
        out.records.push_back(
            make_offset_record(config, transition, level, notify_popup, active_sec, offset_ms));
    };

    if (tier == ClockOffsetTier::None) {
        // 回到阈值以内：只有真正处于活动期时才记恢复（从未越限过的不建状态）。
        if (offset_.period.active) {
            push(ClockThresholdTransition::Recovered, spdlog::level::info, /*notify_popup=*/false,
                 offset_.period.elapsed_sec(inputs.now_monotonic_ms));
            offset_ = OffsetState{};
        }
        return;
    }

    const bool error = tier == ClockOffsetTier::Error;
    if (!offset_.period.active) {
        // 进入新的活动期：按当前档记一条。error 档是唯一弹窗来源（ADR 0047）。
        offset_.period.start(inputs.now_unix_sec, inputs.now_monotonic_ms, repeat_ms);
        offset_.tier = tier;
        push(error ? ClockThresholdTransition::EnteredError
                   : ClockThresholdTransition::EnteredWarning,
             error ? spdlog::level::err : spdlog::level::warn, /*notify_popup=*/error, 0);
        return;
    }

    if (tier != offset_.tier) {
        // 活动期内的档位跃迁：升级是 error 日志 + 弹窗（新出现的故障程度），
        // 降级是 info 日志且**不结束活动期**（偏差仍越 warn 线，问题没有消失）。
        const bool escalating = error;
        offset_.tier = tier;
        push(
            escalating ? ClockThresholdTransition::Escalated : ClockThresholdTransition::Downgraded,
            escalating ? spdlog::level::err : spdlog::level::info,
            /*notify_popup=*/escalating, offset_.period.elapsed_sec(inputs.now_monotonic_ms));
        return;
    }

    // 档位不变、仍在活动期内：阈值持续满足不重记，但每满间隔记一条「仍越限」。
    if (offset_.period.repeat_due(inputs.now_monotonic_ms, repeat_ms)) {
        push(ClockThresholdTransition::Ongoing, spdlog::level::info, /*notify_popup=*/false,
             offset_.period.elapsed_sec(inputs.now_monotonic_ms));
    }
}

void ClockThresholdEvaluator::evaluate_bound(const ClockThresholdInputs& inputs,
                                             const ClockThresholdConfig& config,
                                             int64_t repeat_ms,
                                             Result& out) {
    if (!is_valid_ms(inputs.bound_ms)) {
        return;  // 本轮测不到：保留活动状态（「测不准」本身也要靠活动期表达，不伪造恢复）
    }
    const double bound_ms = *inputs.bound_ms;
    // 契约 §2.8：误差界"恰好等于阈值"不算越限（`>`），且满足恢复（`≤`）。
    const bool breached = bound_ms > static_cast<double>(config.bound_alarm_ms);

    ClockThresholdLogRecord record;
    record.key = ThresholdKey{platform::SYSMON_THRESHOLD_CLOCK_BOUND, ""};
    record.bound_ms = bound_ms;
    record.offset_warn_ms = config.offset_warn_ms;
    record.offset_error_ms = config.offset_error_ms;
    record.bound_alarm_ms = config.bound_alarm_ms;
    record.since_unix_sec = bound_.since_unix_sec;
    // 「测量质量」提示永远不弹窗，也不影响偏差判定（契约 §2.8 / ADR 0047）。
    record.notify_popup = false;
    const std::string fields =
        std::format("bound_ms={:.1f} bound_alarm_ms={}", bound_ms, config.bound_alarm_ms);

    if (breached) {
        if (!bound_.active) {
            bound_.start(inputs.now_unix_sec, inputs.now_monotonic_ms, repeat_ms);
            record.since_unix_sec = bound_.since_unix_sec;
            record.transition = ClockThresholdTransition::EnteredWarning;
            record.level = spdlog::level::warn;
            record.active_sec = 0;
            record.message =
                std::format("clock bound threshold exceeded | subject={} state=entered {} since={}",
                            record.key.kind, fields, record.since_unix_sec);
            out.records.push_back(std::move(record));
            return;
        }
        if (bound_.repeat_due(inputs.now_monotonic_ms, repeat_ms)) {
            record.transition = ClockThresholdTransition::Ongoing;
            record.level = spdlog::level::info;
            record.active_sec = bound_.elapsed_sec(inputs.now_monotonic_ms);
            record.message = std::format(
                "clock bound threshold still exceeded | subject={} state=ongoing {} "
                "active_sec={}",
                record.key.kind, fields, record.active_sec);
            out.records.push_back(std::move(record));
        }
        return;
    }

    if (!bound_.active) {
        return;  // 从未越限过的对象不建状态、不记恢复
    }
    record.transition = ClockThresholdTransition::Recovered;
    record.level = spdlog::level::info;
    record.active_sec = bound_.elapsed_sec(inputs.now_monotonic_ms);
    record.message =
        std::format("clock bound threshold recovered | subject={} state=recovered {} active_sec={}",
                    record.key.kind, fields, record.active_sec);
    out.records.push_back(std::move(record));
    bound_ = ActivityPeriod{};
}

ClockThresholdEvaluator::Result ClockThresholdEvaluator::evaluate(
    const ClockThresholdInputs& inputs, const ClockThresholdConfig& config) {
    Result result;
    if (!inputs.applicable) {
        // 配置关闭 → 静默清除（清除原因不是"指标回到阈值以下"，不写恢复日志）。
        clear();
        return result;
    }
    // 重记间隔（秒 → 毫秒）；配置校验保证 >= 0，不在此重复钳制。
    const int64_t repeat_ms = static_cast<int64_t>(config.log_repeat_sec) * 1000;
    // 两个判定对象相互独立：各推进各的，互不影响对方的活动期。
    evaluate_offset(inputs, config, repeat_ms, result);
    evaluate_bound(inputs, config, repeat_ms, result);
    return result;
}

ClockThresholdLogRecord ClockThresholdEvaluator::make_offset_record(
    const ClockThresholdConfig& config,
    ClockThresholdTransition transition,
    spdlog::level::level_enum level,
    bool notify_popup,
    int64_t active_sec,
    double offset_ms) {
    ClockThresholdLogRecord record;
    record.key = ThresholdKey{platform::SYSMON_THRESHOLD_CLOCK_OFFSET, ""};
    record.transition = transition;
    record.level = level;
    record.notify_popup = notify_popup;
    record.offset_ms = offset_ms;
    // 两个偏差阈值都进每条记录（契约 §2.8：`clock_offset` 的「阈值」含两档）。
    record.offset_warn_ms = config.offset_warn_ms;
    record.offset_error_ms = config.offset_error_ms;
    record.active_sec = active_sec;
    record.since_unix_sec = offset_.period.since_unix_sec;
    const std::string fields = std::format(
        "offset_ms={} warn_ms={} error_ms={}", format_signed_ms(offset_ms),
        format_limit_ms(config.offset_warn_ms), format_limit_ms(config.offset_error_ms));
    const std::string_view state_text = to_state_text(transition);
    if (transition == ClockThresholdTransition::EnteredWarning ||
        transition == ClockThresholdTransition::EnteredError) {
        // 进入记录带上墙钟起点：UI 不呈现"始于何时"，这条日志是唯一来源。
        record.message = std::format(
            "clock offset threshold exceeded | subject={} state={} {} "
            "since={}",
            record.key.kind, state_text, fields, record.since_unix_sec);
    } else {
        const char* verb =
            transition == ClockThresholdTransition::Recovered ? "recovered" : "still exceeded";
        record.message = std::format(
            "clock offset threshold {} | subject={} state={} {} "
            "active_sec={}",
            verb, record.key.kind, state_text, fields, active_sec);
    }
    if (notify_popup) {
        // 弹窗正文与日志同源生成：两处各拼一套文本口径必然漂移。
        record.notify_message =
            std::format("clock offset exceeds error threshold | offset_ms={} error_ms={}",
                        format_signed_ms(offset_ms), format_limit_ms(config.offset_error_ms));
    }
    return record;
}

}  // namespace dztrader::sysmon
