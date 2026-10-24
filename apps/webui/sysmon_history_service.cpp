#include "sysmon_history_service.h"

#include <dztrader/sysmonstore/history_query.h>
#include <dztrader/sysmonstore/history_store.h>
#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <exception>
#include <utility>
#include <vector>

namespace dztrader::webui {

namespace {

using dztrader::sysmonstore::HistoryAvailability;
using dztrader::sysmonstore::HistoryReader;
using dztrader::sysmonstore::MetricRef;
using dztrader::sysmonstore::QueryLevel;

constexpr int64_t kDefaultRangeSec = 6 * 3600;  ///< `from` 缺省：最近 6 小时

/// 解析整数参数：缺失用默认值；非数字/溢出/越界返回 `std::nullopt`（调用方回 400）。
/// `std::stoll` 会抛 invalid_argument/out_of_range，必须隔离（宁肯 400 不崩溃）。
std::optional<int64_t> parse_int_param(const std::function<std::string(std::string_view)>& param,
                                       std::string_view key) {
    const std::string text = param(key);
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        size_t consumed = 0;
        const long long value = std::stoll(text, &consumed);
        if (consumed != text.size()) {
            return std::nullopt;  // "12abc" 必须拒绝，不做部分解析
        }
        return static_cast<int64_t>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// 请求了点参数但值非法（区别于「没给参数」）。
bool has_param(const std::function<std::string(std::string_view)>& param, std::string_view key) {
    return !param(key).empty();
}

/// 当前墙钟秒（Unix 秒）。
int64_t current_unix_sec() { return static_cast<int64_t>(std::time(nullptr)); }

/// 降级响应（契约 sysmon §6.1.3）：只有 available 与 reason，不带任何数据字段。
nlohmann::json unavailable_body(HistoryAvailability availability) {
    return {
        {"available", false},
        {"reason", std::string(dztrader::sysmonstore::history_availability_reason(availability))}};
}

/// 一个聚合量的 JSON 表示：空聚合输出四个 null + n=null（不得用 0 冒充，契约 §6.1）。
nlohmann::json aggregate_to_json(const dztrader::sysmonstore::MetricAggregate& aggregate) {
    if (aggregate.empty()) {
        return {{"ts", nullptr},  {"min", nullptr},  {"max", nullptr},
                {"avg", nullptr}, {"last", nullptr}, {"n", nullptr}};
    }
    return {{"min", aggregate.min},
            {"max", aggregate.max},
            {"avg", aggregate.avg},
            {"last", aggregate.last},
            {"n", aggregate.count}};
}

/// 构造一个数据点；`ts` 为该桶起点。
nlohmann::json point_to_json(int64_t ts, const dztrader::sysmonstore::MetricAggregate& aggregate) {
    nlohmann::json point = aggregate_to_json(aggregate);
    point["ts"] = ts;
    return point;
}

}  // namespace

SysmonHistoryService::SysmonHistoryService(std::filesystem::path db_path,
                                           int busy_timeout_ms,
                                           int64_t now_ts)
    : db_path_(std::move(db_path)),
      busy_timeout_ms_(busy_timeout_ms),
      now_ts_(now_ts > 0 ? now_ts : current_unix_sec()) {}

SysmonHistoryParseResult SysmonHistoryService::parse(
    const std::function<std::string(std::string_view)>& param) const {
    SysmonHistoryParseResult result;

    // ---- metric（契约 §6.1：必须是已注册指标键）----
    std::string metric = param("metric");
    if (metric.empty()) {
        metric = "cpu.usage_ratio";  // 契约默认值
    }
    const MetricRef ref = dztrader::sysmonstore::resolve_metric(metric);
    if (!ref.valid()) {
        result.error = "unknown metric";
        return result;
    }

    // ---- dimension（维度指标必填、标量指标不得给）----
    std::string dimension = param("dimension");
    if (ref.is_dimension) {
        if (dimension.empty()) {
            result.error = "dimension required";
            return result;
        }
    } else if (!dimension.empty()) {
        result.error = "dimension not allowed";
        return result;
    }

    // ---- level ----
    std::string level_text = param("level");
    if (level_text.empty()) {
        level_text = "auto";
    }
    if (!dztrader::sysmonstore::parse_query_level(level_text).has_value()) {
        result.error = "invalid level";
        return result;
    }

    // ---- max_points（缺省用默认值；给了就必须在区间内，不做静默钳制）----
    int64_t max_points = dztrader::sysmonstore::kDefaultMaxPoints;
    if (has_param(param, "max_points")) {
        const auto parsed = parse_int_param(param, "max_points");
        if (!parsed.has_value() || *parsed < dztrader::sysmonstore::kMinMaxPoints ||
            *parsed > dztrader::sysmonstore::kMaxMaxPoints) {
            result.error = "invalid max_points";
            return result;
        }
        max_points = *parsed;
    }

    // ---- to / from（to 缺省为当前时刻；from 缺省为 to - 6h）----
    int64_t to_ts = now_ts_;
    if (has_param(param, "to")) {
        const auto parsed = parse_int_param(param, "to");
        if (!parsed.has_value()) {
            result.error = "invalid to";
            return result;
        }
        to_ts = *parsed;
    }
    int64_t from_ts = to_ts - kDefaultRangeSec;
    if (has_param(param, "from")) {
        const auto parsed = parse_int_param(param, "from");
        if (!parsed.has_value()) {
            result.error = "invalid from";
            return result;
        }
        from_ts = *parsed;
    }
    // 契约：0 < from < to。`from <= 0` 无意义（Unix 秒纪元前的历史不存在），
    // `from >= to` 是空/倒置范围 —— 一律 400，不静默改成默认范围。
    if (from_ts <= 0 || to_ts <= from_ts) {
        result.error = "invalid time range";
        return result;
    }

    SysmonHistoryQuery query;
    query.metric = std::move(metric);
    query.dimension = std::move(dimension);
    query.from_ts = from_ts;
    query.to_ts = to_ts;
    query.level = std::move(level_text);
    query.max_points = max_points;
    result.query = std::move(query);
    return result;
}

nlohmann::json SysmonHistoryService::query(const SysmonHistoryQuery& query) const {
    // ---- 只读打开：库缺失 / 版本不符 / 损坏一律降级，不抛 ----
    auto opened = HistoryReader::open(db_path_, busy_timeout_ms_);
    if (opened.first != HistoryAvailability::Ok || !opened.second) {
        return unavailable_body(opened.first);
    }
    const auto& reader = *opened.second;

    const MetricRef ref = dztrader::sysmonstore::resolve_metric(query.metric);
    if (!ref.valid()) {
        // 调用方已校验过，走到这里说明参数与解析器漂移：按「历史不可用」降级而不是 500。
        SPDLOG_WARN("sysmon history metric unresolved | metric={}", query.metric);
        return unavailable_body(HistoryAvailability::OpenFailed);
    }

    // ---- 层级：auto 按范围宽度选择（§6.1.1）----
    const QueryLevel requested =
        dztrader::sysmonstore::parse_query_level(query.level).value_or(QueryLevel::Auto);
    const QueryLevel level =
        requested == QueryLevel::Auto
            ? dztrader::sysmonstore::select_auto_level(query.from_ts, query.to_ts)
            : requested;
    const auto storage_level = dztrader::sysmonstore::query_level_to_storage(level);
    if (!storage_level.has_value()) {
        return unavailable_body(HistoryAvailability::OpenFailed);
    }

    // ---- 点数上限：多取一行精确判定是否截断（§6.1.2）----
    const int64_t fetch_rows = query.max_points + 1;

    nlohmann::json series_points = nlohmann::json::array();
    std::vector<int64_t> bucket_ts;
    int64_t interval_sec = 0;
    bool truncated = false;
    int64_t effective_from = query.from_ts;

    try {
        if (ref.is_dimension) {
            auto rows = reader.query_dimensions_desc(ref.domain, query.dimension, ref.metric,
                                                     *storage_level, query.from_ts, query.to_ts,
                                                     fetch_rows);
            const auto trim =
                dztrader::sysmonstore::trim_to_point_limit(rows.size(), query.max_points);
            truncated = trim.truncated;
            // 查询是降序的：保留前 `keep` 行（最新的 keep 行），再反转成升序输出。
            rows.resize(trim.keep);
            std::reverse(rows.begin(), rows.end());
            bucket_ts.reserve(rows.size());
            for (const auto& row : rows) {
                bucket_ts.push_back(row.bucket_ts);
                series_points.push_back(point_to_json(row.bucket_ts, row.aggregate));
            }
        } else {
            auto rows =
                reader.query_scalars_desc(*storage_level, query.from_ts, query.to_ts, fetch_rows);
            const auto trim =
                dztrader::sysmonstore::trim_to_point_limit(rows.size(), query.max_points);
            truncated = trim.truncated;
            rows.resize(trim.keep);
            std::reverse(rows.begin(), rows.end());
            bucket_ts.reserve(rows.size());
            for (const auto& row : rows) {
                const size_t index =
                    static_cast<size_t>(ref.scalar - dztrader::sysmonstore::kScalarMetrics.data());
                bucket_ts.push_back(row.bucket_ts);
                series_points.push_back(point_to_json(row.bucket_ts, row.metrics[index]));
                if (interval_sec == 0) {
                    interval_sec = row.interval_sec;
                }
            }
        }
    } catch (const std::exception& e) {
        // 查询期间的 SQL 失败同样降级（§6.1.3），不把半截数据当结果返回。
        SPDLOG_WARN("sysmon history query failed | metric={} error=\"{}\"", query.metric, e.what());
        return unavailable_body(HistoryAvailability::OpenFailed);
    }

    // 生效起点：被截断时为首个返回点的桶时刻（契约 §6.1.2 的精确口径）
    if (truncated && !bucket_ts.empty()) {
        effective_from = bucket_ts.front();
    }
    if (level == QueryLevel::Minute) {
        // 分钟聚合层级桶宽恒为 60（契约 §6.1 的 interval_sec 口径）；
        // 无桶时保持 0，序列化时输出 null。
        interval_sec = dztrader::sysmonstore::kMinuteResolutionSec;
    }

    // ---- 标记与缺口（§6.1.4）----
    nlohmann::json marks_json = nlohmann::json::array();
    std::vector<dztrader::sysmonstore::HistoryMark> marks;
    try {
        marks = reader.query_marks(effective_from, query.to_ts, query.max_points);
    } catch (const std::exception& e) {
        SPDLOG_WARN("sysmon history marks query failed | error=\"{}\"", e.what());
        return unavailable_body(HistoryAvailability::OpenFailed);
    }
    std::vector<dztrader::sysmonstore::Gap> gaps;
    if (interval_sec > 0) {
        gaps = dztrader::sysmonstore::derive_gaps(bucket_ts, interval_sec, marks);
    }
    for (const auto& mark : marks) {
        marks_json.push_back({{"ts", mark.ts}, {"kind", mark.kind}, {"detail", mark.detail}});
    }
    nlohmann::json gaps_json = nlohmann::json::array();
    for (const auto& gap : gaps) {
        gaps_json.push_back({{"from", gap.from_ts}, {"to", gap.to_ts}, {"kind", gap.kind}});
    }

    nlohmann::json series = nlohmann::json::array();
    series.push_back({{"dimension", query.dimension}, {"points", std::move(series_points)}});

    return {{"available", true},
            {"level", std::string(dztrader::sysmonstore::query_level_name(level))},
            {"metric", query.metric},
            {"from", effective_from},
            {"to", query.to_ts},
            {"interval_sec", interval_sec > 0 ? nlohmann::json(interval_sec) : nlohmann::json()},
            {"truncated", truncated},
            {"series", std::move(series)},
            {"marks", std::move(marks_json)},
            {"gaps", std::move(gaps_json)}};
}

}  // namespace dztrader::webui
