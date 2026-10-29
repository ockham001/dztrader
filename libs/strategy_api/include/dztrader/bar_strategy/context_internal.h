/**
 * @file context_internal.h
 * @brief `InstrumentContext` 的内部构造缝 `context_internal::ContextAccess`（issue #153 的
 *        「接缝 C（纯单元）」）
 *
 * **内部头，不进公开承诺面**：只服务上下文与台账的单元测试、以及 #164 的两个纯 ctx 基准
 * （`bench_series_read` / `bench_relocate`）。策略作者不应包含本头——B 侧只能经
 * `InstrumentContext` 的公开面（价格视图、`make_series`、交易意图、Tier 1 查询）接触框架。公开头
 * `dztrader/bar_strategy/context.h` 只声明 `struct ContextAccess;` 并把它 列为
 * friend，本头才给出定义——与 `dztrader/series.h` 声明 `series_internal::Access`、
 * `dztrader/series_internal.h` 定义它的先例完全一致。
 *
 * 为什么需要这条缝：台账与 Tier 1 的口径（四函数全表、跨越 0 的开平归属、进出场时间、
 * 极值）是纯状态转移。用 Seam A（帧通道 + 真实回调）验证每一项代价过高，而且会让「哪一格
 * 算错」淹没在事件序断言里。这里给测试一个与 A 等价的入口：直接用 `InstrumentInit` 造 ctx，
 * 然后像 B 那样调四个意图方法并读 Tier 1——不需要引擎句柄、通道或配置文件。
 *
 * 生产代码**不用**本缝：`BarStrategyManager` 是 `InstrumentContext` 的 friend，在节点内
 * 直接构造 ctx 并调用 `advance_bar` / `update_current_bar` / `update_profit_extremes`。
 *
 * @note 本头的每个转发都对应一个 A 实际会调用的内部方法；只加「测试可能想用」但不被生产
 * 路径调用的转发会让测试断言到一条生产线不存在的路径（那是假覆盖）。`make_series` 转发是
 * 「A 持 `OnInitScope` 调 B.on_init、B 再调 ctx.make_series」这两次生产调用的组合，用于
 * 脱离 A 的纯 ctx 基准（#164）建出与真实策略相同的用户序列。
 *
 * 除单元测试外，#164 的 `bench_series_read` / `bench_relocate` 也用本缝构造脱离引擎的 ctx
 * （基准与单测共用同一条构造路径，不各写一份）。
 */
#ifndef DZTRADER_BAR_STRATEGY_CONTEXT_INTERNAL_H_
#define DZTRADER_BAR_STRATEGY_CONTEXT_INTERNAL_H_

#include <dztrader/bar_strategy/context.h>

#include <memory>

namespace dztrader::context_internal {

/// @brief `InstrumentContext` 的内部构造与驱动缝（测试与基准专用；生产路径由 A 直接调用内部方法）
struct ContextAccess {
    /// @brief 构造一个脱离引擎的 ctx（不订阅、不发单，只做虚拟台账）
    ///
    /// 0 个挂靠账户正是「先纸上跑通策略」的口径：ctx 内本来就没有账户表，因此这条路径
    /// 天然覆盖「只记逻辑账、不发单、不上报」。
    ///
    /// @param init 构造参数（品种代码 / 窗口容量 / 合约属性）
    /// @return 就地构造的 ctx（显式不可拷贝/移动，只能交出所有权）
    [[nodiscard]] static std::unique_ptr<InstrumentContext> make(const InstrumentInit& init) {
        // 构造函数是 private（只有 A 与本缝可调）：用普通 new 而不是 make_unique，
        // 后者在 std::make_unique 内部调用会失去 friend 身份。
        return std::unique_ptr<InstrumentContext>(new InstrumentContext(init));
    }

    /// @brief 标记实例失败（生产路径由 A 的回调异常边界调用；测试直接注入）
    static void mark_failed(InstrumentContext& ctx) noexcept { ctx.mark_failed(); }

    /// @brief 推进一根 bar（生产路径由 A 的 `dispatch_bar_open` 调用）
    ///
    /// 测试用它是为了验证**推进与台账的接线**——`bar_index_` 自增、进出场时间从 `bar::Bar`
    /// 取标准结束时间、价格写进窗口（`position_profit()` 读 `[0]`）。用真实 `advance_bar`
    /// 而不是在测试里手写这些字段，接线的正确性才有覆盖。
    static void advance(InstrumentContext& ctx, const bar::Bar& bar) noexcept {
        ctx.advance_bar(bar);
    }

    /// @brief 用形成中 bar 的最新字段刷新窗口 `[0]`（生产路径由 A 的 `dispatch_bar_update`）
    static void update_current(InstrumentContext& ctx, const bar::Bar& bar) noexcept {
        ctx.update_current_bar(bar);
    }

    /// @brief 用一根 bar 的收盘价更新浮盈浮亏极值（生产路径由 A 在 close 分发里调用）
    static void update_profit_extremes(InstrumentContext& ctx, double bar_close) noexcept {
        ctx.update_profit_extremes(bar_close);
    }

    /// @brief 回卷窗口（生产路径由 A 的 `relocate_live_instruments` 在 18:00 定时器里调用）
    ///
    /// 基准（`bench_relocate`，#164）用它量「18:00 回卷成本」——A 的定时器路径除了本调用还
    /// 有 map 遍历与一行诊断日志，那些不是回卷的搬移成本，故单独量本调用。
    static void relocate(InstrumentContext& ctx) noexcept { ctx.relocate(); }

    /**
     * @brief 在 `on_init` 作用域内注册一条用户序列（等价于 B 的 `on_init` 里调 `make_series`）
     *
     * 生产路径是 A 持 `OnInitScope` 调 B.on_init、B 再调 ctx.make_series；本转发把这两次
     * 生产调用按同一顺序组合起来，供脱离 A 的纯 ctx 基准（`bench_series_read` /
     * `bench_relocate`，#164）建出与真实策略相同的用户序列。
     *
     * @tparam T 元素类型（`double` / `std::int64_t`）
     * @param ctx  目标实例
     * @param init 静默填窗阶段逐根写入的初值
     * @return 可写序列视图
     */
    template <class T>
    [[nodiscard]] static Series<T> make_series(InstrumentContext& ctx, T init) {
        const InstrumentContext::OnInitScope on_init_scope(ctx);
        return ctx.make_series<T>(init);
    }
};

}  // namespace dztrader::context_internal

#endif  // DZTRADER_BAR_STRATEGY_CONTEXT_INTERNAL_H_
