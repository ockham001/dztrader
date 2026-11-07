#include "clock_probe.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

/// 纳秒 -> 毫秒（快照 payload 的偏移/误差界单位；保留小数，契约 §2.9 未规定精度）。
double to_milliseconds(int64_t nanos) noexcept { return static_cast<double>(nanos) / 1'000'000.0; }

/// 契约 §2.9 / §7 的五种失败原因（发送方义务：只取这五个值）。
constexpr const char* kErrorTimeout = "timeout";
constexpr const char* kErrorKodRate = "kod_rate";
constexpr const char* kErrorKodDeny = "kod_deny";
constexpr const char* kErrorBadEcho = "bad_echo";
constexpr const char* kErrorUnreachable = "unreachable";

}  // namespace

ManualTriggerDecision judge_manual_trigger(int64_t now_steady_ns,
                                           std::optional<int64_t> last_trigger_steady_ns,
                                           int32_t min_interval_sec,
                                           bool probe_in_flight) noexcept {
    if (probe_in_flight) {
        // 在飞优先于限流：此时无论间隔多长都不该发起第二轮，日志要看到真实原因。
        return ManualTriggerDecision::ProbeInFlight;
    }
    if (!last_trigger_steady_ns.has_value() || min_interval_sec <= 0) {
        return ManualTriggerDecision::Allowed;
    }
    const int64_t elapsed_ns = now_steady_ns - *last_trigger_steady_ns;
    if (elapsed_ns < 0) {
        // 单调时钟不可能倒退；调用方传错顺序时按「刚触发过」处理，不放大成两轮探测。
        return ManualTriggerDecision::RateLimited;
    }
    const int64_t interval_ns = static_cast<int64_t>(min_interval_sec) * 1'000'000'000LL;
    return elapsed_ns < interval_ns ? ManualTriggerDecision::RateLimited
                                    : ManualTriggerDecision::Allowed;
}

ClockProber::ClockProber(NtpTransport& transport, MomentSource moment)
    : transport_(transport),
      moment_(std::move(moment)) {}

bool ClockProber::probe(const std::vector<std::string>& hosts,
                        int32_t timeout_ms,
                        RoundHandler done) {
    if (in_flight_) {
        return false;  // 不排队、不叠加（契约 §1.1 的权威拒绝由调用方记日志）
    }
    in_flight_ = true;
    done_ = std::move(done);
    pending_.clear();
    remaining_ = 0;
    finish_requested_ = false;

    // 去重：快照 `servers[].host` 必须帧内唯一（契约 §7），重复配置只探一次、只保留一条明细。
    std::vector<std::string> unique;
    unique.reserve(hosts.size());
    for (const auto& host : hosts) {
        if (std::find(unique.begin(), unique.end(), host) == unique.end()) {
            unique.push_back(host);
        }
    }
    details_.assign(unique.size(), platform::SysmonClockServerInfo{});
    pending_.assign(unique.size(), Pending{});

    // 第一阶段：建立明细骨架并结算不发包的台（退避/拉黑直接失败）。这一阶段必须在发出任何
    // 请求**之前**结束——传输允许同步回调，回调要能安全地按 slot 找回上下文。
    std::vector<std::size_t> slots;
    for (std::size_t i = 0; i < unique.size(); ++i) {
        details_[i].host = unique[i];
        const Moment now = moment_();
        if (const auto reason = skip_reason(details_[i].host, now.steady_ns)) {
            mark_failed(details_[i], *reason);
            continue;
        }
        ++remaining_;
        slots.push_back(i);
    }

    // 第二阶段：逐台发包。同步回调只会写 `details_`、从 `pending_` 摘一条并递减 `remaining_`。
    issuing_ = true;
    for (const std::size_t slot : slots) {
        const Moment now = moment_();
        Pending& item = pending_[slot];
        item.t1 = ntp::from_unix_nanoseconds(now.wall_ns);
        item.steady_ns = now.steady_ns;
        item.settled = false;
        transport_.async_exchange(
            details_[slot].host, ntp::build_request(item.t1), timeout_ms,
            [this, slot](NtpExchange result) { handle_exchange(slot, std::move(result)); });
    }
    issuing_ = false;
    if (finish_requested_ || remaining_ == 0) {
        finish_round();
    }
    return true;
}

void ClockProber::mark_failed(platform::SysmonClockServerInfo& detail, const char* error) {
    // ok=false 的明细不产出任何测量值（契约 §7）。
    detail.ok = false;
    detail.offset_ms = std::nullopt;
    detail.bound_ms = std::nullopt;
    detail.rtt_ms = std::nullopt;
    detail.stratum = std::nullopt;
    detail.error = std::string(error);
}

void ClockProber::handle_exchange(std::size_t slot, NtpExchange result) {
    // 迟到的回调（上一轮已收尾）：直接丢弃。单台失败不影响本轮其它台。
    if (!in_flight_ || slot >= details_.size() || remaining_ == 0) {
        return;
    }
    platform::SysmonClockServerInfo& detail = details_[slot];
    if (pending_[slot].settled) {
        return;  // 该台已经结算过（传输违约重复回调）：幂等忽略，不让计数变负。
    }
    pending_[slot].settled = true;
    const Pending pending = pending_[slot];
    const Moment now = moment_();

    switch (result.status) {
        case NtpExchange::Status::Timeout:
            mark_failed(detail, kErrorTimeout);
            break;
        case NtpExchange::Status::Unreachable:
            mark_failed(detail, kErrorUnreachable);
            break;
        case NtpExchange::Status::Received: {
            const auto response = ntp::parse_response(result.packet);
            if (!response.has_value()) {
                // 截断包与版本/模式不符都归入 `bad_echo`（契约 §2.9 的五种原因里它的释义是
                // 「origin 回显不匹配**或畸形包**」）。
                mark_failed(detail, kErrorBadEcho);
                break;
            }
            switch (ntp::classify_response(*response, pending.t1)) {
                case ntp::Verdict::KissOfDeath: {
                    const ntp::KissCode code = ntp::kiss_code(*response);
                    note_kiss(detail.host, code, now.steady_ns);
                    if (code == ntp::KissCode::Rate) {
                        mark_failed(detail, kErrorKodRate);
                    } else if (code == ntp::KissCode::Deny || code == ntp::KissCode::Restrict) {
                        mark_failed(detail, kErrorKodDeny);
                    } else {
                        // 其余 KoD（INIT / AUTH / ...）忽略处置，但包本身无可用时间戳。
                        mark_failed(detail, kErrorBadEcho);
                    }
                    break;
                }
                case ntp::Verdict::Unsynchronised:
                case ntp::Verdict::BadEcho:
                    mark_failed(detail, kErrorBadEcho);
                    break;
                case ntp::Verdict::Usable: {
                    const ntp::Timestamps stamps{pending.t1, response->receive, response->transmit,
                                                 ntp::from_unix_nanoseconds(now.wall_ns)};
                    const auto sample = ntp::derive_sample(
                        stamps, now.steady_ns - pending.steady_ns, response->root_dispersion);
                    if (!sample.has_value()) {
                        mark_failed(detail, kErrorBadEcho);
                        break;
                    }
                    detail.ok = true;
                    detail.error = std::nullopt;
                    detail.offset_ms = to_milliseconds(sample->offset_ns);
                    detail.bound_ms = to_milliseconds(sample->bound_ns);
                    detail.rtt_ms = to_milliseconds(sample->rtt_ns);
                    detail.stratum = response->stratum;
                    break;
                }
            }
            break;
        }
    }

    --remaining_;
    if (remaining_ == 0) {
        if (issuing_) {
            finish_requested_ = true;  // 还在发包循环里：出循环后再交付，避免回调重入。
        } else {
            finish_round();
        }
    }
}

void ClockProber::note_kiss(const std::string& host, ntp::KissCode code, int64_t now_steady_ns) {
    switch (code) {
        case ntp::KissCode::Rate:
            // RATE：退避一个时段（跨轮保持），时段过后照常参与探测。
            kiss_state_[host] = KissEntry{false, now_steady_ns + kKissRateBackoffNs};
            break;
        case ntp::KissCode::Deny:
        case ntp::KissCode::Restrict:
            // DENY / RSTR：拉黑到进程重启（契约 §2.7「拉黑该服务器」）。
            kiss_state_[host] = KissEntry{true, 0};
            break;
        case ntp::KissCode::None:
        case ntp::KissCode::Other:
            // 其余 KoD 忽略：不改变已有退避/拉黑状态，也不新增状态。
            break;
    }
}

std::optional<const char*> ClockProber::skip_reason(const std::string& host,
                                                    int64_t now_steady_ns) const {
    const auto it = kiss_state_.find(host);
    if (it == kiss_state_.end()) {
        return std::nullopt;
    }
    if (it->second.blocked) {
        return kErrorKodDeny;
    }
    if (now_steady_ns < it->second.until_ns) {
        return kErrorKodRate;
    }
    return std::nullopt;
}

void ClockProber::finish_round() {
    ProbeOutcome outcome;
    outcome.servers = details_;
    // 完成时刻按此刻的墙钟：`last_probe_unix_ms` 是「最近一轮探测的完成时刻」（契约 §2.9）。
    outcome.finished_unix_ms = moment_().wall_ns / 1'000'000;

    // 聚合值：最小 RTT 的那台；同 RTT 取 bound_ms 更小者（契约 §2.7）。
    const platform::SysmonClockServerInfo* best = nullptr;
    for (const auto& server : outcome.servers) {
        if (!server.ok || !server.rtt_ms.has_value() || !server.bound_ms.has_value()) {
            continue;
        }
        if (best == nullptr || *server.rtt_ms < *best->rtt_ms ||
            (*server.rtt_ms == *best->rtt_ms && *server.bound_ms < *best->bound_ms)) {
            best = &server;
        }
    }
    if (best != nullptr) {
        // 偏差 / 误差界 / stratum 同生共死：有聚合值就三者齐备（契约 §2.9、§7）。
        outcome.offset_ms = best->offset_ms;
        outcome.bound_ms = best->bound_ms;
        outcome.source = best->host;
        outcome.source_stratum = best->stratum;
    }
    // 全部失败：三个聚合字段保持不可用——不得用 0 或上一轮结果冒充（契约 §2.9）。

    in_flight_ = false;
    pending_.clear();
    details_.clear();
    RoundHandler handler = std::move(done_);
    done_ = nullptr;
    if (handler) {
        handler(std::move(outcome));
    }
}

}  // namespace dztrader::sysmon
