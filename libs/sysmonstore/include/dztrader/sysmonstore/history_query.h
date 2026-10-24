#ifndef DZTRADER_SYSMONSTORE_HISTORY_QUERY_H_
#define DZTRADER_SYSMONSTORE_HISTORY_QUERY_H_

/**
 * @file history_query.h
 * @brief
 * 历史查询的**语义层**：指标键解析、层级选择、点数上限与缺口归并（《帧契约：系统监控》§6.1）。
 *
 * 为什么查询语义放在共享模块而不是 dzweb：这些规则全部由 schema 的形状决定（哪些逻辑键
 * 存在、宽表与长表怎么区分、桶宽从哪来），放进 dzweb 会让 schema 的知识出现第二份。
 * dzweb 只负责把 HTTP query 参数映射成本模块的入参、再把结果序列化成 JSON。
 *
 * 本模块**不碰 SQLite**：只做纯函数（解析/选择/归并）。真正的行读取仍由
 * `HistoryReader`（`history_store.h`）承担，本模块只消费它的输出。
 *
 * 线程约定：全部为无状态纯函数，可任意线程调用。
 */

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "records.h"
#include "schema.h"

namespace dztrader::sysmonstore {

// ===== 指标键解析 =====

/// 查询目标：宽表的一个标量指标，或长表的一个 (域, 指标) 组合。
struct MetricRef {
    /// 宽表：`scalar` 非空、`domain`/`metric` 为空；长表：`scalar` 为空、其余非空。
    const ScalarMetricSpec* scalar = nullptr;
    std::string_view domain;  ///< 长表域标识（`k*Domain`）
    std::string_view metric;  ///< 长表指标名（属于该域）
    bool is_dimension = false;

    [[nodiscard]] bool valid() const noexcept { return scalar != nullptr || is_dimension; }
};

/**
 * @brief 解析查询用的指标键（《帧契约：系统监控》§6.1 的 `metric` 参数）。
 *
 * 接受两种形式：
 * - 宽表逻辑键：直接等于 `kScalarMetrics[].key`（如 `cpu.usage_ratio`）；
 * - 长表标识：`<domain>.<metric>`（如 `network.rx_bytes_per_sec`），域与指标都必须已注册。
 *
 * 歧义处理：先匹配宽表键（宽表键本身含点，如 `memory.used_bytes`），未命中再按
 * 「第一个 `.` 切分」尝试长表——域标识都不含点，因此切分点唯一，不存在需要回溯的情况。
 *
 * @return 解析结果；未知键返回无效 `MetricRef`（`valid()==false`）
 */
[[nodiscard]] MetricRef resolve_metric(std::string_view key) noexcept;

// ===== 层级选择 =====

/// 查询粒度（《帧契约：系统监控》§6.1 的 `level` 参数）。
enum class QueryLevel { Auto, Raw, Minute };

/// 解析 `level` 参数；未知取值返回 `std::nullopt`（调用方回 400）。
[[nodiscard]] std::optional<QueryLevel> parse_query_level(std::string_view text) noexcept;

/// 层级 -> 稳定字符串（`raw` / `minute`），用于响应回显。
[[nodiscard]] std::string_view query_level_name(QueryLevel level) noexcept;

/// 层级 -> 历史库的 `level` 列值；`Auto` 不是具体层级，返回 `std::nullopt`。
[[nodiscard]] std::optional<int32_t> query_level_to_storage(QueryLevel level) noexcept;

/**
 * @brief `level=auto` 的层级**偏好**（《帧契约：系统监控》§6.1.1）：范围宽度 ≤ 1 小时选原始层，
 *        否则分钟层。
 *
 * 这里只给偏好、不做「该层级有没有数据」的判断：本函数是纯函数，拿不到库。
 * 调用方拿到偏好后查询，若该层级无行则回退到另一层级并把实际层级回显（《帧契约：系统监控》§6.1.1
 * 的回退规则） ——默认配置 `record_interval_sec = 60` 时从不压缩，分钟层永远为空，不回退会让所有
 * 超过 1 小时的范围返回空序列。
 *
 * @param from_ts 起始时刻（Unix 秒）
 * @param to_ts   结束时刻（Unix 秒，排他）
 * @return 偏好层级（恒为 `Raw` 或 `Minute`，不会是 `Auto`）
 */
[[nodiscard]] QueryLevel select_auto_level(int64_t from_ts, int64_t to_ts) noexcept;

/// `auto` 层级切换的范围阈值（秒）：范围不超过该值时用原始记录。
inline constexpr int64_t kAutoRawMaxRangeSec = 3600;

/// 每序列点数上限的合法区间（《帧契约：系统监控》§6.1 的 `max_points` 参数）。
inline constexpr int64_t kMinMaxPoints = 10;
inline constexpr int64_t kMaxMaxPoints = 20000;

/// `max_points` 未指定时的默认值。
inline constexpr int64_t kDefaultMaxPoints = 1500;

// ===== 点数上限 =====

/**
 * @brief 按点数上限裁剪的判定（《帧契约：系统监控》§6.1.2）。
 *
 * 读取方**按时间降序**取 `max_points + 1` 行：多取一行才能**精确**判定「范围内是否还有
 * 更旧的桶」——只按范围宽度除以桶宽去估算会把「起点不在桶边界上」「这一段时间本就没有
 * 数据」都误判成截断（两者都不该丢任何数据）。因此截断与否由实际取回的行数决定，
 * 不做估算。
 *
 * 为什么保留最新而非最早：使用者看的是「最近一段发生了什么」，截掉旧数据比截掉新数据有用。
 * 裁剪后不跨桶合并、不插值、不重采样——合并会造出历史库里不存在的桶边界，
 * `min`/`max` 包络的语义随之改变（不再是记录窗口的包络）。
 *
 * @param fetched_rows 降序取回的行数
 * @param max_points   点数上限（> 0）
 * @return `keep` = 应保留的行数（从降序结果的前端取），`truncated` = 是否丢弃了更旧的行
 */
struct PointLimitTrim {
    size_t keep = 0;
    bool truncated = false;
};
[[nodiscard]] PointLimitTrim trim_to_point_limit(size_t fetched_rows, int64_t max_points) noexcept;

// ===== 缺口归并 =====

/// 序列内不连续区间（《帧契约：系统监控》§6.1.4 的 `gaps[]` 元素）。
struct Gap {
    int64_t from_ts = 0;  ///< 缺口起点（上一桶的结束时刻）
    int64_t to_ts = 0;    ///< 缺口终点（下一桶的起始时刻）
    std::string kind;     ///< 原因：`restart` / `sample_gap` / `unmarked`
};

/// 无标记解释缺口时使用的 kind（「数据确实缺了但没有标记」本身就是要暴露的事实）。
inline constexpr std::string_view kGapUnmarked = "unmarked";

/**
 * @brief 从桶时刻序列与标记归并出缺口（《帧契约：系统监控》§6.1.4）。
 *
 * 判定只看**相邻桶时刻的连续性**（`cur - (prev + interval_sec) > 0`），这是外部可观察事实；
 * 标记只用来解释缺口原因：落在 `[from, to)` 内的标记中 `restart` 优先于 `sample_gap`，
 * 都没有则为 `unmarked`。`clock_jump` 不产生缺口（跳变期间数据可能仍连续）。
 *
 * @param bucket_ts  已按升序排列的桶起始时刻
 * @param interval_sec 桶宽（秒，> 0）
 * @param marks      该时间范围内的标记（顺序无关）
 * @return 按 `from_ts` 升序、互不重叠的缺口列表；点数 < 2 时为空
 */
[[nodiscard]] std::vector<Gap> derive_gaps(const std::vector<int64_t>& bucket_ts,
                                           int64_t interval_sec,
                                           const std::vector<HistoryMark>& marks);

}  // namespace dztrader::sysmonstore

#endif  // DZTRADER_SYSMONSTORE_HISTORY_QUERY_H_
