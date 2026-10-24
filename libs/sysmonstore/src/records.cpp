#include <dztrader/sysmonstore/records.h>

#include <algorithm>
#include <limits>

namespace dztrader::sysmonstore {

void MetricAggregate::observe(double value) noexcept {
    if (count <= 0) {
        min = value;
        max = value;
        avg = value;
        last = value;
        count = 1;
        return;
    }
    min = std::min(min, value);
    max = std::max(max, value);
    // 增量式均值：避免累加和溢出，也避免保留额外状态（sum 不在持久化形状里）。
    const double n = static_cast<double>(count);
    avg = (avg * n + value) / (n + 1.0);
    last = value;
    ++count;
}

void MetricAggregate::merge(const MetricAggregate& other) noexcept {
    if (other.empty()) {
        return;
    }
    if (empty()) {
        *this = other;
        return;
    }
    min = std::min(min, other.min);
    max = std::max(max, other.max);
    // 按有效样本数加权：压缩两个桶时不能把 `avg` 直接平均——两侧样本数可能不等
    // （部分样本不可用的桶 count 更少），直接平均会把均值算偏。
    const double lhs_n = static_cast<double>(count);
    const double rhs_n = static_cast<double>(other.count);
    avg = (avg * lhs_n + other.avg * rhs_n) / (lhs_n + rhs_n);
    // 调用方保证 `other` 时间更晚，故 last 取 other；这里不比较时间戳是因为
    // 聚合量本身不携带时间（时间在 ScalarRecord 上）。
    last = other.last;
    count += other.count;
}

int64_t bucket_start(int64_t now_ts, int64_t interval_sec, int64_t phase_sec) noexcept {
    if (interval_sec <= 0) {
        // 防御：非法间隔不推进（配置校验已保证 [1, 86400]）；返回原时刻而不是除零。
        return now_ts;
    }
    // 相位钳制到 [0, interval)：契约允许 phase_offset_sec 取 [0, 86400]，而记录间隔可能
    // 只有 1 秒——不钳制会得到负偏移或跨多个桶的偏移，桶边界就无意义了。
    // 负的 now_ts（时钟被回拨到 1970 前）同样需要 floor 语义，故用 floor 除法而非截断。
    int64_t phase = phase_sec % interval_sec;
    if (phase < 0) {
        phase += interval_sec;
    }
    const int64_t shifted = now_ts - phase;
    int64_t quotient = shifted / interval_sec;
    if (shifted % interval_sec != 0 && (shifted < 0)) {
        --quotient;  // 负时刻按 floor 对齐，避免桶边界在 1970 附近跳一格
    }
    return quotient * interval_sec + phase;
}

int64_t minute_bucket_start(int64_t ts) noexcept {
    int64_t quotient = ts / kMinuteResolutionSec;
    if (ts % kMinuteResolutionSec != 0 && ts < 0) {
        --quotient;
    }
    return quotient * kMinuteResolutionSec;
}

std::optional<int64_t> detect_clock_jump(int64_t prev_wall_ms,
                                         int64_t prev_steady_ms,
                                         int64_t wall_ms,
                                         int64_t steady_ms,
                                         int64_t threshold_ms) noexcept {
    const int64_t wall_delta = wall_ms - prev_wall_ms;
    const int64_t steady_delta = steady_ms - prev_steady_ms;
    const int64_t drift = wall_delta - steady_delta;
    if (drift > threshold_ms || drift < -threshold_ms) {
        return drift;
    }
    return std::nullopt;
}

void HistoryBatch::clear() noexcept {
    scalars.clear();
    dimensions.clear();
    marks.clear();
}

}  // namespace dztrader::sysmonstore
