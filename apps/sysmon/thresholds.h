#ifndef DZTRADER_SYSMON_THRESHOLDS_H_
#define DZTRADER_SYSMON_THRESHOLDS_H_

/**
 * @file thresholds.h
 * @brief 阈值越限的判定、周期重记与恢复（契约 sysmon §2.8；#123）。
 *
 * 边界（刻意保持纯逻辑，便于确定性测试）：
 * - 本类**不读时钟、不读全局配置、不写帧、不写日志**：判定所需的资源事实、配置与两个注入
 *   时刻（墙钟用于活动期起点、单调时钟用于重记间隔）全部由调用方给出；
 * - 本类只产出「待写日志记录」；记录怎么落、落到哪个 sink 由调用方（`main.cpp`）负责。
 *
 * 状态机语义（契约 sysmon §2.8）：
 * - 每个判定对象（内存 / 交换 / CPU 饱和各一个，卷逐卷一个）维护一个**活动期**：
 *   首次越限进入活动期（记 `since` 墙钟与单调时钟基准），真正回落到阈值以内时结束；
 * - 进入记一条（`warning`）、恢复记一条（`info`）；同一次活动期内阈值持续满足**不重记**，
 *   但每满 `log_repeat_sec`（默认 600s，单调时钟计）记一条「仍越限」（`info`，带已持续秒数）；
 * - `log_repeat_sec = 0` 表示不重记——一次活动期只有进入与恢复两条；
 * - 本帧读不到数值（能力仍在）时**保留**活动状态、不写恢复——"读不到"不是"已恢复"；
 * - 能力被配置关闭或判定对象消失时**静默清除**（清除原因不是"指标回到阈值以下"），
 *   不写恢复记录。
 *
 * 与旧实现（#80 `AlertEvaluator`）的差异：不再有通知冷却与恢复限流（不再有通知可限流），
 * 防刷屏完全由「进入/恢复各一条 + 按间隔重记」保证（ADR 0034）。
 */

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include <dztrader/platform/sysmon.h>

#include "snapshot_builder.h"

namespace dztrader::sysmon {

/// 卷容量事实（本机全部卷中本帧容量可读的条目）。
struct VolumeUsage {
    std::string mount;         ///< 卷挂载点（与 `volumes[].mount` 同口径）
    uint64_t total_bytes = 0;  ///< 卷总容量（> 0；总量为 0 的卷不参与判定）
    uint64_t free_bytes = 0;   ///< 卷剩余容量（已钳制到 `[0, total_bytes]`）
};

/// 一次计量得到的判定输入事实。
///
/// `*_applicable = false` 表示该指标类别已被配置关闭（或本机不再具备该能力，如交换被移除）：
/// 对应活动期被**静默清除**（配置关闭不是恢复）。`optional` 数值为 `nullopt` 表示
/// 「能力仍在但本帧未采到」：保留活动状态，不伪造恢复。
struct ThresholdInputs {
    bool memory_applicable = false;           ///< 内存采集能力是否仍在
    std::optional<double> memory_used_ratio;  ///< 内存使用率 `[0,1]`；nullopt = 本帧未采到
    bool swap_applicable = false;             ///< 交换能力是否仍在（无交换/已移除时 false）
    std::optional<double> swap_used_ratio;    ///< 交换使用率 `[0,1]`；nullopt = 本帧未采到
    /// CPU 饱和判定是否适用（`sampling.groups.cpu = false` 时为 false → 静默清除）。
    bool cpu_applicable = false;
    /// 本帧每核使用率是否**确实可读**（至少一个核 `usage_ratio != null`）。
    /// `cpu_saturated` 不得按「saturated_cores 为空」判恢复：每核序列整体未采到时
    /// `saturated` 全部为 false（契约 §2.7），直接按"空即恢复"会把采集失败误记成恢复。
    bool cpu_cores_readable = false;
    std::vector<uint32_t> saturated_cores;  ///< 本帧饱和核 index 集合（可为空）
    bool volumes_applicable = false;        ///< 卷容量采集能力是否仍在
    /// 本帧容量可读的**全部卷**（`mount` 单帧内唯一，`total_bytes > 0`）。
    /// 判定范围恒为本机全部卷，不存在「关注哪一卷」这一层（ADR 0034）。
    std::vector<VolumeUsage> volumes;
    /// 本帧枚举到的全部挂载点（含容量读不到与总量为 0 的卷），用于识别"卷已消失"。
    /// `nullopt` = 本帧卷域整体不可读（无法判断消失，不得据此静默清除任何卷）。
    std::optional<std::vector<std::string>> known_mounts;
    int64_t now_unix_sec = 0;      ///< 墙钟时刻（只用于活动期起点，Unix 秒）
    int64_t now_monotonic_ms = 0;  ///< 单调时钟毫秒（只用于重记计时，不受系统时间调整影响）
};

/**
 * @brief 由一次计量的指标事实构造判定输入（内存/交换口径换算在此完成）。
 *
 * 转换口径与快照一致：`used = total − available/free`（契约 sysmon §2.4 的唯一口径）；
 * 能力被配置关闭或本机无交换时 `*_applicable = false`（活动期静默清除）。
 * 卷只收容量可读且总量 > 0 的条目——总量为 0 的卷无法计算百分比，不参与越限判定
 * （它是"容量可读但总量为 0"的合法事实，与"读不到"是两件事）。
 *
 * @param cpu_metrics     CPU 测量结果（含配置关闭标记与饱和核集合）
 * @param memory_metrics  内存/交换测量结果（含配置关闭与无交换标记）
 * @param disk_metrics    卷测量结果（含配置关闭标记）
 * @param now_unix_sec    墙钟时刻（Unix 秒，写入活动期起点）
 * @param now_monotonic_ms 单调时钟毫秒（重记计时）
 * @return 可直接交给 `ThresholdEvaluator::evaluate` 的输入
 */
[[nodiscard]] ThresholdInputs make_threshold_inputs(const CpuMetrics& cpu_metrics,
                                                    const MemorySwapMetrics& memory_metrics,
                                                    const DiskMetrics& disk_metrics,
                                                    int64_t now_unix_sec,
                                                    int64_t now_monotonic_ms);

/// 判定对象标识：kind + subject（`volume_free` 时 subject 为挂载点，其余为空串）。
///
/// `(kind, subject)` 在单次评估内唯一，本类型是状态表键的单一标识类型——
/// 避免把"两个字符串"当裸 pair 在接口间传递。
struct ThresholdKey {
    std::string kind;
    std::string subject;
    [[nodiscard]] auto operator<=>(const ThresholdKey& other) const = default;
};

/// 一次活动期内的记录状态（契约 sysmon §2.8 的三态）。
enum class ThresholdState {
    Entered,    ///< 本帧进入越限（活动期起点，级别 warning）
    Ongoing,    ///< 活动期内按 `log_repeat_sec` 周期重记（级别 info）
    Recovered,  ///< 本帧真正回落到阈值以内、活动期结束（级别 info）
};

/// 需要写的一条日志记录（不构成协议语义；调用方负责落日志）。
///
/// 字段覆盖契约 §2.8 的内容义务：判定对象、状态、当前值、阈值、活动期已持续秒数。
/// `message` 是组装好的英文 `prose | k=v` 单行文本，直接交给 spdlog 即可
/// （避免调用方再按 state 分支拼一遍，两处口径不得漂移）。
struct ThresholdLogRecord {
    ThresholdKey key;                                       ///< 判定对象标识
    ThresholdState state = ThresholdState::Entered;         ///< 进入 / 重记 / 恢复
    spdlog::level::level_enum level = spdlog::level::warn;  ///< 日志级别（进入 warn、其余 info）
    /// 当前值：使用率/卷空闲的比率 `[0,1]`；`cpu_saturated` 为**本帧饱和核数**（无量纲）。
    double current_value = 0.0;
    /// 阈值：与 `current_value` 同口径的比率。`cpu_saturated` 没有阈值语义（饱和判定由
    /// `thresholds.cpu_sustain_*` 在采集阶段完成，越限层只看"有没有饱和核"）时为 nullopt。
    std::optional<double> threshold_value;
    int64_t active_sec = 0;  ///< 活动期已持续秒数（进入时为 0）
    std::string message;     ///< 组装好的英文单行日志文本（`prose | k=v`）
};

/**
 * @brief 阈值越限状态机：进入 / 周期重记 / 恢复。
 *
 * 非线程安全：只在 io_context 线程上使用。
 */
class ThresholdEvaluator {
public:
    /// 一次评估的产物。
    struct Result {
        std::vector<ThresholdLogRecord> records;  ///< 本帧需要写的日志记录（可能为空）
    };

    ThresholdEvaluator() = default;
    ThresholdEvaluator(const ThresholdEvaluator&) = delete;
    ThresholdEvaluator& operator=(const ThresholdEvaluator&) = delete;
    ThresholdEvaluator(ThresholdEvaluator&&) = delete;
    ThresholdEvaluator& operator=(ThresholdEvaluator&&) = delete;
    ~ThresholdEvaluator() = default;

    /**
     * @brief 用本帧事实与当前生效配置推进状态机。
     *
     * @param inputs 资源事实 + 注入的两个时刻
     * @param config 当前生效 `thresholds` 配置（调用方须保证已经过校验）
     * @return 本帧待写日志记录（按 kind、subject 升序）
     */
    [[nodiscard]] Result evaluate(const ThresholdInputs& inputs,
                                  const platform::SysmonThresholdsConfig& config);

private:
    /// 单个判定对象的**活动期**状态（每次"进入越限 → 回到阈值以内"是一次活动期）。
    struct SubjectState {
        bool active = false;             ///< 当前是否处于活动期
        int64_t since_unix_sec = 0;      ///< 本次活动的起点（墙钟，Unix 秒）
        int64_t since_monotonic_ms = 0;  ///< 本次活动期起点（单调时钟，重记基准）
        /// 下一条「仍越限」的里程碑（单调时钟；按活动期起点对齐，见 transition 注释）
        int64_t next_repeat_monotonic_ms = 0;
    };

    /// 推进一个"使用率"类判定对象（内存 / 交换同构：达到阈值进入、低于阈值恢复）。
    /// @param kind          判定对象 kind（契约 sysmon §2.8，同时用作日志里的对象标识）
    /// @param applicable    采集能力是否仍在；false = 静默清除该 kind 的全部状态
    /// @param used_ratio    本帧使用率；nullopt = 本帧未采到（保留活动状态）
    /// @param threshold_pct 使用率阈值（百分比，[0,100]）
    /// @param repeat_ms     持续越限的重记间隔（毫秒；0 = 不重记）
    void evaluate_usage(const char* kind,
                        bool applicable,
                        const std::optional<double>& used_ratio,
                        double threshold_pct,
                        int64_t repeat_ms,
                        const ThresholdInputs& inputs,
                        Result& out);

    /// 一次状态机推进的结果（只描述"发生了什么"，不含任何展示口径）。
    ///
    /// 把状态记账与日志文本组装分开：不同判定对象的"当前值/阈值"口径不同
    /// （比率 vs 饱和核数），混在一个签名里必然要为某个对象编造无意义的数值。
    struct TransitionOutcome {
        ThresholdState state = ThresholdState::Entered;  ///< 本次产生的记录类型
        bool produced = false;   ///< 本帧是否产生了记录（否则动作为静默清空/无变化）
        int64_t active_sec = 0;  ///< 活动期已持续秒数（进入时为 0）
    };

    /// 推进一个判定对象的状态机，返回"本帧产生了哪类记录"。
    ///
    /// @param key        判定对象标识
    /// @param breached   本帧是否处于越限状态
    /// @param repeat_ms  持续越限的重记间隔（毫秒；0 = 不重记）
    [[nodiscard]] TransitionOutcome transition(const ThresholdKey& key,
                                               bool breached,
                                               int64_t repeat_ms,
                                               const ThresholdInputs& inputs);

    /// 组装一条记录（级别随状态：进入 warn，其余 info）。
    static ThresholdLogRecord make_record(const ThresholdKey& key,
                                          const TransitionOutcome& outcome,
                                          double current_value,
                                          std::optional<double> threshold_value,
                                          std::string message);

    /// 静默清除某一 kind 的全部状态（配置关闭 / 能力消失；配置关闭不是"指标恢复"）。
    void erase_kind(const std::string& kind);

    /// 周期重记判定：到达按活动期起点对齐的下一个里程碑时返回 true（契约 §2.8）。
    ///
    /// 里程碑由 `transition` 在进入与每次重记时推进；`log_repeat_sec = 0` 恒为 false。
    /// 运行期改动间隔后，下一次重记按"新间隔"从当前里程碑续算（不补发、不立刻触发）。
    static bool repeat_due(const SubjectState& state, int64_t now_monotonic_ms, int64_t repeat_ms);

    std::map<ThresholdKey, SubjectState> states_;
};

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_THRESHOLDS_H_
