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

/// 单次查询返回的标记上限。标记不是记录点：每次重启/跳变/缺口才写一条，量级极小；
/// 给它一个远高于 `max_points` 合法上界（20000）的独立上限，避免缺口原因被点数上限连带截断。
/// 取值必须是 `kMaxMaxPoints` 的若干倍——若低于该上界，用户把 `max_points` 调到上限附近时
/// 标记仍会被静默截断（此时缺口又被错报成 unmarked，与独立上限的初衷相反）。
constexpr int64_t kMaxMarksPerQuery = 100000;

// 编译期守卫：标记上限必须显著高于 `max_points` 的合法上界（契约 §6.1.4 的要求），
// 否则用户在合法范围内把 max_points 调大就能触发标记静默截断（缺口错报 unmarked）。
static_assert(kMaxMarksPerQuery >= 5 * dztrader::sysmonstore::kMaxMaxPoints,
              "marks cap must stay well above the max_points upper bound");

/**
 * @brief 从相邻桶时刻反推名义桶宽（仅供**没有** `interval_sec` 列的长表使用）。
 *
 * 取相邻间隔的最小正值：桶网格是等距的，缺口只会让某个间隔变大，因此最小间隔就是
 * 名义桶宽。单点或全部相同时刻（脏数据）无从反推，返回 0（调用方序列化成 null）。
 */
int64_t derive_interval_from_buckets(const std::vector<int64_t>& bucket_ts) {
    if (bucket_ts.size() < 2) {
        return 0;
    }
    int64_t smallest = 0;
    for (size_t i = 1; i < bucket_ts.size(); ++i) {
        const int64_t delta = bucket_ts[i] - bucket_ts[i - 1];
        if (delta > 0 && (smallest == 0 || delta < smallest)) {
            smallest = delta;
        }
    }
    return smallest;
}

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

/// 降级响应（《帧契约：系统监控》§6.1.3）：只有 available 与 reason，不带任何数据字段。
nlohmann::json unavailable_body(HistoryAvailability availability) {
    return {
        {"available", false},
        {"reason", std::string(dztrader::sysmonstore::history_availability_reason(availability))}};
}

/// 一个聚合量的 JSON 表示：空聚合输出四个 null + n=null（不得用 0
/// 冒充，《帧契约：系统监控》§6.1）。
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

    // ---- metric（《帧契约：系统监控》§6.1：必须是已注册指标键）----
    std::string metric = param("metric");
    if (metric.empty()) {
        metric = "cpu.usage_ratio";  // 契约默认值
    }
    const MetricRef ref = dztrader::sysmonstore::resolve_metric(metric);
    if (!ref.valid()) {
        return result;
    }

    // ---- dimension（维度指标必填、标量指标不得给）----
    std::string dimension = param("dimension");
    if (ref.is_dimension) {
        if (dimension.empty()) {
            return result;
        }
    } else if (!dimension.empty()) {
        return result;
    }

    // ---- level ----
    std::string level_text = param("level");
    if (level_text.empty()) {
        level_text = "auto";
    }
    if (!dztrader::sysmonstore::parse_query_level(level_text).has_value()) {
        return result;
    }

    // ---- max_points（缺省用默认值；给了就必须在区间内，不做静默钳制）----
    int64_t max_points = dztrader::sysmonstore::kDefaultMaxPoints;
    if (has_param(param, "max_points")) {
        const auto parsed = parse_int_param(param, "max_points");
        if (!parsed.has_value() || *parsed < dztrader::sysmonstore::kMinMaxPoints ||
            *parsed > dztrader::sysmonstore::kMaxMaxPoints) {
            return result;
        }
        max_points = *parsed;
    }

    // ---- to / from（to 缺省为当前时刻；from 缺省为 to - 6h）----
    int64_t to_ts = now_ts_;
    if (has_param(param, "to")) {
        const auto parsed = parse_int_param(param, "to");
        if (!parsed.has_value()) {
            return result;
        }
        to_ts = *parsed;
    }
    int64_t from_ts = to_ts - kDefaultRangeSec;
    if (has_param(param, "from")) {
        const auto parsed = parse_int_param(param, "from");
        if (!parsed.has_value()) {
            return result;
        }
        from_ts = *parsed;
    }
    // 契约：0 < from < to。`from <= 0` 无意义（Unix 秒纪元前的历史不存在），
    // `from >= to` 是空/倒置范围 —— 一律 400，不静默改成默认范围。
    if (from_ts <= 0 || to_ts <= from_ts) {
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
    QueryLevel level = requested == QueryLevel::Auto
                           ? dztrader::sysmonstore::select_auto_level(query.from_ts, query.to_ts)
                           : requested;
    // ---- 点数上限：多取一行精确判定是否截断（§6.1.2）----
    const int64_t fetch_rows = query.max_points + 1;

    nlohmann::json series_points = nlohmann::json::array();
    std::vector<int64_t> bucket_ts;
    std::vector<int64_t> row_interval_sec;  // 仅宽表有；长表原始行没有该列
    int64_t interval_sec = 0;
    bool truncated = false;
    int64_t effective_from = query.from_ts;

    try {
        // 取行并裁剪：降序取 max_points+1 行 → 精确裁剪 → 反转成升序。
        // 两个层级/两种指标的差异都收敛在这里的三个小 lambda 之后，主流程只有一条。
        auto fetch_scalars = [&](QueryLevel for_level) {
            auto rows =
                reader.query_scalars_desc(*dztrader::sysmonstore::query_level_to_storage(for_level),
                                          query.from_ts, query.to_ts, fetch_rows);
            const auto trim =
                dztrader::sysmonstore::trim_to_point_limit(rows.size(), query.max_points);
            rows.resize(trim.keep);
            std::reverse(rows.begin(), rows.end());
            return std::make_pair(std::move(rows), trim.truncated);
        };
        auto fetch_dimensions = [&](QueryLevel for_level) {
            auto rows = reader.query_dimensions_desc(
                ref.domain, query.dimension, ref.metric,
                *dztrader::sysmonstore::query_level_to_storage(for_level), query.from_ts,
                query.to_ts, fetch_rows);
            const auto trim =
                dztrader::sysmonstore::trim_to_point_limit(rows.size(), query.max_points);
            rows.resize(trim.keep);
            std::reverse(rows.begin(), rows.end());
            return std::make_pair(std::move(rows), trim.truncated);
        };

        // `auto` 选中的层级可能在本机根本不产出数据：压缩只在 `record_interval_sec < 60`
        // 时发生（默认配置是 60 ⇒ 从不压缩 ⇒ 一分钟聚合层永远为空）。此时若死守「auto 选的
        // 层级」不回退，默认部署下所有 >1 小时的范围都会返回空序列 —— 功能整体失效。
        // 因此拿到空结果时回退到另一层级，并把**实际使用的层级**回显在 `level` 字段里，
        // 回退本身对使用方可见（前端摘要显示「粒度 …」），不构成静默掩盖。
        std::vector<dztrader::sysmonstore::MetricAggregate> aggregates;

        auto load_level = [&](QueryLevel for_level) {
            if (ref.is_dimension) {
                auto [rows, was_truncated] = fetch_dimensions(for_level);
                truncated = was_truncated;
                aggregates.clear();
                aggregates.reserve(rows.size());
                bucket_ts.clear();
                bucket_ts.reserve(rows.size());
                for (const auto& row : rows) {
                    bucket_ts.push_back(row.bucket_ts);
                    aggregates.push_back(row.aggregate);
                }
            } else {
                auto [rows, was_truncated] = fetch_scalars(for_level);
                truncated = was_truncated;
                aggregates.clear();
                aggregates.reserve(rows.size());
                bucket_ts.clear();
                bucket_ts.reserve(rows.size());
                row_interval_sec.clear();
                row_interval_sec.reserve(rows.size());
                const size_t index =
                    static_cast<size_t>(ref.scalar - dztrader::sysmonstore::kScalarMetrics.data());
                for (const auto& row : rows) {
                    bucket_ts.push_back(row.bucket_ts);
                    aggregates.push_back(row.metrics[index]);
                    row_interval_sec.push_back(row.interval_sec);
                }
                // 桶宽取**最新一行**的 `interval_sec`：记录间隔可在运行期调整，跨调整点的一批
                // 行会有不同桶宽；最新的那一行代表当前生效的桶宽，也是画图最该用的刻度。
                if (!row_interval_sec.empty()) {
                    interval_sec = row_interval_sec.back();
                }
            }
            return !bucket_ts.empty();
        };

        bool found = load_level(level);
        if (!found && requested == QueryLevel::Auto) {
            // 回退到另一层级（auto 的偏好在本机没有数据时才这么做）
            const QueryLevel other =
                level == QueryLevel::Raw ? QueryLevel::Minute : QueryLevel::Raw;
            found = load_level(other);
            if (found) {
                level = other;
            }
        }
        for (size_t i = 0; i < bucket_ts.size(); ++i) {
            series_points.push_back(point_to_json(bucket_ts[i], aggregates[i]));
        }
    } catch (const std::exception& e) {
        // 查询期间的 SQL 失败同样降级（§6.1.3），不把半截数据当结果返回。
        SPDLOG_WARN("sysmon history query failed | metric={} error=\"{}\"", query.metric, e.what());
        return unavailable_body(HistoryAvailability::OpenFailed);
    }

    // 生效起点：被截断时为首个返回点的桶时刻（《帧契约：系统监控》§6.1.2 的精确口径）
    if (truncated && !bucket_ts.empty()) {
        effective_from = bucket_ts.front();
    }
    // 桶宽（《帧契约：系统监控》§6.1 的 interval_sec 口径）：
    // - 分钟聚合恒为 60；
    // - 原始层的宽表从行内 `interval_sec` 取；
    // - 原始层的长表**没有** `interval_sec` 列（维度行的存在本身表示有样本，桶宽是全表属性），
    //   因此从相邻桶时刻反推：相邻最小间隔就是名义桶宽（缺口只会拉大间隔，不会缩小）。
    // 范围内没有任何桶时保持 0，序列化时输出 null —— 无桶即无桶宽，不得猜一个值。
    if (!bucket_ts.empty()) {
        if (level == QueryLevel::Minute) {
            interval_sec = dztrader::sysmonstore::kMinuteResolutionSec;
        } else if (interval_sec <= 0) {
            interval_sec = derive_interval_from_buckets(bucket_ts);
        }
    }

    // ---- 标记与缺口（§6.1.4）----
    nlohmann::json marks_json = nlohmann::json::array();
    std::vector<dztrader::sysmonstore::HistoryMark> marks;
    try {
        // 标记**不**受 max_points 约束：标记解释缺口原因，若随点数上限一起截断，
        // 缺口会被错报成 unmarked（把「有解释的停机」误报成「无人解释的异常」）。
        // 标记数量由写入侧决定（每次重启/跳变/缺口一条），量级远小于记录点。
        marks = reader.query_marks(effective_from, query.to_ts, kMaxMarksPerQuery);
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
