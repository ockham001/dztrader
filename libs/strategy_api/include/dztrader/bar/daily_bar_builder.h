/**
 * @file daily_bar_builder.h
 * @brief 日线 tick→bar 合成器（ADR 0031）
 */
#ifndef DZTRADER_BAR_DAILY_BAR_BUILDER_H_
#define DZTRADER_BAR_DAILY_BAR_BUILDER_H_

#include <dztrader/bar/types.h>
#include <dztrader/struct.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace dztrader::bar {

/**
 * @brief 日线 tick→bar 合成器：一个实例 = 单合约
 *
 * 纯 tick 驱动、惰性收盘：没有 tick 就不产生任何动作，形成中的日线通过 current_bar() 读取。
 * on_tick 热路径无哈希、无字符串、无堆分配、无异常。
 *
 * 与 IntradayBarBuilder 的差别（ADR 0031 决策 7）：
 * - 不依赖交易小节与竞价表，只按 tick 的交易日与当日累计字段合成；
 * - open/high/low/close 直接取 tick 的当日累计字段（open_price/highest_price/lowest_price/
 *   last_price），不做 last_price 的极值统计、不做 tick 累计高低补采；
 * - 成交量/成交额直接用 tick 的当日累计字段：日线起点基准恒为 0，故 volume == cum_volume、
 *   turnover == cum_turnover；
 * - 日线标识固定 begin_sec = 0（18:00 标记）、period_sec = 0，调用方以 trading_day 判断日线结束。
 *
 * 交易日切换：交易日变大的第一笔有效 tick 到来时，先以 `is_final=true` 关闭前一日线，再以本笔
 * tick 开新日线；没有 tick 的交易日不生成空日线（相邻日线的 trading_day 可跳跃）。日线收盘同样
 * 是惰性的（下一交易日第一笔有效 tick 触发），不建定时器、不 flush。跨日后首笔 tick 不受前一
 * 交易日 last_tick_sec 的顺序约束（新交易日重新计时）。
 *
 * 数据守卫：丢弃的 tick 不改变任何 bar/累计状态、不触发 sink 回调，按下列固定顺序判定：
 * 1. `time` 越界（0..86399 之外）丢弃并计入 `out_of_session`；
 * 2. `date` 非正、或交易日相对当前 bar（含 restore final 的已收日线）倒退丢弃并计入
 *    `out_of_order`；
 * 3. 非法价格（`last_price` 非正/NaN/Inf）跳过，不计数；
 * 4. restore final 后同交易日的 tick 丢弃并计入 `out_of_order`（日线 begin_sec 恒为 0，
 *    不晚于已收日线，见 restore 注释）；
 * 5. 同一交易日内累计量或累计额倒退（含 NaN/Inf）丢弃并计入 `bad_volume`；跨交易日基准为 0，
 *    不视为坏数据。
 *
 * 事件顺序：同日每笔已接受 tick 恰好一次 update（首笔先 open）；交易日变化先 close 旧日线
 * （is_final=true），再 open 新日线（is_final=false），最后 update 新日线。
 *
 * @tparam Sink bar 事件接收者：可选实现 on_bar_open / on_bar_update / on_bar_close，
 *              未实现视为空操作；回调不得抛异常
 *
 * @note 续接与重置：restore() 只允许在构造/reset() 之后、尚未调用 on_tick() 时调用（同一
 *       reset 周期内只能调用一次，违反抛 std::invalid_argument）；period_sec 必须为 0、
 *       begin_sec 必须为 0，且 volume/turnover 必须与对应的累计字段一致（日线起点基准恒为 0），
 *       否则 fail-fast。restore final 只恢复交易日、不建立形成中 bar、不补发 close；下一次更晚
 *       交易日的 tick 才开新日线，其量额即该 tick 的当日累计量额（基准为 0）。restore forming
 *       采纳为当前形成中日线（视为已 open、不再补发 open）；同交易日且不早于 last_tick_sec 的
 *       tick 只 update，更晚交易日先 close（is_final=true）再 open。reset() 清空当前 bar、
 *       交易日/累计基准与统计，保留 sink 引用，用于回测重跑。
 */
template <class Sink = NullBarSink>
class DailyBarBuilder {
public:
    /**
     * @brief 构造 builder
     * @param sink bar 事件接收者（生命周期由调用方保证）
     */
    explicit DailyBarBuilder(Sink& sink) noexcept
        : sink_(sink) {}

    /**
     * @brief 喂入一笔 tick：同一交易日更新当前日线；交易日变大先收旧日线（close）再开新日线
     *        （open），随后 update 新日线
     *
     * 坏数据守卫按类注释的固定顺序判定；被丢弃的 tick 不建立/更新 bar、不推进交易日与量额基准、
     * 不触发回调。累计量额基准取自最近一笔已接受 tick 的累计值（当前 bar，或 restore final 恢复
     * 的基准；交易日变化必定换根，故跨交易日基准为 0）。
     *
     * @param tick 行情快照（调用方保证属于本实例绑定的合约）
     */
    void on_tick(const DzTick& tick) {
        tick_received_ = true;
        if (tick.time < 0 || tick.time > MAX_CLOCK_SEC) {
            ++stats_.out_of_session;
            return;
        }
        const int32_t tsec = TradingTime::from_clock(tick.time);
        if (tick.date <= 0) {
            ++stats_.out_of_order;
            return;
        }
        if (has_bar_) {
            if (tick.date < bar_.trading_day ||
                (tick.date == bar_.trading_day && tsec < bar_.last_tick_sec)) {
                ++stats_.out_of_order;
                return;
            }
        } else if (restored_final_ && tick.date < restored_final_bar_.trading_day) {
            ++stats_.out_of_order;
            return;
        }
        if (!std::isfinite(tick.last_price) || tick.last_price <= 0.0) {
            return;  // 非法价格只跳过，不计数、不推进任何基准
        }
        // restore final 后同交易日 tick 属于已收日线：日线 begin_sec 恒为 0，不再接受。
        if (!restored_final_accepts(tick)) {
            ++stats_.out_of_order;
            return;
        }
        if (!cumulative_basis_ok(tick)) {
            ++stats_.bad_volume;
            return;
        }
        if (!has_bar_) {
            begin_bar(tick, tsec);
        } else if (tick.date != bar_.trading_day) {
            finalize_bar();
            begin_bar(tick, tsec);
        } else {
            update_bar(tick, tsec);
        }
        notify_update(bar_);
    }

    /// @brief 是否已有形成中 bar（首笔 tick 之前为 false）
    [[nodiscard]] bool has_current_bar() const noexcept { return has_bar_; }

    /// @brief 当前形成中 bar（以 has_current_bar() 为 true 为前提，否则是调用方编程错误）
    [[nodiscard]] const Bar& current_bar() const noexcept { return bar_; }

    /// @brief 累计丢弃计数（构造后从 0 累计）
    [[nodiscard]] BarBuilderStats stats() const noexcept { return stats_; }

    /**
     * @brief 用归档的最后一根日线续接（final 或 forming），恢复交易日（forming 还恢复当前 bar）
     *
     * 只允许在构造/reset() 之后、尚未调用 on_tick() 时调用，同一 reset 周期内只能调用一次；
     * 违反前置条件、period_sec/begin_sec 不是日线取值，或量额与累计字段不一致时抛
     * std::invalid_argument（热路径不抛异常）。
     *
     * - final bar：只恢复交易日，不建立形成中 bar、不补发 close；下一次更晚交易日（量额基准
     *   按日重置为 0）的 tick 才开新日线。同交易日的 tick 属于已收日线，丢弃并计入
     *   out_of_order。
     * - forming bar：采纳为当前形成中日线，视为已 open、不再补发 open；同交易日且不早于
     *   last_tick_sec 的 tick 只 update，更晚交易日先 close（is_final=true）再 open。
     *
     * @param bar 归档的最后一根日线（period_sec 必须为 0、begin_sec 必须为 0）
     */
    void restore(const Bar& bar) {
        if (!restore_allowed()) {
            throw std::invalid_argument(
                "restore in non-pristine state | tick_received=" + std::to_string(tick_received_) +
                " has_bar=" + std::to_string(has_bar_) +
                " restored_final=" + std::to_string(restored_final_));
        }
        validate_restore_bar(bar);
        if (bar.is_final) {
            restored_final_ = true;
            restored_final_bar_ = bar;
            return;
        }
        bar_ = bar;
        bar_.is_final = false;
        has_bar_ = true;
    }

    /// @brief 清空当前 bar、交易日/累计基准与统计，保留 sink 引用
    ///
    /// 用于回测重跑：reset 后可重新 restore()，或从全新 tick 流开始完整序列。
    void reset() noexcept {
        bar_ = Bar{};
        has_bar_ = false;
        restored_final_ = false;
        restored_final_bar_ = Bar{};
        stats_ = BarBuilderStats{};
        tick_received_ = false;
    }

private:
    /// @brief 一日内的最大墙钟秒数（23:59:59）；time 越界计入 out_of_session，restore 的
    ///        last_tick_sec 用同一时间域校验
    static constexpr int32_t MAX_CLOCK_SEC = 86399;

    /**
     * @brief restore final 后的续接顺序守卫：只接受更晚交易日的 tick
     *
     * 日线 begin_sec 恒为 0，同交易日的 tick 都不晚于已恢复的已收日线，丢弃并计入
     * out_of_order（避免已收日线重新生成 bar）；更晚交易日不受限制（累计基准按日重置）。
     *
     * @param tick 行情快照（时间/日期/价格守卫已通过）
     * @return true = 可以开新日线；false = 应丢弃并计 out_of_order
     */
    [[nodiscard]] bool restored_final_accepts(const DzTick& tick) const noexcept {
        return !restored_final_ || tick.date > restored_final_bar_.trading_day;
    }

    /**
     * @brief 累计量额差分基准守卫：同一交易日内累计量/额不得倒退（成交额不得为 NaN/Inf）
     *
     * 基准取自最近一笔已接受 tick 的累计量额（当前 bar 或 restore final 恢复的基准）；交易日
     * 变化必定换根，故跨交易日基准为 0，不视为坏数据。
     *
     * @param tick 行情快照（时间/日期/价格守卫已通过）
     * @return true = 累计量额没有倒退，可接受
     */
    [[nodiscard]] bool cumulative_basis_ok(const DzTick& tick) const noexcept {
        const bool same_day_basis = has_day_basis() && tick.date == day_basis_trading_day();
        const int64_t basis_volume = same_day_basis ? day_basis_cum_volume() : 0;
        const double basis_turnover = same_day_basis ? day_basis_cum_turnover() : 0.0;
        // NaN/Inf 成交额不参与比较，用 isfinite 显式拒掉（否则 Inf 会永久毒化后续差分）。
        return static_cast<int64_t>(tick.volume) >= basis_volume && std::isfinite(tick.turnover) &&
               tick.turnover >= basis_turnover;
    }

    /// @brief 是否存在最近一笔已接受 tick 的累计量额基准（当前 bar 或 restore final）
    [[nodiscard]] bool has_day_basis() const noexcept { return has_bar_ || restored_final_; }

    /// @brief 累计量额基准所属交易日（以 has_day_basis() 为 true 为前提）
    [[nodiscard]] DzDate day_basis_trading_day() const noexcept {
        return has_bar_ ? bar_.trading_day : restored_final_bar_.trading_day;
    }

    /// @brief 累计成交量基准（以 has_day_basis() 为 true 为前提）
    [[nodiscard]] int64_t day_basis_cum_volume() const noexcept {
        return has_bar_ ? bar_.cum_volume : restored_final_bar_.cum_volume;
    }

    /// @brief 累计成交额基准（以 has_day_basis() 为 true 为前提）
    [[nodiscard]] double day_basis_cum_turnover() const noexcept {
        return has_bar_ ? bar_.cum_turnover : restored_final_bar_.cum_turnover;
    }

    /// @brief restore 前置状态：构造/reset 之后、任何 on_tick 之前，且尚未 restore 过
    [[nodiscard]] bool restore_allowed() const noexcept {
        return !tick_received_ && !has_bar_ && !restored_final_;
    }

    /**
     * @brief 校验待续接日线：周期/起始时间/交易日/最后 tick 时间与量额累计一致性
     * @param bar 待续接 bar
     * @throws std::invalid_argument 任一前置条件不满足
     */
    void validate_restore_bar(const Bar& bar) const {
        if (bar.period_sec != 0) {
            throw std::invalid_argument(
                "restore period mismatch | builder_period_sec=0 bar_period_sec=" +
                std::to_string(bar.period_sec));
        }
        if (bar.trading_day <= 0) {
            throw std::invalid_argument("restore invalid trading_day | trading_day=" +
                                        std::to_string(bar.trading_day));
        }
        if (bar.begin_sec != 0) {
            throw std::invalid_argument("restore invalid begin_sec | begin_sec=" +
                                        std::to_string(bar.begin_sec));
        }
        if (bar.last_tick_sec < 0 || bar.last_tick_sec > MAX_CLOCK_SEC) {
            throw std::invalid_argument("restore invalid last_tick_sec | last_tick_sec=" +
                                        std::to_string(bar.last_tick_sec));
        }
        if (!bar.is_final && !cumulative_fields_ok(bar)) {
            throw std::invalid_argument(
                "restore forming bar inconsistent | volume=" + std::to_string(bar.volume) +
                " cum_volume=" + std::to_string(bar.cum_volume) + " turnover=" +
                std::to_string(bar.turnover) + " cum_turnover=" + std::to_string(bar.cum_turnover));
        }
        if (bar.is_final && !cumulative_fields_ok(bar)) {
            throw std::invalid_argument(
                "restore final bar inconsistent | volume=" + std::to_string(bar.volume) +
                " cum_volume=" + std::to_string(bar.cum_volume) + " turnover=" +
                std::to_string(bar.turnover) + " cum_turnover=" + std::to_string(bar.cum_turnover));
        }
    }

    /// @brief 日线量额与累计字段一致（起点基准恒为 0）：volume == cum_volume 且非负有限、
    ///        turnover == cum_turnover
    [[nodiscard]] static bool cumulative_fields_ok(const Bar& bar) noexcept {
        return std::isfinite(bar.cum_turnover) && bar.cum_turnover >= 0.0 && bar.cum_volume >= 0 &&
               bar.volume == bar.cum_volume && bar.turnover == bar.cum_turnover;
    }

    /**
     * @brief 用本笔 tick 建立新的形成中日线并通知 sink 打开
     *
     * 日线起点基准恒为 0（当日累计字段即当日量额）；restore final 的基准在此消费掉。
     *
     * @param tick 行情快照
     * @param tsec 本笔 tick 的交易日内秒
     */
    void begin_bar(const DzTick& tick, int32_t tsec) {
        bar_ = Bar{};
        bar_.trading_day = tick.date;
        bar_.begin_sec = 0;
        bar_.period_sec = 0;
        bar_.is_final = false;
        has_bar_ = true;
        restored_final_ = false;  // 恢复基准已在本根建立时消费
        update_bar(tick, tsec);
        notify_open(bar_);
    }

    /// @brief 用本笔 tick 的当日累计字段更新形成中/新开日线的全部行情字段
    ///
    /// 日线口径直接从 tick 取累计 OHLC 与累计量额（不做 last_price 极值统计、不做高低补采），
    /// 故 bar 内量额恒等于累计量额。
    void update_bar(const DzTick& tick, int32_t tsec) {
        bar_.open = tick.open_price;
        bar_.high = tick.highest_price;
        bar_.low = tick.lowest_price;
        bar_.close = tick.last_price;
        bar_.last_tick_sec = tsec;
        bar_.volume = static_cast<int64_t>(tick.volume);
        bar_.turnover = tick.turnover;
        bar_.cum_volume = static_cast<int64_t>(tick.volume);
        bar_.cum_turnover = tick.turnover;
        bar_.open_interest = tick.open_interest;
    }

    /// @brief 以 is_final=true 关闭当前日线并通知 sink（下一笔更晚交易日的 tick 会开新日线）
    void finalize_bar() {
        bar_.is_final = true;
        notify_close(bar_);
    }

    /// @brief 若 Sink 实现 on_bar_open 则通知新 bar 已打开（缺失视为空操作）
    void notify_open(const Bar& bar) {
        if constexpr (requires(Sink& sink, const Bar& event_bar) { sink.on_bar_open(event_bar); }) {
            sink_.on_bar_open(bar);
        }
    }

    /// @brief 若 Sink 实现 on_bar_update 则通知当前 bar 已更新（缺失视为空操作）
    void notify_update(const Bar& bar) {
        if constexpr (requires(Sink& sink, const Bar& event_bar) {
                          sink.on_bar_update(event_bar);
                      }) {
            sink_.on_bar_update(bar);
        }
    }

    /// @brief 若 Sink 实现 on_bar_close 则通知当前 bar 已收（缺失视为空操作）
    void notify_close(const Bar& bar) {
        if constexpr (requires(Sink& sink, const Bar& event_bar) {
                          sink.on_bar_close(event_bar);
                      }) {
            sink_.on_bar_close(bar);
        }
    }

    Sink& sink_;                   ///< bar 事件接收者（生命周期事件回调）
    Bar bar_{};                    ///< 当前形成中日线
    BarBuilderStats stats_{};      ///< 丢弃计数（构造后从 0 累计）
    bool has_bar_ = false;         ///< 是否已有形成中 bar
    bool tick_received_ = false;   ///< on_tick 是否已调用过（restore 前置状态）
    bool restored_final_ = false;  ///< 已 restore final，等待更晚交易日 tick 开新日线
    Bar restored_final_bar_{};     ///< restore final 的 bar（交易日/累计基准）
};

}  // namespace dztrader::bar

#endif  // DZTRADER_BAR_DAILY_BAR_BUILDER_H_
