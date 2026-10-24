#ifndef DZTRADER_SYSMONSTORE_RECORDS_H_
#define DZTRADER_SYSMONSTORE_RECORDS_H_

/**
 * @file records.h
 * @brief 历史库的行模型与保留策略（与 schema.h 共同构成唯一一份定义）。
 *
 * 「一条记录」= 一个时间桶里一个指标的 5 个聚合量（`min` / `max` / `avg` / `last` / `n`）。
 * 保留 5 个而不是只留均值，是因为尖峰只能由 `max` 保留（#41「尖峰由 max 保留」），
 * 而 `last` 让「当前值」在聚合粒度下仍然可读；`n` 是有效样本数，压缩时用它给 `avg`
 * 做加权合并——否则「部分样本不可用」的桶会把均值算偏。
 *
 * 为什么 `values` 统一用 `double`：SQLite 的 `INTEGER` 列与 `REAL` 列由 schema 声明
 * （见 `ScalarType`），而这里保持单一表示让 min/max/avg/last 的合并逻辑只有一份；
 * 字节/计数指标的原值上限远小于 2^53，double 表示无损，落库时按列类型显式取整
 * （见 `records.cpp` 的绑定）。
 */

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "schema.h"

namespace dztrader::sysmonstore {

/// 一个指标在一个时间桶内的聚合量。`count == 0` 表示该桶内没有有效样本（落库为 NULL）。
struct MetricAggregate {
    double min = 0.0;
    double max = 0.0;
    double avg = 0.0;
    double last = 0.0;
    int64_t count = 0;

    [[nodiscard]] bool empty() const noexcept { return count <= 0; }

    /// 并入一个样本（min/max/sum 递增，last 覆盖）。
    void observe(double value) noexcept;

    /// 并入另一个同粒度聚合（压缩时合并两个桶）：min/max 取极值，avg 按 `count` 加权，
    /// `last` 取 `other`（调用方保证 `other` 时间更晚）。
    void merge(const MetricAggregate& other) noexcept;
};

/// 宽表一行：一个时间桶的全部固定标量指标（`metrics` 与 `kScalarMetrics` 同序）。
struct ScalarRecord {
    int32_t level = kLevelRaw;  ///< 桶层级（`kLevelRaw` / `kLevelMinute`）
    int64_t bucket_ts = 0;      ///< 桶起始墙钟时刻（Unix 秒，UTC）
    int64_t interval_sec = 0;   ///< 该桶的宽度（秒）；压缩产物为 60
    std::array<MetricAggregate, kScalarMetrics.size()> metrics{};
};

/// 长表一行：一个时间桶内某个维度（CPU 核 / 卷 / 物理盘 / 网卡 / 温度传感器）的一个指标。
///
/// `dimension` 是**不透明标识**：CPU 核为十进制序号文本（`"3"`）、卷为挂载点、物理盘为
/// 设备名、网卡为接口名、温度传感器为稳定 sensor 标识。读取方按 `domain` 解释它，
/// 写入方不解析——CPU 核序号不保证连续，且各域标识类型不同，塞进固定列会把表结构
/// 钉死在某一台机器上。
struct DimensionRecord {
    int32_t level = kLevelRaw;
    int64_t bucket_ts = 0;
    std::string domain;
    std::string dimension;
    std::string metric;
    MetricAggregate aggregate;
};

/// 一条历史标记（采集中断 / 进程重启 / 时钟跳变）。
///
/// 标记是**事实记录**，不是填补数据的借口：读取方看到标记就知道那段时间没有可信数据，
/// 绝不因为标记存在而伪造连续序列。`detail` 为自由文本（数值型细节按十进制字符串存放）。
struct HistoryMark {
    int64_t ts = 0;      ///< 标记时刻（Unix 秒，UTC）
    std::string kind;    ///< `kMarkSampleGap` / `kMarkRestart` / `kMarkClockJump`
    std::string detail;  ///< 细节（如丢失 tick 数、跳变毫秒数）；无细节为空串
};

/// 一次批量提交的内容：若干桶 + 若干标记，在**单个事务**内落库。
struct HistoryBatch {
    std::vector<ScalarRecord> scalars;
    std::vector<DimensionRecord> dimensions;
    std::vector<HistoryMark> marks;

    [[nodiscard]] bool empty() const noexcept {
        return scalars.empty() && dimensions.empty() && marks.empty();
    }
    [[nodiscard]] size_t record_count() const noexcept {
        return scalars.size() + dimensions.size() + marks.size();
    }
    void clear() noexcept;
};

/// 保留与压缩策略（契约 sysmon §3.1 `history` 段 + `sampling.record_interval_sec`）。
struct RetentionPolicy {
    int64_t now_ts = 0;                      ///< 当前墙钟时刻（Unix 秒），由调用方注入便于测试
    int64_t raw_retention_sec = 24 * 3600;   ///< 原始记录保留时长（`raw_retention_hours`）
    int64_t max_retention_sec = 30 * 86400;  ///< 总保留上限（`max_retention_days`）
    int64_t record_interval_sec = 60;        ///< 记录间隔：< 60 时超期原始记录先压缩再删除
    int64_t max_bytes = 0;                   ///< 库文件总量上限（0 = 不限制）
    int64_t maintenance_batch = 1000;        ///< 单次压缩/删除的行数上限（避免长事务阻塞读方）
};

/// 保留策略执行结果（观测与测试用）。
struct RetentionResult {
    int64_t compacted_rows = 0;        ///< 被压缩掉的原始行数（标量 + 维度）
    int64_t compacted_buckets = 0;     ///< 整轮维护涉及的分钟桶数（去重）
    int64_t deleted_raw_rows = 0;      ///< 超期删除的原始**桶**数
    int64_t deleted_minute_rows = 0;   ///< 超期删除的分钟**桶**数
    int64_t deleted_marks = 0;         ///< 超期删除的标记条数
    int64_t deleted_by_size_rows = 0;  ///< 因总量上限被删除的**桶**数
    int64_t bytes_before = 0;          ///< 执行前库文件大小（字节）
    int64_t bytes_after = 0;           ///< 执行后库文件大小（字节）
};

/// 本机历史库的默认文件名（位于 `$DZTRADER_HOME/db/`，与交易库、WebUI 库分离）。
inline constexpr std::string_view kHistoryFileName = "sysmon.db";

// ===== 时间桶辅助（写入方与读取方共用同一对齐口径）=====

/**
 * @brief 求某时刻所属时间桶的起始时刻。
 *
 * 桶边界对齐到「相位偏移 + k × interval」的墙钟网格：`phase_offset_sec` 的用途是让
 * 记录 tick 避开整点附近的其他任务（K 线切分、页清理、预加载），因此默认记录间隔 60 秒
 * 时桶起点是每分钟的第 `phase` 秒（如 :03），**不落在整数分钟边界**。
 *
 * @param now_ts      时刻（Unix 秒）
 * @param interval_sec 桶宽（必须 > 0；调用方保证配置校验通过）
 * @param phase_sec   相位偏移（秒，钳制到 `[0, interval)`；契约允许 [0, 86400] 的越界值）
 * @return 桶起始时刻（Unix 秒）；`interval_sec <= 0` 时原样返回 `now_ts`
 */
[[nodiscard]] int64_t bucket_start(int64_t now_ts,
                                   int64_t interval_sec,
                                   int64_t phase_sec) noexcept;

/// 压缩目标桶（一分钟聚合）的起始时刻：整分钟对齐，便于与交易事件按墙钟对照。
[[nodiscard]] int64_t minute_bucket_start(int64_t ts) noexcept;

/**
 * @brief 检测墙钟跳变。
 *
 * 采样定时器用单调时钟（免疫 NTP 步进），因此「墙钟走过的时间」与「单调时钟走过的时间」
 * 应近似相等；两者差异超过阈值即说明墙钟被步进/回拨。跳变期间的历史桶按墙钟对齐，
 * 时间轴会错乱，必须打标记而不是让读取方把 NTP 步进误读成负载突变。
 *
 * @param prev_wall_ms   上次采样的墙钟（Unix 毫秒）
 * @param prev_steady_ms 上次采样的单调时钟（毫秒，同一基准）
 * @param wall_ms        本次采样墙钟
 * @param steady_ms      本次采样单调时钟
 * @param threshold_ms   判定阈值（毫秒）
 * @return 跳变毫秒数（带符号：正 = 墙钟前跳）；未超阈值返回 `std::nullopt`
 */
[[nodiscard]] std::optional<int64_t> detect_clock_jump(int64_t prev_wall_ms,
                                                       int64_t prev_steady_ms,
                                                       int64_t wall_ms,
                                                       int64_t steady_ms,
                                                       int64_t threshold_ms) noexcept;

}  // namespace dztrader::sysmonstore

#endif  // DZTRADER_SYSMONSTORE_RECORDS_H_
