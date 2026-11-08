#include "probe_schedule.h"

#include <dztrader/platform/sysmon.h>

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

/// 把时间戳换算成本地日历时间；失败返回 false（调用方按「本元素非法」忽略）。
bool local_time(std::time_t timestamp, std::tm& out) noexcept {
#ifdef _WIN32
    return localtime_s(&out, &timestamp) == 0;
#else
    return localtime_r(&timestamp, &out) != nullptr;
#endif
}

/// 取一个 `std::tm` 的本地时区日历时刻（`tm_isdst = -1` 交给运行时判定夏令时）。
///
/// `mktime` 会就地归一化越界字段（如 `tm_mday += 1` 跨月），因此「次日同一 HH:MM」与
/// 夏令时切换都由它处理，不需要本模块自己维护日历规则。
std::optional<std::chrono::system_clock::time_point> resolve_local(std::tm fields) noexcept {
    fields.tm_isdst = -1;
    const std::time_t timestamp = std::mktime(&fields);
    if (timestamp == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(timestamp);
}

}  // namespace

std::optional<std::chrono::system_clock::time_point> next_probe_wall_time(
    std::chrono::system_clock::time_point now_wall,
    const std::vector<std::string>& probe_times) noexcept {
    const std::time_t now_seconds = std::chrono::system_clock::to_time_t(now_wall);
    std::tm now_local{};
    if (!local_time(now_seconds, now_local)) {
        return std::nullopt;
    }

    std::optional<std::chrono::system_clock::time_point> earliest;
    for (const auto& text : probe_times) {
        const auto parsed = platform::parse_sysmon_probe_time(text);
        if (!parsed.has_value()) {
            continue;
        }
        std::tm target = now_local;
        target.tm_hour = parsed->hour;
        target.tm_min = parsed->minute;
        target.tm_sec = 0;
        auto candidate = resolve_local(target);
        if (!candidate.has_value()) {
            continue;
        }
        // 严格晚于 now：已到达/已过去的时刻算「明天」——启动首探覆盖当下这一刻，
        // 定时探测不得在同一分钟内再补一次（契约：错过则跳过、不补采）。
        if (*candidate <= now_wall) {
            target.tm_mday += 1;  // mktime 归一化跨月/跨年与夏令时
            candidate = resolve_local(target);
            if (!candidate.has_value()) {
                continue;
            }
        }
        if (!earliest.has_value() || *candidate < *earliest) {
            earliest = *candidate;
        }
    }
    return earliest;
}

std::optional<std::chrono::steady_clock::duration> next_probe_delay(
    std::chrono::system_clock::time_point now_wall,
    const std::vector<std::string>& probe_times) noexcept {
    const auto next = next_probe_wall_time(now_wall, probe_times);
    if (!next.has_value()) {
        return std::nullopt;
    }
    const auto delay = *next - now_wall;
    // 防御：换算结果不倒退（正常路径由「严格晚于」保证为正）。
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::max(delay, std::chrono::system_clock::duration::zero()));
}

ProbeSchedule::ProbeSchedule(boost::asio::io_context& io)
    : timer_(io) {}

void ProbeSchedule::reschedule(std::chrono::system_clock::time_point now_wall,
                               const std::vector<std::string>& probe_times,
                               Handler on_due) {
    stop();  // 取消旧等待并清空旧回调
    const auto delay = next_probe_delay(now_wall, probe_times);
    if (!delay.has_value()) {
        return;  // 没有可排的时刻：保持停止（armed() 恒 false）
    }
    on_due_ = std::move(on_due);
    arm(*delay);
}

void ProbeSchedule::arm(std::chrono::steady_clock::duration delay) {
    armed_ = true;
    ++generation_;
    timer_.expires_after(delay);
    timer_.async_wait([this, token = generation_](const boost::system::error_code& ec) {
        if (ec || token != generation_) {
            // 被 stop()/reschedule() 取消，或旧等待的 handler 迟到：新等待已由调用方排好，
            // 这里不得改 armed_、也不得触发新排期的回调。
            return;
        }
        armed_ = false;
        Handler handler = std::move(on_due_);
        on_due_ = nullptr;
        if (handler) {
            handler();
        }
    });
}

void ProbeSchedule::stop() {
    timer_.cancel();
    ++generation_;
    armed_ = false;
    on_due_ = nullptr;
}

}  // namespace dztrader::sysmon
