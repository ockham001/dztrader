/**
 * @file intraday_bar_builder.h
 * @brief 日内容周期 tick→bar 合成器（ADR 0031）
 */
#ifndef DZTRADER_BAR_INTRADAY_BAR_BUILDER_H_
#define DZTRADER_BAR_INTRADAY_BAR_BUILDER_H_

#include <dztrader/bar/types.h>
#include <dztrader/struct.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace dztrader::bar {

/**
 * @brief 日内容周期 tick→bar 合成器：一个实例 = 单合约 + 单周期
 *
 * 纯 tick 驱动、惰性收盘：没有 tick 就不产生任何动作，形成中的 bar 通过
 * current_bar() 读取。构造时校验周期与小节表，非法输入抛 std::invalid_argument；
 * on_tick 热路径无哈希、无字符串、无堆分配、无异常。
 *
 * 交易时段：tick 必须落在某个交易小节内（端点含，多小节间休息与非交易时段均为
 * 丢弃并计入 stats().out_of_session），否则不改变任何 bar/累计状态、不触发回调。
 * 小节末端（tsec == session.end_sec）的 tick 归前一根 bar。
 *
 * 周期可以是任意正整数秒：桶起点 = floor(tsec / period_sec) * period_sec（18:00 锚点
 * 整数网格），1 小时周期即自然整点对齐；小节休息落在桶内时，休息前后小节的 tick
 * 拼进同一根 bar，不按小节切根。
 *
 * 桶边界事件顺序：新 bar 首笔 tick 先 close 旧 bar（is_final=true），再 open 新 bar
 * （is_final=false），最后 update 新 bar；同桶 tick 只 update。无成交的桶不生成
 * bar（相邻 bar 的 begin_sec 可跳跃）。
 *
 * @tparam Sink bar 事件接收者：可选实现 on_bar_open / on_bar_update / on_bar_close，
 *              未实现视为空操作；回调不得抛异常
 *
 * @note 交易日变化按「换根」处理（累计基准按日重置、旧根先 close），但跨日的空交易日、
 *       夜盘组合等完整语义与集合竞价、续接（restore）由后续票定稿，公开方法只增不改。
 */
template <class Sink = NullBarSink>
class IntradayBarBuilder {
public:
    /**
     * @brief 构造 builder
     * @param table 交易小节与竞价窗口表（按值保存；非法表抛 std::invalid_argument）
     * @param period_sec 周期秒数（正整数；非法值抛 std::invalid_argument）
     * @param sink bar 事件接收者（生命周期由调用方保证）
     */
    IntradayBarBuilder(const TradingSessionTable& table, int32_t period_sec, Sink& sink)
        : table_(table),
          period_sec_(period_sec),
          sink_(sink) {
        if (period_sec <= 0) {
            throw std::invalid_argument("invalid period_sec | period_sec=" +
                                        std::to_string(period_sec));
        }
        if (!table.valid()) {
            throw std::invalid_argument("invalid trading session table");
        }
    }

    /**
     * @brief 喂入一笔 tick：非交易小节内的 tick 丢弃并计数；同桶更新当前 bar；跨桶或
     *        交易日变化先收旧 bar（close）再开新 bar（open）并更新；每笔已接受 tick
     *        恰好一次 update
     *
     * 坏数据守卫按下列固定顺序判定，被丢弃的 tick 不改变任何 bar/累计状态、不触发回调：
     * 1. `time` 越界（0..86399 之外）丢弃并计入 `out_of_session`；
     * 2. `date` 非正、或交易日/交易日内秒相对当前 bar 倒退丢弃并计入 `out_of_order`
     *    （同秒不倒退，同一 tsec 的多笔 tick 全部接受）；
     * 3. 非法价格（`last_price` 非正/NaN/Inf）跳过，不计数；
     * 4. 不在任何交易小节内丢弃并计入 `out_of_session`；
     * 5. 同一交易日内累计量或累计额倒退（含 NaN/Inf）丢弃并计入 `bad_volume`。
     *
     * 基准取自当前 bar：交易日变化必定换根（见下），故 `bar_.trading_day` 恒等于最近
     * 已接受 tick 的交易日，`bar_.last_tick_sec`/`bar_.cum_*` 即该 tick 的时间与累计。
     *
     * @param tick 行情快照（调用方保证属于本实例绑定的合约）
     */
    void on_tick(const DzTick& tick) {
        if (tick.time < 0 || tick.time > MAX_CLOCK_SEC) {
            ++stats_.out_of_session;
            return;
        }
        const int32_t tsec = TradingTime::from_clock(tick.time);
        if (tick.date <= 0) {
            ++stats_.out_of_order;
            return;
        }
        if (has_bar_ && (tick.date < bar_.trading_day ||
                         (tick.date == bar_.trading_day && tsec < bar_.last_tick_sec))) {
            ++stats_.out_of_order;
            return;
        }
        if (!std::isfinite(tick.last_price) || tick.last_price <= 0.0) {
            return;  // 非法价格只跳过，不计数、不推进任何基准
        }
        const SessionMatch match = match_session(tsec);
        if (!match.in_session) {
            ++stats_.out_of_session;
            return;
        }
        // 累计量额倒退只与当日基准比较；交易日变化必定换根，故基准按 bar 的交易日取。
        // NaN/Inf 成交额不参与比较，用 isfinite 显式拒掉（否则 Inf 会永久毒化后续差分）。
        const bool same_day_basis = has_bar_ && tick.date == bar_.trading_day;
        const int64_t basis_volume = same_day_basis ? bar_.cum_volume : 0;
        const double basis_turnover = same_day_basis ? bar_.cum_turnover : 0.0;
        if (static_cast<int64_t>(tick.volume) < basis_volume || !std::isfinite(tick.turnover) ||
            tick.turnover < basis_turnover) {
            ++stats_.bad_volume;
            return;
        }
        const int32_t begin_sec = (match.bucket_tsec / period_sec_) * period_sec_;
        // 交易日变化与换桶同样开新根：累计基准按日重置，避免跨日沿用旧日基准产出负量额。
        // （跨日的空交易日、夜盘组合等完整生命周期由 #88 定稿。）
        if (!has_bar_) {
            begin_bar(tick, begin_sec, tsec);
        } else if (begin_sec != bar_.begin_sec || tick.date != bar_.trading_day) {
            finalize_bar();
            begin_bar(tick, begin_sec, tsec);
        } else {
            update_bar(tick, tsec);
        }
        notify_update();
    }

    /// @brief 是否已有形成中 bar（首笔 tick 之前为 false）
    [[nodiscard]] bool has_current_bar() const noexcept { return has_bar_; }

    /// @brief 当前形成中 bar（以 has_current_bar() 为 true 为前提，否则是调用方编程错误）
    [[nodiscard]] const Bar& current_bar() const noexcept { return bar_; }

    /// @brief 累计丢弃计数（构造后从 0 累计）
    [[nodiscard]] BarBuilderStats stats() const noexcept { return stats_; }

private:
    /// @brief 一日内的最大墙钟秒数（23:59:59）；time 越界计入 out_of_session
    static constexpr DzTime MAX_CLOCK_SEC = 86399;

    /// @brief 交易小节匹配结果
    struct SessionMatch {
        bool in_session;      ///< true = tick 落在某个交易小节内
        int32_t bucket_tsec;  ///< 分桶用有效时间（小节末端为 tsec-1）
    };

    /**
     * @brief 判定交易日内秒是否落在某个交易小节内（端点含），并给出分桶用有效时间
     *
     * 小节末端时刻的 tick 归前一根 bar：分桶用有效时间取 tsec-1；last_tick_sec
     * 仍记录 tick 的真实 tsec。
     *
     * @param tsec 交易日内秒
     * @return 命中结果；未命中时 in_session=false
     */
    [[nodiscard]] SessionMatch match_session(int32_t tsec) const noexcept {
        for (int32_t i = 0; i < table_.session_count; ++i) {
            const TradingSession& session = table_.sessions[i];
            if (tsec < session.begin_sec) {
                return SessionMatch{false, tsec};  // 小节按时间升序，后续小节开始更晚
            }
            if (tsec <= session.end_sec) {
                return SessionMatch{true, (tsec == session.end_sec) ? tsec - 1 : tsec};
            }
        }
        return SessionMatch{false, tsec};
    }

    /// @brief 以 is_final=true 关闭当前 bar 并通知 sink（下一笔 tick 会开新 bar）
    void finalize_bar() {
        bar_.is_final = true;
        notify_close();
    }

    /// @brief 用本笔 tick 建立新的形成中 bar（交易日变化时累计基准重置为 0）
    void begin_bar(const DzTick& tick, int32_t begin_sec, int32_t tsec) {
        const bool same_day = has_bar_ && tick.date == bar_.trading_day;
        bar_start_cum_volume_ = same_day ? bar_.cum_volume : 0;
        bar_start_cum_turnover_ = same_day ? bar_.cum_turnover : 0.0;
        bar_ = Bar{};
        bar_.trading_day = tick.date;
        bar_.begin_sec = begin_sec;
        bar_.period_sec = period_sec_;
        bar_.open = tick.last_price;
        bar_.high = tick.last_price;
        bar_.low = tick.last_price;
        bar_.is_final = false;
        has_bar_ = true;
        update_bar(tick, tsec);
        notify_open();
    }

    /// @brief 用本笔 tick 更新形成中 bar 的 H/L/C、量额差、持仓量与最后 tick 时间
    void update_bar(const DzTick& tick, int32_t tsec) {
        if (tick.last_price > bar_.high) {
            bar_.high = tick.last_price;
        }
        if (tick.last_price < bar_.low) {
            bar_.low = tick.last_price;
        }
        bar_.close = tick.last_price;
        bar_.last_tick_sec = tsec;
        bar_.volume = static_cast<int64_t>(tick.volume) - bar_start_cum_volume_;
        bar_.turnover = tick.turnover - bar_start_cum_turnover_;
        bar_.cum_volume = static_cast<int64_t>(tick.volume);
        bar_.cum_turnover = tick.turnover;
        bar_.open_interest = tick.open_interest;
    }

    /// @brief 若 Sink 实现 on_bar_open 则通知新 bar 已打开（缺失视为空操作）
    void notify_open() {
        if constexpr (requires(Sink& sink, const Bar& bar) { sink.on_bar_open(bar); }) {
            sink_.on_bar_open(bar_);
        }
    }

    /// @brief 若 Sink 实现 on_bar_update 则通知当前 bar 已更新（缺失视为空操作）
    void notify_update() {
        if constexpr (requires(Sink& sink, const Bar& bar) { sink.on_bar_update(bar); }) {
            sink_.on_bar_update(bar_);
        }
    }

    /// @brief 若 Sink 实现 on_bar_close 则通知当前 bar 已收（缺失视为空操作）
    void notify_close() {
        if constexpr (requires(Sink& sink, const Bar& bar) { sink.on_bar_close(bar); }) {
            sink_.on_bar_close(bar_);
        }
    }

    TradingSessionTable table_;            ///< 交易小节与竞价窗口表（按值保存）
    int32_t period_sec_;                   ///< 周期秒数
    Sink& sink_;                           ///< bar 事件接收者（生命周期事件回调）
    Bar bar_{};                            ///< 当前形成中 bar
    BarBuilderStats stats_{};              ///< 丢弃计数（构造后从 0 累计）
    bool has_bar_ = false;                 ///< 是否已有形成中 bar
    int64_t bar_start_cum_volume_ = 0;     ///< 本 bar 起始累计成交量基准
    double bar_start_cum_turnover_ = 0.0;  ///< 本 bar 起始累计成交额基准
};

}  // namespace dztrader::bar

#endif  // DZTRADER_BAR_INTRADAY_BAR_BUILDER_H_
