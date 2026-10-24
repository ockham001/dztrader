#ifndef DZTRADER_SYSMON_HISTORY_RECORDER_H_
#define DZTRADER_SYSMON_HISTORY_RECORDER_H_

/**
 * @file history_recorder.h
 * @brief 把每个计量 tick 的快照聚合成历史记录窗口（契约 sysmon；#78）。
 *
 * 职责边界（刻意保持纯逻辑，便于确定性测试）：
 * - **本类只做聚合**：给定一串「快照 + 墙钟时刻」，产出「若干时间桶的 min/max/avg/last」；
 * - **不做 I/O**：不碰数据库、不读时钟、不读配置——落库由落盘线程（`HistoryWriter`）承担，
 *   时刻由调用方注入。
 *
 * 记录窗口 = `sampling.record_interval_sec`（默认 60 秒）+ `sampling.phase_offset_sec`
 * 相位偏移。窗口边界按**墙钟**对齐，使历史能与交易事件对照；相位偏移让 tick 避开整点
 * 附近的其它任务（K 线切分、页清理、预加载），因此默认配置下窗口起点是每分钟的第 3 秒，
 * **不落在整数分钟边界**。
 *
 * 缺口与跳变纪律（本票的核心验收点）：缺一个样本就是缺一个样本——绝不补零、绝不插值、
 * 绝不把上一帧的值复制到本帧。窗口内一个样本都没有的指标落库为 NULL（读取方显示
 * 「不可用」而不是 0）。
 */

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/platform/sysmon.h>
#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>

namespace dztrader::sysmon {

/// 记录窗口配置（来自 `sampling.record_interval_sec` / `sampling.phase_offset_sec`）。
struct RecordWindowConfig {
    int64_t interval_sec = 60;           ///< 记录间隔（窗口宽度）
    int64_t phase_sec = 3;               ///< 相位偏移（窗口起点相对整数间隔的秒数）
    int64_t measure_interval_ms = 1000;  ///< 计量间隔，用于判断窗口内样本是否齐全
};

/**
 * @brief 记录窗口累加器：把一串快照聚合成可落库的批次。
 *
 * 生命周期：每个计量 tick 调 `observe()`；返回非空批次即表示「一个窗口结束了，把它落盘」。
 * 窗口切换由墙钟桶边界驱动（`bucket_start`），因此缺口、重启、时钟跳变都表现为
 * 「桶跳了不止一格」，本类据此给新窗口打标记。
 *
 * 非线程安全：只在 io_context 线程上使用。
 */
class HistoryRecorder {
public:
    explicit HistoryRecorder(RecordWindowConfig config);

    HistoryRecorder(const HistoryRecorder&) = delete;
    HistoryRecorder& operator=(const HistoryRecorder&) = delete;
    HistoryRecorder(HistoryRecorder&&) = delete;
    HistoryRecorder& operator=(HistoryRecorder&&) = delete;
    ~HistoryRecorder() = default;

    /**
     * @brief 观测一个计量 tick 的快照，必要时产出已结束窗口的批次。
     *
     * 语义：
     * - 首个观测只建立当前窗口，不产生批次；
     * - 同一窗口内的后续观测并入聚合（min/max/avg/last）；
     * - 窗口变化时产出**上一窗口**的批次并按新窗口重置；若上一窗口的样本数不足一个
     *   满窗（进程停顿/被强杀/时钟跳变），批次里带 `sample_gap` 标记（detail = 缺失
     *   样本数），绝不把缺口补成连续数据。
     *
     * @param snapshot 本 tick 的快照事实（采集/派生结果，不是编码后的 JSON）
     * @param now_ts   本 tick 的墙钟时刻（Unix 秒，调用方注入）
     * @return 已结束窗口的批次；没有窗口结束时空批次
     */
    [[nodiscard]] sysmonstore::HistoryBatch observe(const platform::SysmonSnapshot& snapshot,
                                                    int64_t now_ts);

    /**
     * @brief 结束当前窗口并产出批次（优雅退出路径）。
     *
     * 退出时窗口通常不满一个周期，但**已采到的样本必须落盘**——丢弃它们会让
     * 「进程退出前发生了什么」永久缺失。不完整的窗口带 `sample_gap` 标记，
     * 读取方据此知道该窗口的均值只基于部分样本。
     *
     * @param now_ts 当前墙钟时刻；退出结算时缺口标记以此为时间戳（窗口起始时刻已由
     *               数据行的 `bucket_ts` 表达，标记记「退出时发现它没满」这一时刻）
     * @return 当前窗口的批次（含退出时尚未落库的待写标记）；从未观测到样本时仍返回
     *         仅含标记的批次——重启/时钟跳变标记必须落库，不能因为没有样本就丢掉
     */
    [[nodiscard]] sysmonstore::HistoryBatch flush(int64_t now_ts);

    /**
     * @brief 运行期更新记录窗口配置（`SET_SYSMON_CONFIG` 之后调用）。
     *
     * 只改窗口宽度与相位，**不丢弃**当前已累计的窗口：已观测的样本属于它开始时那个
     * 窗口，按旧宽度落库才不会把历史切碎。新配置从下一个窗口起生效。
     *
     * @param config 新配置（非法间隔退回默认，与构造时同一防御）
     */
    void reconfigure(RecordWindowConfig config);

    /// 上报本窗口内计量定时器跳过的 tick 数（#70 的 `sampling.missed_ticks` 增量）。
    ///
    /// 与「样本数不足」是两个独立证据：定时器明确报告跳过时才算「错过 tick」，
    /// 因此这里注入的值会在窗口结算时与样本数一起决定缺口标记。
    void note_missed_ticks(uint64_t missed) noexcept;

    /// 给下一次产出的批次追加一条标记（采集中断 / 重启 / 时钟跳变由调用方判定后注入）。
    ///
    /// 标记随**下一个**产出的批次一起落库：标记与它所描述的数据同批次提交，
    /// 避免「标记已落库、对应窗口还没落」的中间状态。
    void add_mark(sysmonstore::HistoryMark mark);

    /// 取走当前待落盘标记（立即持久化用，不影响正在累计的窗口数据）。
    ///
    /// 返回的批次**只含标记**、不含窗口数据：调用方用它把「必须马上落库」的事实
    /// （如启动时的重启标记）单独提交，而不必等窗口结算。
    [[nodiscard]] sysmonstore::HistoryBatch drain_marks();

    /// 当前窗口的桶起始时刻；尚未观测到样本时为 `std::nullopt`。
    [[nodiscard]] std::optional<int64_t> current_bucket_ts() const noexcept { return bucket_ts_; }

    /// 当前窗口已累计的样本数。
    [[nodiscard]] int64_t sample_count() const noexcept { return samples_in_window_; }

    /// 一个满窗应有的样本数（`interval_sec * 1000 / measure_interval_ms`，至少 1）。
    [[nodiscard]] int64_t expected_samples_per_window() const noexcept;

    /// 窗口内样本数是否达到满窗（允许 ±1 的调度抖动）。
    [[nodiscard]] bool window_is_full(int64_t samples) const noexcept;

    /// 一个窗口的维度聚合槽位（域 + 维度 + 指标 → 聚合量）。公开是为了让
    /// `observe_values` 的实现类（`WindowAccumulator`）能构造它——它只是数据结构，
    /// 不暴露任何不变量。
    struct DimensionSlot {
        std::string domain;
        std::string dimension;
        std::string metric;
        sysmonstore::MetricAggregate aggregate;
    };

private:
    /// 把快照的全部标量与维度值观测进当前窗口。
    void observe_values(const platform::SysmonSnapshot& snapshot);

    /// 把当前窗口转成批次（含待落盘标记）并重置累加状态。
    [[nodiscard]] sysmonstore::HistoryBatch take_batch(int64_t bucket_ts);

    /// 按窗口实际样本数与期间检测到的错过 tick 数追加缺口标记（都正常时不追加）。
    ///
    /// 两个来源合起来才是「采集中断」的完整事实：
    /// - `samples` 少于满窗 ⇒ 窗口内确实少了样本（进程停顿/被强杀）；
    /// - `missed_ticks` > 0 ⇒ 计量定时器明确报告跳过了 tick（#70 的缺口计数）。
    /// 后者比「样本数不足」更精确：窗口恰好卡在边界时样本数可能差一个但并没有丢失 tick。
    ///
    /// `samples` 由调用方在累加状态重置前取好——重置后读到的是 0。
    void mark_if_incomplete(sysmonstore::HistoryBatch& batch,
                            int64_t bucket_ts,
                            int64_t samples,
                            uint64_t missed_ticks) const;

    RecordWindowConfig config_;
    std::optional<int64_t> bucket_ts_;  ///< 当前窗口起始时刻
    /// 当前窗口**开启时**的记录间隔。运行期改配置不改变已打开窗口的宽度：
    /// 该窗口的样本是按这个间隔采的，落库的 `interval_sec` 必须与它一致，
    /// 否则历史里的「窗口宽度」与「实际采样粒度」对不上。
    int64_t window_interval_sec_ = 60;
    int64_t samples_in_window_ = 0;
    std::array<sysmonstore::MetricAggregate, sysmonstore::kScalarMetrics.size()> scalars_{};
    std::vector<DimensionSlot> dimensions_;
    std::vector<sysmonstore::HistoryMark> pending_marks_;
    /// 本窗口内计量定时器明确报告跳过的 tick 数（由调用方经 `note_missed_ticks` 注入）。
    uint64_t missed_ticks_in_window_ = 0;
};

/**
 * @brief 时钟跳变检测阈值（毫秒）。
 *
 * 采样定时器走单调时钟，因此墙钟与单调时钟的推进应基本一致；差异超过该阈值即判定
 * 墙钟被步进/回拨（NTP 校时、手工改时间、虚拟机暂停恢复）。取 5 秒：正常运行时
 * 舍入误差在毫秒级，而 NTP 步进通常是一次性跳几百毫秒到数秒，5 秒能过滤抖动又不漏
 * 真实跳变。
 */
inline constexpr int64_t kClockJumpThresholdMs = 5000;

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_HISTORY_RECORDER_H_
