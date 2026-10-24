#include <dztrader/sysmonstore/history_query.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace dztrader::sysmonstore {

namespace {

/// 按第一个 `.` 切分域标识与指标名。域标识都不含点，因此切分点唯一。
std::pair<std::string_view, std::string_view> split_domain_metric(std::string_view key) noexcept {
    const auto dot = key.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= key.size()) {
        return {std::string_view{}, std::string_view{}};
    }
    return {key.substr(0, dot), key.substr(dot + 1)};
}

/// 缺口原因优先级：`restart` 强于 `sample_gap`（进程重启是更强的解释）。
int mark_priority(std::string_view kind) noexcept {
    if (kind == kMarkRestart) {
        return 2;
    }
    if (kind == kMarkSampleGap) {
        return 1;
    }
    return 0;  // clock_jump 与未知 kind 都不解释缺口
}

}  // namespace

MetricRef resolve_metric(std::string_view key) noexcept {
    MetricRef ref;
    if (key.empty()) {
        return ref;
    }
    // 先匹配宽表逻辑键：宽表键本身含点（`memory.used_bytes`），若先按点切分会把
    // `memory` 当成域而匹配不到；宽表键集合小且为编译期常量，线性查找足够。
    if (const auto* scalar = find_scalar_metric(key); scalar != nullptr) {
        ref.scalar = scalar;
        return ref;
    }
    const auto [domain, metric] = split_domain_metric(key);
    if (domain.empty() || metric.empty()) {
        return ref;
    }
    if (!dimension_domain_has_metric(domain, metric)) {
        return ref;
    }
    ref.domain = domain;
    ref.metric = metric;
    ref.is_dimension = true;
    return ref;
}

std::optional<QueryLevel> parse_query_level(std::string_view text) noexcept {
    if (text == "auto") {
        return QueryLevel::Auto;
    }
    if (text == "raw") {
        return QueryLevel::Raw;
    }
    if (text == "minute") {
        return QueryLevel::Minute;
    }
    return std::nullopt;
}

std::string_view query_level_name(QueryLevel level) noexcept {
    switch (level) {
        case QueryLevel::Auto:
            return "auto";
        case QueryLevel::Raw:
            return "raw";
        case QueryLevel::Minute:
            return "minute";
    }
    return "raw";
}

std::optional<int32_t> query_level_to_storage(QueryLevel level) noexcept {
    switch (level) {
        case QueryLevel::Raw:
            return kLevelRaw;
        case QueryLevel::Minute:
            return kLevelMinute;
        case QueryLevel::Auto:
            return std::nullopt;
    }
    return std::nullopt;
}

QueryLevel select_auto_level(int64_t from_ts, int64_t to_ts) noexcept {
    // 范围宽度 <= 阈值时原始记录点数有界（默认记录间隔 60s ⇒ 1 小时 60 点），用最细粒度；
    // 更宽的范围用原始记录会撑爆点数上限，而一分钟聚合正是为此存在。
    const int64_t width = to_ts > from_ts ? to_ts - from_ts : 0;
    return width <= kAutoRawMaxRangeSec ? QueryLevel::Raw : QueryLevel::Minute;
}

PointLimitTrim trim_to_point_limit(size_t fetched_rows, int64_t max_points) noexcept {
    PointLimitTrim trim;
    if (max_points <= 0) {
        trim.keep = fetched_rows;
        return trim;
    }
    const auto limit = static_cast<size_t>(max_points);
    if (fetched_rows <= limit) {
        trim.keep = fetched_rows;
        return trim;
    }
    // 多取的那一行是「还有更旧数据」的证据：确实丢了行才算 truncated。
    trim.keep = limit;
    trim.truncated = true;
    return trim;
}

std::vector<Gap> derive_gaps(const std::vector<int64_t>& bucket_ts,
                             int64_t interval_sec,
                             const std::vector<HistoryMark>& marks) {
    std::vector<Gap> gaps;
    if (bucket_ts.size() < 2 || interval_sec <= 0) {
        return gaps;
    }
    for (size_t i = 1; i < bucket_ts.size(); ++i) {
        const int64_t prev_end = bucket_ts[i - 1] + interval_sec;
        const int64_t cur_start = bucket_ts[i];
        if (cur_start <= prev_end) {
            continue;  // 相邻或重叠：连续
        }
        // 缺口 [prev_end, cur_start)：找落在其中的标记解释原因。
        int best = 0;
        std::string_view kind;
        for (const auto& mark : marks) {
            if (mark.ts < prev_end || mark.ts >= cur_start) {
                continue;
            }
            const int priority = mark_priority(mark.kind);
            if (priority > best) {
                best = priority;
                kind = mark.kind;
            }
        }
        Gap gap;
        gap.from_ts = prev_end;
        gap.to_ts = cur_start;
        gap.kind = best == 0 ? std::string(kGapUnmarked) : std::string(kind);
        gaps.push_back(std::move(gap));
    }
    return gaps;
}

}  // namespace dztrader::sysmonstore
