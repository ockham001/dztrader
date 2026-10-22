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
 * 交易时段：tick 必须落在某个交易小节或 Open 竞价窗口内（端点含，多小节间休息与非
 * 交易时段均为丢弃并计入 stats().out_of_session），否则不改变任何 bar/累计状态、不触发
 * 回调。小节末端（tsec == session.end_sec）的 tick 归前一根 bar。
 *
 * 集合竞价：窗口命中优先于小节判定，窗口长度由 TradingSessionTable 描述（任意长度，
 * 一个交易日可有多个）。
 * - 命中 Open 窗口：tick 归目标小节首桶（floor(session.begin_sec / period_sec) * period_sec），
 *   若该桶 bar 只有竞价 tick，按旧口径逐 tick 覆盖 O=H=L=C（同桶多笔竞价 tick 逐个覆盖）；
 *   若为粗周期跨小节拼根、该桶已含场内 tick，则按普通 tick 静默并入，不覆盖已公开的
 *   开盘价与高低点。量额累计差分、OI 取最后一笔；窗口内不回调任何 sink 事件，静默收尾的
 *   bar 延迟到离开窗口后的第一笔有效 tick 处补发：从未暴露过的 bar 补发 open
 *   （is_final=false）+ close（is_final=true），使每根补发的 bar 恰好 open/close 各一次。
 *   补发依赖后续 tick：tick 流若在竞价窗口内结束，这些 bar 不补发（与惰性收盘一致）。
 * - 命中 Close 窗口：按普通小节内 tick 处理（含 tsec == session.end_sec 归前一根）。
 *
 * 周期可以是任意正整数秒：桶起点 = floor(tsec / period_sec) * period_sec（18:00 锚点
 * 整数网格），1 小时周期即自然整点对齐；小节休息落在桶内时，休息前后小节的 tick
 * 拼进同一根 bar，不按小节切根。
 *
 * 桶边界事件顺序：新 bar 首笔 tick 先 close 旧 bar（is_final=true），再 open 新 bar
 * （is_final=false），最后 update 新 bar；小节内同桶 tick 只 update。无成交的桶不生成
 * bar（相邻 bar 的 begin_sec 可跳跃）。
 *
 * @tparam Sink bar 事件接收者：可选实现 on_bar_open / on_bar_update / on_bar_close，
 *              未实现视为空操作；回调不得抛异常
 *
 * @note 交易日变化按「换根」处理：交易日变大的第一笔有效 tick 到来时，先以 `is_final=true`
 *       关闭当日最后一根 bar，再开新交易日首根（即便落在同一桶起点也换根），累计基准重置
 *       为 0，故新日首笔 tick 的量额 delta 从 0 起算、不跨日累加；被丢弃的新日 tick
 *       （非交易小节/非法价格/坏量）不切日。`trading_day` 变小或 `date` 非正的 tick 按
 *       乱序丢弃并计入 `out_of_order`；没有 tick 的交易日不生成空 bar（惰性收盘）。夜盘
 *       跨午夜不切日（`date` 未变即同一交易日）。续接（restore）由后续票定稿，公开方法
 *       只增不改。
 *
 * @note 竞价窗口内静默收尾的 bar 进入待补发队列（容量 = 单日竞价窗口数），离开窗口后的
 *       第一笔有效 tick 按入队顺序补发。只有「跨交易日且窗口之间没有任何场内有效 tick」
 *       的病态数据流会使队列溢出；溢出时丢弃最旧的待补发 bar，优先守住窗口内零回调
 *       （策略安全），正常行情下一个场内 tick 即清空队列。
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
     * @brief 喂入一笔 tick：不在任何交易小节或 Open 竞价窗口内的 tick 丢弃并计数；同桶更新
     *        当前 bar；跨桶或交易日变化先收旧 bar（close）再开新 bar（open）并更新；小节内
     *        每笔已接受 tick 恰好一次 update（Open 竞价窗口内的 tick 只静默更新 bar 状态）
     *
     * 坏数据守卫按下列固定顺序判定，被丢弃的 tick 不改变任何 bar/累计状态、不触发回调：
     * 1. `time` 越界（0..86399 之外）丢弃并计入 `out_of_session`；
     * 2. `date` 非正、或交易日/交易日内秒相对当前 bar 倒退丢弃并计入 `out_of_order`
     *    （同秒不倒退，同一 tsec 的多笔 tick 全部接受）；
     * 3. 非法价格（`last_price` 非正/NaN/Inf）跳过，不计数；
     * 4. 既不在任何 Open 竞价窗口内、也不在任何交易小节内丢弃并计入 `out_of_session`；
     * 5. 同一交易日内累计量或累计额倒退（含 NaN/Inf）丢弃并计入 `bad_volume`。
     *
     * Open 竞价窗口内的 tick 只更新当前 bar 状态、不回调（见类注释）；窗口内静默收尾的 bar
     * 在离开窗口后的第一笔有效 tick 处按序补发事件。
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
        // 窗口命中优先于 session 判定；Close 窗口返回 false 后按普通小节内 tick 处理。
        const AuctionMatch auction = match_auction(tsec);
        if (auction.in_open_window) {
            if (!cumulative_basis_ok(tick)) {
                ++stats_.bad_volume;
                return;
            }
            on_open_auction_tick(tick, tsec, auction.session_index);
            return;
        }
        const SessionMatch match = match_session(tsec);
        if (!match.in_session) {
            ++stats_.out_of_session;
            return;
        }
        if (!cumulative_basis_ok(tick)) {
            ++stats_.bad_volume;
            return;
        }
        // 离开竞价窗口后的第一笔有效 tick：补发窗口内静默收尾的 bar（先 open 后 close）
        flush_pending_bars();
        const int32_t begin_sec = (match.bucket_tsec / period_sec_) * period_sec_;
        // 交易日变化与换桶同样开新根：累计基准按日重置，避免跨日沿用旧日基准产出负量额。
        // 同日换桶沿用当日基准；跨日（含同桶起点）先 close 旧根再 open 新根（#88）。
        if (!has_bar_) {
            begin_bar(tick, begin_sec, tsec, BarOrigin::Session);
        } else if (begin_sec != bar_.begin_sec || tick.date != bar_.trading_day) {
            finalize_bar();
            begin_bar(tick, begin_sec, tsec, BarOrigin::Session);
        } else {
            expose_bar();  // 竞价建立的 bar 首次被场内 tick 暴露：先补 open 再 update
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

private:
    /// @brief 一日内的最大墙钟秒数（23:59:59）；time 越界计入 out_of_session
    static constexpr DzTime MAX_CLOCK_SEC = 86399;

    /// @brief 竞价窗口内静默收尾的待补发 bar 容量（以单日竞价窗口数为基准）
    static constexpr int32_t MAX_PENDING_BARS = TradingSessionTable::MAX_AUCTIONS;

    /// @brief 交易小节匹配结果
    struct SessionMatch {
        bool in_session;      ///< true = tick 落在某个交易小节内
        int32_t bucket_tsec;  ///< 分桶用有效时间（小节末端为 tsec-1）
    };

    /// @brief Open 竞价窗口匹配结果
    struct AuctionMatch {
        bool in_open_window;    ///< true = tick 落在某个 Open 竞价窗口内
        int32_t session_index;  ///< Open 窗口归属交易小节下标（Close 窗口无意义）
    };

    /// @brief 新 bar 的来源（决定 open 回调时机与 O=H=L=C 口径）
    enum class BarOrigin {
        Session,  ///< 小节内 tick：按普通口径建立并立即回调 open
        Auction,  ///< Open 竞价窗口 tick：静默建立，等暴露或收尾时补发
    };

    /// @brief 竞价窗口内静默收尾、等待补发的 bar
    struct PendingBar {
        Bar bar;          ///< 静默收尾的 bar 快照
        bool needs_open;  ///< true = 该 bar 从未向 sink 暴露，补发时先 open 再 close
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

    /**
     * @brief 判定交易日内秒是否落在某个 Open 竞价窗口内（端点含）
     *
     * 窗口按时间升序且互不重叠；命中 Close 窗口返回 false（按普通小节内 tick 处理）。
     *
     * @param tsec 交易日内秒
     * @return 命中结果；未命中 Open 窗口时 in_open_window=false
     */
    [[nodiscard]] AuctionMatch match_auction(int32_t tsec) const noexcept {
        for (int32_t i = 0; i < table_.auction_count; ++i) {
            const AuctionWindow& window = table_.auctions[i];
            if (tsec < window.begin_sec) {
                break;  // 窗口按时间升序，后续窗口开始更晚
            }
            if (tsec <= window.end_sec) {
                return AuctionMatch{window.kind == AuctionKind::Open, window.session_index};
            }
        }
        return AuctionMatch{false, 0};
    }

    /**
     * @brief 累计量额差分基准守卫：同一交易日内累计量/额不得倒退（成交额不得为 NaN/Inf）
     *
     * 基准取自当前 bar：交易日变化必定换根，故基准按 bar 的交易日取；跨交易日基准为 0，
     * 不视为坏数据。
     *
     * @param tick 行情快照（价格/时间守卫已通过）
     * @return true = 累计量额没有倒退，可接受
     */
    [[nodiscard]] bool cumulative_basis_ok(const DzTick& tick) const noexcept {
        const bool same_day_basis = has_bar_ && tick.date == bar_.trading_day;
        const int64_t basis_volume = same_day_basis ? bar_.cum_volume : 0;
        const double basis_turnover = same_day_basis ? bar_.cum_turnover : 0.0;
        // NaN/Inf 成交额不参与比较，用 isfinite 显式拒掉（否则 Inf 会永久毒化后续差分）。
        return static_cast<int64_t>(tick.volume) >= basis_volume && std::isfinite(tick.turnover) &&
               tick.turnover >= basis_turnover;
    }

    /**
     * @brief 处理 Open 竞价窗口内的 tick：归目标小节首桶并静默更新，不回调 sink
     *
     * 换桶/跨日时先静默收尾当前 bar（存入待补发队列，离开窗口后补发），再建立竞价 bar。
     *
     * @param tick 行情快照（价格/时间/量额守卫已在 on_tick 通过）
     * @param tsec 交易日内秒
     * @param session_index Open 窗口归属的交易小节下标
     */
    void on_open_auction_tick(const DzTick& tick, int32_t tsec, int32_t session_index) {
        const int32_t begin_sec =
            (table_.sessions[session_index].begin_sec / period_sec_) * period_sec_;
        if (!has_bar_) {
            begin_bar(tick, begin_sec, tsec, BarOrigin::Auction);
        } else if (begin_sec != bar_.begin_sec || tick.date != bar_.trading_day) {
            silently_finalize_bar();
            begin_bar(tick, begin_sec, tsec, BarOrigin::Auction);
        } else if (bar_open_notified_) {
            // 粗周期跨小节拼根：目标桶已含场内 tick（已暴露）。按普通 tick 静默并入，
            // 不覆盖已公开的开盘价/高低点，避免竞价 tick 抹掉同桶更早的场内数据。
            update_bar(tick, tsec);
        } else {
            update_auction_bar(tick, tsec);
        }
    }

    /// @brief 静默收尾当前 bar（竞价窗口内不回调），存入待补发队列
    ///
    /// 队列按单日竞价窗口数定容；只有「跨交易日且窗口之间没有任何场内有效 tick」的病态流
    /// 才会溢出。溢出时丢弃最旧的待补发 bar：优先守住「竞价窗口内零回调」（策略不可能在
    /// 竞价时段下单），正常行情下一个场内 tick 就会清空队列、不会走到这里。
    void silently_finalize_bar() {
        if (pending_count_ == MAX_PENDING_BARS) {
            for (int32_t i = 1; i < pending_count_; ++i) {
                pending_bars_[i - 1] = pending_bars_[i];
            }
            --pending_count_;
        }
        PendingBar& pending = pending_bars_[pending_count_];
        pending.bar = bar_;
        pending.needs_open = !bar_open_notified_;
        ++pending_count_;
    }

    /// @brief 补发待发事件：从未暴露的 bar 先 open（is_final=false）再 close（is_final=true）
    void flush_pending_bars() {
        for (int32_t i = 0; i < pending_count_; ++i) {
            Bar& pending_bar = pending_bars_[i].bar;
            if (pending_bars_[i].needs_open) {
                notify_open(pending_bar);
            }
            pending_bar.is_final = true;
            notify_close(pending_bar);
        }
        pending_count_ = 0;
    }

    /// @brief 当前 bar 若尚未向 sink 暴露则补发 open（竞价建立、首次被场内 tick 暴露时）
    void expose_bar() {
        if (bar_open_notified_) {
            return;
        }
        bar_open_notified_ = true;
        notify_open(bar_);
    }

    /// @brief 以 is_final=true 关闭当前 bar 并通知 sink（下一笔 tick 会开新 bar）
    ///
    /// 从未暴露过的 bar（仅由竞价 tick 建立）在此补发 open（is_final=false）再 close，
    /// 保证每根 bar 恰好 open 一次、close 一次。
    void finalize_bar() {
        expose_bar();
        bar_.is_final = true;
        notify_close(bar_);
    }

    /**
     * @brief 用本笔 tick 建立新的形成中 bar（交易日变化时累计基准重置为 0）
     *
     * @param tick 行情快照
     * @param begin_sec 标准开始时间（交易日内秒）
     * @param tsec 本笔 tick 的交易日内秒
     * @param origin 新 bar 来源：Auction 不回调 open，等首次暴露或收尾时补发
     */
    void begin_bar(const DzTick& tick, int32_t begin_sec, int32_t tsec, BarOrigin origin) {
        const bool same_day = has_bar_ && tick.date == bar_.trading_day;
        bar_start_cum_volume_ = same_day ? bar_.cum_volume : 0;
        bar_start_cum_turnover_ = same_day ? bar_.cum_turnover : 0.0;
        bar_ = Bar{};
        bar_.trading_day = tick.date;
        bar_.begin_sec = begin_sec;
        bar_.period_sec = period_sec_;
        bar_.is_final = false;
        has_bar_ = true;
        if (origin == BarOrigin::Auction) {
            update_auction_bar(tick, tsec);
            bar_open_notified_ = false;
            return;
        }
        bar_.open = tick.last_price;
        bar_.high = tick.last_price;
        bar_.low = tick.last_price;
        update_bar(tick, tsec);
        bar_open_notified_ = true;
        notify_open(bar_);
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
        update_bar_market_data(tick, tsec);
    }

    /// @brief 竞价 tick 按旧口径逐笔覆盖 O=H=L=C，量额/OI/最后 tick 时间同普通 tick
    void update_auction_bar(const DzTick& tick, int32_t tsec) {
        bar_.open = tick.last_price;
        bar_.high = tick.last_price;
        bar_.low = tick.last_price;
        bar_.close = tick.last_price;
        update_bar_market_data(tick, tsec);
    }

    /// @brief 更新量额差分、累计字段、持仓量与最后 tick 时间（普通/竞价 tick 共用）
    void update_bar_market_data(const DzTick& tick, int32_t tsec) {
        bar_.last_tick_sec = tsec;
        bar_.volume = static_cast<int64_t>(tick.volume) - bar_start_cum_volume_;
        bar_.turnover = tick.turnover - bar_start_cum_turnover_;
        bar_.cum_volume = static_cast<int64_t>(tick.volume);
        bar_.cum_turnover = tick.turnover;
        bar_.open_interest = tick.open_interest;
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

    TradingSessionTable table_;                    ///< 交易小节与竞价窗口表（按值保存）
    int32_t period_sec_;                           ///< 周期秒数
    Sink& sink_;                                   ///< bar 事件接收者（生命周期事件回调）
    Bar bar_{};                                    ///< 当前形成中 bar
    BarBuilderStats stats_{};                      ///< 丢弃计数（构造后从 0 累计）
    bool has_bar_ = false;                         ///< 是否已有形成中 bar
    bool bar_open_notified_ = false;               ///< 当前 bar 的 open 事件是否已发给 sink
    int64_t bar_start_cum_volume_ = 0;             ///< 本 bar 起始累计成交量基准
    double bar_start_cum_turnover_ = 0.0;          ///< 本 bar 起始累计成交额基准
    PendingBar pending_bars_[MAX_PENDING_BARS]{};  ///< 竞价窗口内静默收尾、待补发的 bar
    int32_t pending_count_ = 0;                    ///< 待补发 bar 数
};

}  // namespace dztrader::bar

#endif  // DZTRADER_BAR_INTRADAY_BAR_BUILDER_H_
