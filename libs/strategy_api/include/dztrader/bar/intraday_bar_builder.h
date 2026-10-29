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
 * 集合竞价（ADR 0032）：窗口命中优先于小节判定，窗口长度由 TradingSessionTable 描述
 * （任意长度，一个交易日可有多个）。
 * - 命中 Open 窗口：tick 归目标小节首桶（floor(session.begin_sec / period_sec) * period_sec），
 *   并按正常事件模型回调：窗口首笔先 close 上一根（实例首根例外）、再 open 新首根、再
 *   update；窗口内后续每笔只 update；离开窗口后的第一笔连续交易 tick 只 update（同一根
 *   bar 继续更新、不再 open）。若目标桶 bar 只由竞价 tick 建立，按旧口径逐笔覆盖
 *   O=H=L=C；若为粗周期跨小节拼根、该桶已含场内 tick，则按普通 tick 并入，不覆盖已公开
 *   的开盘价与高低点。量额累计差分、OI 取最后一笔。
 * - 命中 Close 窗口：按普通小节内 tick 处理（含 tsec == session.end_sec 归前一根），
 *   事件 is_auction 为 false。
 *
 * 周期可以是任意正整数秒：桶起点 = floor(tsec / period_sec) * period_sec（18:00 锚点
 * 整数网格），1 小时周期即自然整点对齐；小节休息落在桶内时，休息前后小节的 tick
 * 拼进同一根 bar，不按小节切根。
 *
 * 桶边界事件顺序：新 bar 首笔 tick 先 close 旧 bar（is_final=true），再 open 新 bar
 * （is_final=false），最后 update 新 bar；小节内同桶 tick 只 update。无成交的桶不生成
 * bar（相邻 bar 的 begin_sec 可跳跃）。Open 竞价窗口内同构：窗口首笔同样先 close 上一根
 * → open 新的竞价首根 → update。
 *
 * 回调参数：三个 bar 回调的签名均为 `(const Bar&, bool is_auction)`（见 types.h 的
 * Sink 契约）。`is_auction` 表示**触发本次回调的 tick 是否处于 Open 竞价窗口**，是回调
 * 时刻的环境相位、不是被交出 bar 的属性；仅 Open 窗口标记，Close 窗口按普通 tick 处理、
 * 不标记。窗口首笔触发的 close 关闭的是上一根 bar，但 is_auction 取本笔竞价 tick 的相位
 * （true）。组件只按 tick 判定：回放合成 tick 若落在 Open 窗口内同样标记 true，是否把
 * 回放期相位交给最终用户由调用方决定（BarStrategy 框架在回放期抑制，见
 * `bar_strategy/manager.h` 的 `InstrumentNode::replaying`）。
 *
 * @tparam Sink bar 事件接收者：可选实现 on_bar_open / on_bar_update / on_bar_close，
 *              签名均为 `void(const Bar&, bool is_auction)`，未实现视为空操作；
 *              回调不得抛异常
 *
 * @note 交易日变化按「换根」处理：交易日变大的第一笔有效 tick 到来时，先以 `is_final=true`
 *       关闭当日最后一根 bar，再开新交易日首根（即便落在同一桶起点也换根），累计基准重置
 *       为 0，故新日首笔 tick 的量额 delta 从 0 起算、不跨日累加；被丢弃的新日 tick
 *       （非交易小节/非法价格/坏量）不切日。`trading_day` 变小或 `date` 非正的 tick 按
 *       乱序丢弃并计入 `out_of_order`；没有 tick 的交易日不生成空 bar（惰性收盘）。夜盘
 *       跨午夜不切日（`date` 未变即同一交易日）。
 *
 * @note 续接与重置：restore() 只允许在构造/reset() 之后、尚未调用 on_tick() 时调用（同一
 *       reset 周期内只能调用一次，违反抛 std::invalid_argument）；周期不匹配或 bar 不属于
 *       本实例小节表同样 fail-fast。restore final 只恢复交易日、桶起点与累计量额基准，不
 *       建立形成中 bar、不重复 close；下一次同交易日更晚桶（或新交易日）的 tick 才开新
 *       bar，其首笔量额 delta 基于恢复的累计基准。restore forming 采纳为当前形成中 bar
 *       （视为已 open、不再补发 open），同桶同秒或更晚 tick 只 update，跨桶/跨日先 close
 *       （is_final=true）再 open；若归档 bar 只由竞价 tick 建立，重启后同窗口竞价 tick 仍
 *       按竞价口径覆盖 O=H=L=C。reset() 清空当前 bar、交易日/累计基准与统计，保留配置与
 *       sink 引用，用于回测重跑。
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
     *        当前 bar；跨桶或交易日变化先收旧 bar（close）再开新 bar（open）并更新；每笔
     *        已接受 tick 恰好一次 update（Open 竞价窗口内同样发事件，仅 is_auction=true）
     *
     * 坏数据守卫按下列固定顺序判定，被丢弃的 tick 不改变任何 bar/累计状态、不触发回调：
     * 1. `time` 越界（0..86399 之外）丢弃并计入 `out_of_session`；
     * 2. `date` 非正、或交易日/交易日内秒相对当前 bar 倒退（含 restore final 后交易日
     *    倒退）丢弃并计入 `out_of_order`（同秒不倒退，同一 tsec 的多笔 tick 全部接受）；
     * 3. 非法价格（`last_price` 非正/NaN/Inf）跳过，不计数；
     * 4. 既不在任何 Open 竞价窗口内、也不在任何交易小节内丢弃并计入 `out_of_session`；
     * 5. restore final 后同交易日目标桶起点不大于恢复 bar 的 tick 丢弃并计入
     *    `out_of_order`（避免已收桶重新生成 bar；只丢弃、不消费恢复基准）；
     * 6. 同一交易日内累计量或累计额倒退（含 NaN/Inf）丢弃并计入 `bad_volume`。
     *
     * 基准取自最近一笔已接受 tick 的累计量额：当前 bar，或 restore final 恢复的基准（同交易日
     * 时）。交易日变化必定换根（见下），故有当前 bar 时 `bar_.trading_day` 恒等于最近已接受
     * tick 的交易日，`bar_.last_tick_sec`/`bar_.cum_*` 即该 tick 的时间与累计。
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
        const TickBucket bucket = resolve_bucket(tsec);
        if (bucket.open_auction) {
            if (!restored_final_accepts(tick, bucket.begin_sec)) {
                ++stats_.out_of_order;
                return;
            }
            if (!cumulative_basis_ok(tick)) {
                ++stats_.bad_volume;
                return;
            }
            on_open_auction_tick(tick, tsec, bucket.begin_sec);
            return;
        }
        if (!bucket.in_session) {
            ++stats_.out_of_session;
            return;
        }
        const int32_t begin_sec = bucket.begin_sec;
        if (!restored_final_accepts(tick, begin_sec)) {
            ++stats_.out_of_order;
            return;
        }
        if (!cumulative_basis_ok(tick)) {
            ++stats_.bad_volume;
            return;
        }
        // 交易日变化与换桶同样开新根：累计基准按日重置，避免跨日沿用旧日基准产出负量额。
        // 同日换桶沿用当日基准；跨日（含同桶起点）先 close 旧根再 open 新根（#88）。
        if (advance_bar(tick, begin_sec, tsec, false)) {
            update_bar(tick, tsec);
            bar_auction_only_ = false;  // 场内 tick 已并入：同桶后续竞价 tick 按普通 tick 合并
        }
        notify_update(bar_, false);
    }

    /// @brief 是否已有形成中 bar（首笔 tick 之前为 false）
    [[nodiscard]] bool has_current_bar() const noexcept { return has_bar_; }

    /// @brief 当前形成中 bar（以 has_current_bar() 为 true 为前提，否则是调用方编程错误）
    [[nodiscard]] const Bar& current_bar() const noexcept { return bar_; }

    /// @brief 累计丢弃计数（构造后从 0 累计）
    [[nodiscard]] BarBuilderStats stats() const noexcept { return stats_; }

    /**
     * @brief 用归档的最后一根 bar 续接（final 或 forming），恢复交易日与累计量额基准
     *
     * 只允许在构造/reset() 之后、尚未调用 on_tick() 时调用，同一 reset 周期内只能调用一次；
     * 违反前置条件、周期不匹配，或 bar 的桶起点不落在本实例小节表内时抛
     * std::invalid_argument（热路径不抛异常）。
     *
     * - final bar：只恢复交易日、桶起点与累计量额基准，不建立形成中 bar、不补发 close；
     *   下一次同交易日更晚桶（或新交易日）的 tick 才开新 bar，其首笔量额 delta 基于恢复的
     *   cum_volume/cum_turnover。同交易日目标桶起点不大于恢复 bar 的 tick 丢弃并计入
     *   out_of_order。
     * - forming bar：采纳为当前形成中 bar，视为已 open、不再补发 open；同桶且不早于
     *   last_tick_sec 的 tick 只 update，跨桶/跨日先 close（is_final=true）再 open。bar 内
     *   量额与累计量额反推出的起始基准用于后续同桶 tick 的差分。若该 bar 只由竞价 tick
     *   建立（O=H=L=C 且最后一笔在目标桶 Open 窗口内），重启后同窗口竞价 tick 继续按竞价
     *   口径覆盖 O=H=L=C，但依然不补发 open。
     *
     * @param bar 归档的最后一根 bar（period_sec 必须与本实例周期一致）
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
        adopt_forming_bar(bar);
    }

    /// @brief 清空当前 bar、交易日/累计基准与统计，保留配置与 sink 引用
    ///
    /// 用于回测重跑：reset 后可重新 restore()，或从全新 tick 流开始完整序列。
    void reset() noexcept {
        bar_ = Bar{};
        has_bar_ = false;
        bar_auction_only_ = false;
        bar_start_cum_volume_ = 0;
        bar_start_cum_turnover_ = 0.0;
        restored_final_ = false;
        restored_final_bar_ = Bar{};
        stats_ = BarBuilderStats{};
        tick_received_ = false;
    }

private:
    /// @brief 一日内的最大墙钟秒数（23:59:59）；time 越界计入 out_of_session
    static constexpr DzTime MAX_CLOCK_SEC = MAX_TRADING_SEC;

    /// @brief 交易日内秒上限（23:59:59）；restore 校验用，与墙钟秒同域
    static constexpr int32_t MAX_TSEC = MAX_TRADING_SEC;

    /**
     * @brief 判定交易日内秒是否落在某个交易小节内（端点含），并给出分桶用有效时间
     *
     * 实际判定在 types.h 的 {@link resolve_tick_bucket}（builder 与框架续接过滤的唯一实现），
     * 这里只是把「小节末端归前一根」的语义保留在调用点，避免桶算术散落。
     *
     * @param tsec 交易日内秒
     * @return 命中时 `in_session == true` 且 `begin_sec` 为目标桶起点
     */
    [[nodiscard]] TickBucket resolve_bucket(int32_t tsec) const noexcept {
        return resolve_tick_bucket(tsec, table_, period_sec_);
    }

    /**
     * @brief restore final 后的续接顺序守卫：同交易日只接受桶起点更晚的 tick
     *
     * restore final 不建立形成中 bar，但已收桶不得重新生成 bar：同交易日目标桶起点不大于
     * 恢复 bar 的 tick 在时段判定之后丢弃（调用方计入 out_of_order）。新交易日的 tick 不受
     * 限制（累计基准按日重置）。
     *
     * @param tick 行情快照（时间/日期/价格守卫已通过）
     * @param begin_sec 本笔 tick 的目标桶起点（交易日内秒）
     * @return true = 可以开新 bar；false = 应丢弃并计 out_of_order
     */
    [[nodiscard]] bool restored_final_accepts(const DzTick& tick,
                                              int32_t begin_sec) const noexcept {
        return !restored_final_ || tick.date > restored_final_bar_.trading_day ||
               begin_sec > restored_final_bar_.begin_sec;
    }

    /**
     * @brief 累计量额差分基准守卫：同一交易日内累计量/额不得倒退（成交额不得为 NaN/Inf）
     *
     * 基准取自最近一笔已接受 tick 的累计量额：当前 bar 或 restore final 恢复的基准；交易日
     * 变化必定换根，故基准按基准交易日取；跨交易日基准为 0，不视为坏数据。
     *
     * @param tick 行情快照（价格/时间守卫已通过）
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
     * @brief 校验待续接 bar：周期、交易日/时间域、桶起点与本节表一致
     * @param bar 待续接 bar
     * @throws std::invalid_argument 任一前置条件不满足
     */
    void validate_restore_bar(const Bar& bar) const {
        if (bar.period_sec != period_sec_) {
            throw std::invalid_argument(
                "restore period mismatch | builder_period_sec=" + std::to_string(period_sec_) +
                " bar_period_sec=" + std::to_string(bar.period_sec));
        }
        if (bar.trading_day <= 0) {
            throw std::invalid_argument("restore invalid trading_day | trading_day=" +
                                        std::to_string(bar.trading_day));
        }
        if (bar.begin_sec < 0 || bar.begin_sec > MAX_TSEC || bar.begin_sec % period_sec_ != 0) {
            throw std::invalid_argument(
                "restore invalid begin_sec | begin_sec=" + std::to_string(bar.begin_sec) +
                " period_sec=" + std::to_string(period_sec_));
        }
        if (bar.last_tick_sec < 0 || bar.last_tick_sec > MAX_TSEC) {
            throw std::invalid_argument("restore invalid last_tick_sec | last_tick_sec=" +
                                        std::to_string(bar.last_tick_sec));
        }
        if (!bucket_overlaps_session(bar.begin_sec)) {
            throw std::invalid_argument("restore begin_sec outside session table | begin_sec=" +
                                        std::to_string(bar.begin_sec) +
                                        " period_sec=" + std::to_string(period_sec_));
        }
        if (!bar.is_final && !forming_bar_basis_ok(bar)) {
            throw std::invalid_argument(
                "restore forming bar inconsistent | volume=" + std::to_string(bar.volume) +
                " cum_volume=" + std::to_string(bar.cum_volume) + " turnover=" +
                std::to_string(bar.turnover) + " cum_turnover=" + std::to_string(bar.cum_turnover));
        }
        if (bar.is_final && !final_bar_basis_ok(bar)) {
            throw std::invalid_argument(
                "restore final bar inconsistent | cum_volume=" + std::to_string(bar.cum_volume) +
                " cum_turnover=" + std::to_string(bar.cum_turnover));
        }
    }

    /// @brief 桶区间 [begin_sec, begin_sec + period_sec) 是否与任一小节相交
    [[nodiscard]] bool bucket_overlaps_session(int32_t begin_sec) const noexcept {
        const int64_t bucket_end = static_cast<int64_t>(begin_sec) + period_sec_;
        for (int32_t i = 0; i < table_.session_count; ++i) {
            const TradingSession& session = table_.sessions[i];
            if (begin_sec <= session.end_sec && bucket_end > session.begin_sec) {
                return true;
            }
        }
        return false;
    }

    /// @brief forming bar 能否反推出非负起始基准（bar 内量额不超过当日累计量额）
    [[nodiscard]] static bool forming_bar_basis_ok(const Bar& bar) noexcept {
        return std::isfinite(bar.turnover) && bar.turnover >= 0.0 &&
               std::isfinite(bar.cum_turnover) && bar.cum_turnover >= bar.turnover &&
               bar.volume >= 0 && bar.cum_volume >= bar.volume;
    }

    /// @brief final bar 的累计量额能否作为续接基准（累计量非负、累计额有限非负）
    [[nodiscard]] static bool final_bar_basis_ok(const Bar& bar) noexcept {
        return std::isfinite(bar.cum_turnover) && bar.cum_turnover >= 0.0 && bar.cum_volume >= 0;
    }

    /**
     * @brief 采纳 restored forming bar 为当前形成中 bar（视为已 open，不补发 open 事件）
     *
     * 若该 bar 只由竞价 tick 建立（最后一笔落在其目标桶的 Open 窗口内且 O=H=L=C），
     * 重启后同窗口竞价 tick 继续按竞价口径覆盖 O=H=L=C（与重启前的连续语义一致），
     * 但始终不补发 open；首次场内 tick 之后 bar 不再标记为竞价-only，后续竞价 tick
     * （粗周期拼根的病态重叠流）按普通 tick 并入。
     *
     * @param bar 待续接的形成中 bar（校验已通过）
     */
    void adopt_forming_bar(const Bar& bar) {
        bar_ = bar;
        bar_.is_final = false;
        has_bar_ = true;
        bar_start_cum_volume_ = bar_.cum_volume - bar_.volume;
        bar_start_cum_turnover_ = bar_.cum_turnover - bar_.turnover;
        bar_auction_only_ = is_auction_only_bar(bar_);
    }

    /**
     * @brief 归档的 forming bar 是否只由 Open 竞价 tick 建立（restore 采纳时判定一次）
     *
     * 判据：最后一笔落在其目标桶的 Open 窗口内，且 O=H=L=C。实时路径不能复用该判据
     * （桶内已并入的场内 tick 可能恰好与竞价价相同、OHLC 仍然全等），故实时路径用
     * `bar_auction_only_` 显式跟踪"是否含场内 tick"。归档 Bar 不带来源标记，该判据对
     * "桶内含场内 tick 但 OHLC 恰好全等"的归档 bar 会误判为竞价-only（Bar 字段冻结、
     * 无法从归档区分），属已知限制。
     */
    [[nodiscard]] bool is_auction_only_bar(const Bar& bar) const noexcept {
        const TickBucket bucket = resolve_bucket(bar.last_tick_sec);
        return bucket.open_auction && bucket.begin_sec == bar.begin_sec && bar.open == bar.high &&
               bar.high == bar.low && bar.low == bar.close;
    }

    /**
     * @brief 处理 Open 竞价窗口内的 tick：归目标小节首桶并按正常事件模型发事件
     *
     * 换桶/跨日时先 close 当前 bar（is_auction=true），再建立竞价 bar（open+update）；
     * 同桶后续竞价 tick 只 update。目标桶已含场内 tick（粗周期跨小节拼根）时按普通 tick
     * 并入，不覆盖已公开的 O/H/L。
     *
     * @param tick 行情快照（价格/时间/量额守卫已在 on_tick 通过）
     * @param tsec 交易日内秒
     * @param begin_sec 目标桶起点（Open 窗口归属小节的首桶，交易日内秒）
     */
    void on_open_auction_tick(const DzTick& tick, int32_t tsec, int32_t begin_sec) {
        if (advance_bar(tick, begin_sec, tsec, true)) {
            if (bar_auction_only_) {
                // 目标桶 bar 只由竞价 tick 建立：按旧口径逐笔覆盖 O=H=L=C
                update_auction_bar(tick, tsec);
            } else {
                // 粗周期跨小节拼根：目标桶已含场内 tick（已公开）。按普通 tick 并入，
                // 不覆盖已公开的开盘价/高低点，避免竞价 tick 抹掉同桶更早的场内数据。
                update_bar(tick, tsec);
            }
        }
        notify_update(bar_, true);
    }

    /**
     * @brief 确保当前 bar 与本笔 tick 同桶：无当前 bar 时 open 新根，换桶/换日时先 close 再 open
     *
     * 同桶时由调用方继续更新（普通 tick 更新 H/L/C，竞价 tick 按 `bar_auction_only_`
     * 决定覆盖还是合并）。
     *
     * @param tick 行情快照（价格/时间/量额守卫已通过）
     * @param begin_sec 本笔 tick 的目标桶起点（交易日内秒）
     * @param tsec 交易日内秒
     * @param is_auction 本笔 tick 是否处于 Open 竞价窗口（open/close 事件相位与新根口径）
     * @return true = 同桶，调用方需更新当前 bar；false = 已建立新根（无当前 bar 时只 open，
     *         换桶/换日时先 close 再 open），本笔 tick 已并入新根，调用方无需再更新
     */
    bool advance_bar(const DzTick& tick, int32_t begin_sec, int32_t tsec, bool is_auction) {
        if (!has_bar_) {
            begin_bar(tick, begin_sec, tsec, is_auction);
            return false;
        }
        if (begin_sec != bar_.begin_sec || tick.date != bar_.trading_day) {
            finalize_bar(is_auction);
            begin_bar(tick, begin_sec, tsec, is_auction);
            return false;
        }
        return true;
    }

    /// @brief 以 is_final=true 关闭当前 bar 并通知 sink（下一笔 tick 会开新 bar）
    /// @param is_auction 触发收尾的 tick 是否处于 Open 竞价窗口（事件相位）
    void finalize_bar(bool is_auction) {
        bar_.is_final = true;
        notify_close(bar_, is_auction);
    }

    /**
     * @brief 用本笔 tick 建立新的形成中 bar（交易日变化时累计基准重置为 0）
     *
     * 量额起始基准取最近一笔已接受 tick 的累计量额：当前 bar，或 restore final 恢复的累计
     * 基准（同一交易日时）。restore final 的基准只在此处消费一次，被丢弃的 tick 不消费。
     *
     * @param tick 行情快照
     * @param begin_sec 标准开始时间（交易日内秒）
     * @param tsec 本笔 tick 的交易日内秒
     * @param is_auction 本笔 tick 是否处于 Open 竞价窗口：决定 open 事件相位与竞价
     *                   O=H=L=C 覆盖口径
     */
    void begin_bar(const DzTick& tick, int32_t begin_sec, int32_t tsec, bool is_auction) {
        const bool same_day = has_day_basis() && tick.date == day_basis_trading_day();
        bar_start_cum_volume_ = same_day ? day_basis_cum_volume() : 0;
        bar_start_cum_turnover_ = same_day ? day_basis_cum_turnover() : 0.0;
        bar_ = Bar{};
        bar_.trading_day = tick.date;
        bar_.begin_sec = begin_sec;
        bar_.period_sec = period_sec_;
        bar_.is_final = false;
        has_bar_ = true;
        restored_final_ = false;         // 恢复基准已在本根建立时消费
        bar_auction_only_ = is_auction;  // 只由竞价 tick 建立：同桶后续竞价 tick 覆盖 O=H=L=C
        if (is_auction) {
            update_auction_bar(tick, tsec);
        } else {
            bar_.open = tick.last_price;
            bar_.high = tick.last_price;
            bar_.low = tick.last_price;
            update_bar(tick, tsec);
        }
        notify_open(bar_, is_auction);
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
    void notify_open(const Bar& bar, bool is_auction) {
        if constexpr (requires(Sink& sink, const Bar& event_bar, bool event_is_auction) {
                          sink.on_bar_open(event_bar, event_is_auction);
                      }) {
            sink_.on_bar_open(bar, is_auction);
        }
    }

    /// @brief 若 Sink 实现 on_bar_update 则通知当前 bar 已更新（缺失视为空操作）
    void notify_update(const Bar& bar, bool is_auction) {
        if constexpr (requires(Sink& sink, const Bar& event_bar, bool event_is_auction) {
                          sink.on_bar_update(event_bar, event_is_auction);
                      }) {
            sink_.on_bar_update(bar, is_auction);
        }
    }

    /// @brief 若 Sink 实现 on_bar_close 则通知当前 bar 已收（缺失视为空操作）
    void notify_close(const Bar& bar, bool is_auction) {
        if constexpr (requires(Sink& sink, const Bar& event_bar, bool event_is_auction) {
                          sink.on_bar_close(event_bar, event_is_auction);
                      }) {
            sink_.on_bar_close(bar, is_auction);
        }
    }

    TradingSessionTable table_;         ///< 交易小节与竞价窗口表（按值保存）
    int32_t period_sec_;                ///< 周期秒数
    Sink& sink_;                        ///< bar 事件接收者（生命周期事件回调）
    Bar bar_{};                         ///< 当前形成中 bar
    BarBuilderStats stats_{};           ///< 丢弃计数（构造后从 0 累计）
    bool has_bar_ = false;              ///< 是否已有形成中 bar
    bool bar_auction_only_ = false;     ///< 当前 bar 是否只由 Open 竞价 tick 建立（覆盖 O=H=L=C）
    int64_t bar_start_cum_volume_ = 0;  ///< 本 bar 起始累计成交量基准
    double bar_start_cum_turnover_ = 0.0;  ///< 本 bar 起始累计成交额基准
    bool tick_received_ = false;           ///< on_tick 是否已调用过（restore 前置状态）
    bool restored_final_ = false;          ///< 已 restore final，等待更晚 tick 开新 bar
    Bar restored_final_bar_{};             ///< restore final 的 bar（交易日/桶/累计基准）
};

}  // namespace dztrader::bar

#endif  // DZTRADER_BAR_INTRADAY_BAR_BUILDER_H_
