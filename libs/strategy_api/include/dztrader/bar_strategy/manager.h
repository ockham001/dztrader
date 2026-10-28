/**
 * @file manager.h
 * @brief 框架管理器 A：配置、订阅、tick 分发、bar 聚合与 B 回调（ADR 0027，组件文档 §2/§5/§8）
 *
 * `BarStrategyManager<Strategy>` 是引擎侧**唯一**的策略对象：它实现引擎要求的
 * `on_start(DzContext*)` / `on_tick(const DzTick&)` / `on_stop()` 与自主退出探测
 * （`should_exit()` / `exit_code()`），内部把每笔 tick 交给该品种的
 * `bar::IntradayBarBuilder`，再把 builder 的三类事件翻译成「推进窗口 + 调 B 回调」。
 * B 不接触引擎回调，也不持有 A 指针。
 *
 * 实例 = 配置里的每个品种：`instruments_` 是 `std::map`（有序、节点地址稳定），每个节点
 * 就地装配 `InstrumentContext` + 事件适配器 + builder + B 实例，构造后不再搬移——其它品种
 * 增删不影响本节点的地址，B 持有的价格视图因此始终有效。
 *
 * 异常隔离（组件文档 §4.6）：每次 B 回调外围都有 `try/catch`，异常绝不穿越引擎/C API
 * 边界；捕获后把**该实例**标记失败（不再回调、后续 tick 直接丢弃），进程与其它品种继续。
 * `on_init` 抛异常 = 该品种创建失败（不加入 map），继续创建其它品种。
 *
 * 已落地（#157 + #158）：配置加载与启动诊断、订阅、tick→bar、回调分发、异常隔离、退化交易
 * 小节表，以及**回调门禁 + 静默填窗 + 用户序列 + 18:00 回卷**（§4.4/§5.2）。交易意图与逻辑
 * 台账（#159）、挂靠缩放与上报（#160）、历史预热回放（#161）、动态管理与参数（#162）、竞价
 * 相位（#163）后续接入，接缝见各成员注释。
 */
#ifndef DZTRADER_BAR_STRATEGY_MANAGER_H_
#define DZTRADER_BAR_STRATEGY_MANAGER_H_

#include <dztrader/api.h>
#include <dztrader/bar/intraday_bar_builder.h>
#include <dztrader/bar/types.h>
#include <dztrader/bar_strategy/config.h>
#include <dztrader/bar_strategy/config_file.h>
#include <dztrader/bar_strategy/context.h>
#include <dztrader/struct.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dztrader {

template <class Strategy>
class BarStrategyManager;  // 前向：事件适配器反向调用 A

template <class Strategy>
struct BarEventSink;  // 前向：builder 事件适配器（friend）

template <class Strategy>
struct InstrumentNode;  // 前向：节点就地构造 ctx（friend）

// ============================================================
// B 回调的编译期探测（鸭子类型，与 run_strategy 同风格）
// ============================================================

namespace manager_internal {

/// @brief B 是否实现 `on_init(InstrumentContext&)`
template <typename T>
concept HasOnInit = requires(T instance, InstrumentContext& ctx) { instance.on_init(ctx); };

/// @brief B 是否实现 `on_destroy(InstrumentContext&)`
template <typename T>
concept HasOnDestroy = requires(T instance, InstrumentContext& ctx) { instance.on_destroy(ctx); };

/// @brief B 是否实现 `on_bar_open(InstrumentContext&)`
template <typename T>
concept HasOnBarOpen = requires(T instance, InstrumentContext& ctx) { instance.on_bar_open(ctx); };

/// @brief B 是否实现 `on_bar_update(InstrumentContext&)`
template <typename T>
concept HasOnBarUpdate =
    requires(T instance, InstrumentContext& ctx) { instance.on_bar_update(ctx); };

/// @brief B 是否实现 `on_bar_close(InstrumentContext&)`
template <typename T>
concept HasOnBarClose =
    requires(T instance, InstrumentContext& ctx) { instance.on_bar_close(ctx); };

/**
 * @brief 从 tick 的定长 `instrument_id` 取出合约代码视图（不分配、不复制）
 *
 * 热路径上唯一的「字符串」操作：长度上限是 `sizeof(DzInstrumentId)`，未以 NUL 结尾时按
 * 字段宽度截断（SDK 分发层已保证写入端 NUL 结尾，这里只做防御）。
 *
 * @param tick 行情快照
 * @return 合约代码视图（指向 tick 自身存储，仅在本次回调内有效）
 */
[[nodiscard]] inline std::string_view instrument_id_of(const DzTick& tick) noexcept {
    std::size_t length = 0;
    while (length < sizeof(tick.instrument_id) && tick.instrument_id[length] != '\0') {
        ++length;
    }
    return std::string_view(tick.instrument_id, length);
}

}  // namespace manager_internal

/**
 * @brief 每日回卷的默认墙钟时刻（18:00，距午夜毫秒；组件文档 §4.4）
 *
 * 18:00 位于日盘收盘之后、夜盘开盘之前，是天然静默窗。生产入口恒用本值；测试经
 * `bar_strategy_internal::run_bar_strategy_with` 注入一个近未来的时刻以便确定性触发
 * （与 #161 的交易小节表注入同风格：都是内部接缝，不进公开承诺面）。
 *
 * 放在 `dztrader` 命名空间而不是 `manager_internal`：后者只装 B 回调的编译期探测，
 * 塞一个回卷时刻常量会让这个命名空间的含义变糊。
 */
inline constexpr std::int32_t DEFAULT_ROLLBACK_CLOCK_MS = 18 * 60 * 60 * 1000;

/**
 * @brief v1 内置退化交易小节表（组件文档 §5.5）
 *
 * platform.db 的交易小节/竞价窗口数据源尚未落地（ADR 0018 决策 2/3），v1 用内置退化表：
 * 单小节 `[0, 86399]`（覆盖全部交易日内秒）、无竞价窗口。**这不是新数据源、也不是配置
 * 项**（ADR 0032 决策 8 不引入占位配置），只是数据源落地前框架内置的临时表，代码里只有
 * 一处构造，落地后删除。
 *
 * 三条已知限制（策略与迁移必须知晓）：竞价 tick 按普通 tick 处理（`ctx.is_auction()` 恒
 * false）、小节末端整点 tick 会开新 bar、无交易时段过滤。
 *
 * @return 合法的退化表（`TradingSessionTable::valid()` 为 true）
 */
[[nodiscard]] inline bar::TradingSessionTable degraded_trading_session_table() noexcept {
    bar::TradingSessionTable table{};
    table.session_count = 1;
    table.sessions[0] = bar::TradingSession{.begin_sec = 0, .end_sec = 86399};
    table.auction_count = 0;
    return table;
}

/**
 * @brief 判定一张小节表是否为内置退化表（组件文档 §5.5）
 *
 * 退化表 = 恰好一个小节 `[0, 86399]` 且无竞价窗口。启动诊断据此声明口径，而不是靠调用方
 * 额外传一个布尔标志——标志与实际表可能不一致，那样打印出的诊断就是假的。
 *
 * @param table 待判定的表
 * @return true = 内置退化口径
 */
[[nodiscard]] inline bool is_degraded_session_table(
    const bar::TradingSessionTable& table) noexcept {
    return table.session_count == 1 && table.auction_count == 0 &&
           table.sessions[0].begin_sec == 0 && table.sessions[0].end_sec == 86399;
}

/**
 * @brief A 内部：builder 事件 → 窗口推进 + B 回调的适配器（组件文档 §5.4）
 *
 * 三个事件入口的动作顺序即回调顺序：`on_bar_close(上一根)`（窗口尚未推进，`[0]` 仍是
 * 被关闭的上一根）→ `on_bar_open(新根)`（回卷兜底 + 推进窗口 + 写 `[0]`）→
 * `on_bar_update`（把 `Bar` 字段写进 `[0]`）。
 *
 * builder 要求 sink 回调不得抛异常，这个契约由 A 保证：三个 `dispatch_*` 把 B 的调用包进
 * `invoke_b`（`noexcept`），异常在那里被捕获并转为实例失败，因此不会有异常穿过 builder。
 */
template <class Strategy>
struct BarEventSink {
    InstrumentNode<Strategy>* node = nullptr;         ///< 本事件所属节点（地址稳定）
    BarStrategyManager<Strategy>* manager = nullptr;  ///< 驱动的 A

    /// @brief 新 bar 已打开：推进窗口后回调 B（`on_bar_open` 时 OHLC 已等于本笔 tick 价）
    void on_bar_open(const bar::Bar& bar, bool is_auction) {
        manager->dispatch_bar_open(*node, bar, is_auction);
    }

    /// @brief 形成中 bar 已更新：把字段写进 `[0]` 后回调 B（每笔已接受 tick 恰好一次）
    void on_bar_update(const bar::Bar& bar, bool is_auction) {
        manager->dispatch_bar_update(*node, bar, is_auction);
    }

    /// @brief 上一根 bar 已收：窗口尚未推进，`[0]` 仍是被关闭的上一根
    void on_bar_close(const bar::Bar& bar, bool is_auction) {
        manager->dispatch_bar_close(*node, bar, is_auction);
    }
};

/**
 * @brief 每品种一个的节点：ctx + 事件适配器 + builder + B 实例（就地构造、不再搬移）
 *
 * 构造顺序即声明顺序（ctx → sink → builder → instance）：ctx 先绑定自身窗口与价格数组，
 * sink 再取节点与 A 的稳定地址，builder 取 sink 引用，最后默认构造 B。节点存放在
 * `std::map` 里（`try_emplace` 就地构造），地址在实例生命周期内不变。
 */
template <class Strategy>
struct InstrumentNode {
    /**
     * @brief 就地装配一个品种实例
     * @param manager    A（sink 反向调用）
     * @param init       ctx 构造参数
     * @param table      交易小节表（#157 = 内置退化表；#161 换成 platform.db 数据源）
     * @param period_sec bar 周期秒数（来自配置）
     */
    InstrumentNode(BarStrategyManager<Strategy>& manager,
                   const InstrumentInit& init,
                   const bar::TradingSessionTable& table,
                   std::int32_t period_sec)
        : ctx(init),
          sink{this, &manager},
          builder(table, period_sec, sink),
          instance() {}

    InstrumentContext ctx;                                    ///< 品种数据 + B 的 API 面
    BarEventSink<Strategy> sink;                              ///< builder 事件适配器
    bar::IntradayBarBuilder<BarEventSink<Strategy>> builder;  ///< tick→bar 唯一实现（ADR 0031）
    Strategy instance;                                        ///< B：默认构造，节点内不再搬移
};

/**
 * @brief A：引擎侧唯一策略对象（组件文档 §2）
 *
 * 生命周期：`on_start` 加载配置并把每个品种装配成 `InstrumentNode` → 事件循环驱动
 * `on_tick` → `on_stop` 逐个调 `on_destroy` 后销毁节点。
 *
 * 启动失败（配置缺失/非法、订阅失败）通过自主退出通道上报：置 `should_exit()` 为 true 并
 * 由 `exit_code()` 给出非 0 码，引擎在 `on_start` 后立即探测到并优雅退出（主循环不启动，
 * 组件文档 §7）。
 *
 * `Strategy` 只要求可默认构造；六个回调全部可选（编译期探测）。
 */
template <class Strategy>
class BarStrategyManager {
public:
    /**
     * @brief 构造 A
     * @param session_table 交易小节表（v1 = `degraded_trading_session_table()`）
     *
     * 「是否退化」由表自身判定（`is_degraded_session_table`），不另设标志位：两个独立状态
     * 会互相矛盾（注入真实表却仍打印 degraded 诊断），而这条诊断正是 §5.5 要保证的口径。
     */
    explicit BarStrategyManager(const bar::TradingSessionTable& session_table,
                                std::int32_t rollback_clock_ms = DEFAULT_ROLLBACK_CLOCK_MS)
        : session_table_(session_table),
          degraded_(is_degraded_session_table(session_table)),
          rollback_clock_ms_(rollback_clock_ms) {}

    BarStrategyManager(const BarStrategyManager&) = delete;
    BarStrategyManager& operator=(const BarStrategyManager&) = delete;
    BarStrategyManager(BarStrategyManager&&) = delete;
    BarStrategyManager& operator=(BarStrategyManager&&) = delete;

    // ==== 引擎回调（`run_strategy` 鸭子类型接入）====

    /**
     * @brief 启动：读配置 → 打印有效配置 → 订阅 → 装配实例并调 `on_init` → 排 18:00 回卷
     *
     * 失败（配置缺失/非法、订阅失败）时打印诊断、置非 0 退出码并返回，不进入事件循环；
     * 单个品种的 `on_init` 抛异常只丢弃该品种（组件文档 §4.6）。
     *
     * @param ctx 引擎会话句柄（进程内不变，注入各 ctx）
     */
    void on_start(DzContext* ctx) {
        dz_ = ctx;
        if (!load_startup_config()) {
            return;
        }
        // 账户表来自配置（启动快照）。放在最前：启动诊断要打印它，实例构造要取得它的地址。
        // 运行期增删改归 #162，那时这里是「加载一次」而不是「唯一的写入点」。
        accounts_ = effective_.config.accounts;
        print_startup_diagnostics();
        if (!subscribe_configured_instruments()) {
            request_startup_exit();
            return;
        }
        create_instances();
        schedule_daily_rollback();
        enter_live();
    }

    /**
     * @brief 每笔行情 tick → 该品种的 tick→bar 组件（bar 边界由组件负责，A 不自行判断）
     *
     * 未知品种（未订阅/已删除）与失败实例的 tick 直接丢弃：A 不做任何 bar 边界计算。
     *
     * **最近 tick 快照只在「本笔 tick 真的产生了事件」时刷新**（组件文档 §5.1）：这里把
     * tick 挂到 `current_tick_` 上，由事件适配器在分发 `open`/`update`/`close` 时拷进 ctx，
     * 调用返回后立刻清空。被 builder 丢弃的 tick（乱序/非交易时段/量额倒退/非法价格）不产生
     * 事件，因此不会刷新快照——否则一次乱序 tick 的涨跌停就会盖掉当前值，后续单子会夹到
     * 过期价格上。用「有没有事件」而不是「stats 计数有没有涨」判定：非法价格那条丢弃路径
     * 按 builder 契约不计入任何 stats 计数，看计数会把它误判为已接受。
     *
     * @param tick 行情快照（属于本进程订阅的某个合约）
     */
    void on_tick(const DzTick& tick) {
        if (startup_failed_) {
            return;
        }
        const auto node = instruments_.find(manager_internal::instrument_id_of(tick));
        if (node == instruments_.end()) {
            return;
        }
        if (node->second.ctx.is_failed()) {
            return;  // 失败实例：不再接收任何回调（组件文档 §4.6）
        }
        current_tick_ = &tick;
        node->second.builder.on_tick(tick);
        current_tick_ = nullptr;
    }

    /**
     * @brief 定时器帧：识别本框架自己的回卷定时器并执行回卷（组件文档 §4.4）
     *
     * **只处理自己的 `timer_id`**：引擎把本策略收到的定时器帧都投给 `on_schedule`，
     * `timer_id` 是唯一判据。v1 的 B 拿不到 `DzContext`、`InstrumentContext` 也不暴露
     * `dz_schedule_*`，因此这份帧在 v1 里不可能属于别人；按 id 过滤仍是要的——将来若给 B
     * 开放定时器 API，或 A 增加第二个定时器（如 #161 的回放进度），不按 id 过滤就会把别人的
     * 触发当成回卷。未知 id 直接忽略，不当作错误。
     *
     * 回卷对每品种执行 7 条价格数组 + 全部用户序列的搬移，B 完全不可见（纯布局）。
     *
     * @param ev 定时器触发事件（`timer_id` 与注册返回值一致）
     */
    void on_schedule(const DzScheduleEvent& ev) {
        if (startup_failed_ || rollback_timer_ == DZ_TIMER_INVALID ||
            ev.timer_id != rollback_timer_) {
            return;
        }
        relocate_live_instruments();
    }

    /// @brief 停止：注销回卷定时器 → 逐品种调 `on_destroy` → 销毁节点
    void on_stop() {
        cancel_daily_rollback();
        for (auto& [instrument_id, node] : instruments_) {
            dispatch_on_destroy(node);
        }
        instruments_.clear();
    }

    /// @brief 自主退出探测：启动失败时请求引擎立即优雅退出（组件文档 §7）
    [[nodiscard]] bool should_exit() const noexcept { return startup_failed_; }

    /// @brief 自主退出码（仅 `should_exit()` 为 true 时有意义）
    [[nodiscard]] std::int32_t exit_code() const noexcept { return startup_exit_code_; }

private:
    template <class>
    friend struct BarEventSink;  // 事件适配器经 dispatch_* 反向调用 A

    /// @brief 注销 18:00 回卷定时器（引擎随后 `dz_release` 也会清，这里显式收回所有权）
    void cancel_daily_rollback() noexcept {
        if (dz_ == nullptr || rollback_timer_ == DZ_TIMER_INVALID) {
            return;
        }
        (void)dz_schedule_cancel(dz_, rollback_timer_);
        rollback_timer_ = DZ_TIMER_INVALID;
    }

    /**
     * @brief 每日 18:00 回卷定时器（组件文档 §4.4「v1 主路径」）
     *
     * 18:00 在日盘收盘之后、夜盘开盘之前，是天然静默窗：没有 tick 正在处理，回卷只造成一次
     * 微秒级停顿。定时器只做布局（{@link InstrumentContext::relocate}），**不定义 bar 边界、
     * 不关闭任何形成中的 bar**（§5.4：bar 生命周期纯 tick 驱动）。
     *
     * 注册失败不阻断启动：回卷还有推进前内联兜底（`advance_bar` 里 `end` 用尽时搬），
     * 定时器只是把搬移挪到静默窗、避开盘中路径的性能优化。打一行诊断说明退化了。
     */
    void schedule_daily_rollback() {
        if (dz_ == nullptr) {
            return;
        }
        rollback_timer_ = dz_schedule_daily(dz_, rollback_clock_ms_);
        if (rollback_timer_ == DZ_TIMER_INVALID) {
            std::cerr << std::format(
                             "daily rollback timer unavailable | clock_ms={} reason=\"{}\" "
                             "fallback=inline_before_advance",
                             rollback_clock_ms_, dz_errmsg())
                      << '\n';
        }
    }

    /**
     * @brief 对全部存活实例执行回卷（18:00 定时器路径）
     *
     * 失败实例跳过：它不再接收回调，搬移没有收益（且不值得为一个已死的实例做 memmove）。
     * 打一行诊断（每天一次）：回卷是纯布局操作、B 不可见，运维侧唯一的证据就是这行日志——
     * 没有它，「定时器是否真的触发」只能靠猜。
     */
    void relocate_live_instruments() {
        std::size_t relocated = 0;
        for (auto& [instrument_id, node] : instruments_) {
            if (node.ctx.is_failed()) {
                continue;
            }
            node.ctx.relocate();
            ++relocated;
        }
        std::cerr << std::format("daily rollback | instrument_count={} clock_ms={}", relocated,
                                 rollback_clock_ms_)
                  << '\n';
    }

    // ==== 事件适配器回调（`BarEventSink` 调用；A 内部，B 不可见）====

    /**
     * @brief `on_bar_open` 事件：回卷兜底 → 推进窗口 → 调 B
     *
     * 推进后 `[0]` 已是新根，且 OHLC 由 builder 用本笔 tick 播种完毕——B 在 `on_bar_open`
     * 里读 `ctx.close[0]` 即本笔 tick 价（组件文档 §5.1）。
     *
     * **回调门禁（§5.2）**：推进与静默填窗照常进行，但窗口填满前不调 B。
     */
    void dispatch_bar_open(InstrumentNode<Strategy>& node, const bar::Bar& bar, bool is_auction) {
        (void)is_auction;  // ctx.is_auction() 随 #163 接入
        if (node.ctx.is_failed()) {
            return;
        }
        refresh_tick_snapshot(node);
        node.ctx.advance_bar(bar);
        if (!bar_callback_enabled(node.ctx)) {
            return;  // 静默填窗：B 零回调
        }
        if constexpr (manager_internal::HasOnBarOpen<Strategy>) {
            invoke_b(node, "on_bar_open",
                     [](Strategy& instance, InstrumentContext& ctx) { instance.on_bar_open(ctx); });
        }
    }

    /// @brief `on_bar_update` 事件：把 `Bar` 字段写进 `[0]` → 调 B（每笔已接受 tick 一次）
    void dispatch_bar_update(InstrumentNode<Strategy>& node, const bar::Bar& bar, bool is_auction) {
        (void)is_auction;  // ctx.is_auction() 随 #163 接入
        if (node.ctx.is_failed()) {
            return;
        }
        refresh_tick_snapshot(node);
        node.ctx.update_current_bar(bar);
        if (!bar_callback_enabled(node.ctx)) {
            return;  // 静默填窗：B 零回调
        }
        if constexpr (manager_internal::HasOnBarUpdate<Strategy>) {
            invoke_b(node, "on_bar_update", [](Strategy& instance, InstrumentContext& ctx) {
                instance.on_bar_update(ctx);
            });
        }
    }

    /**
     * @brief `on_bar_close` 事件：窗口尚未推进，`[0]` 仍是被关闭的上一根 → 调 B
     *
     * `bar` 不入窗口：被关闭的这根在它自己的 `open` / `update` 事件里已经写进 `[0]`。
     *
     * **回调门禁（§5.2）**：这里判的是「被关闭的那根是否已进入回调期」。上一根是静默填窗的
     * 最后一根（`bar_index == max_lookback - 1`）时，它自己就发生在静默期——但那根 close 不
     * 是「它的」事件而是「新根」的前导，按 AC 必须交付给 B：B 的第一次回调序列是
     * `close(静默填窗最后一根) → open → update`。因此判据是**即将打开的新根**是否已过门禁，
     * 等价于「当前窗口已满」（`size == max_lookback`）。
     */
    void dispatch_bar_close(InstrumentNode<Strategy>& node, const bar::Bar& bar, bool is_auction) {
        (void)is_auction;  // ctx.is_auction() 随 #163 接入
        if (node.ctx.is_failed()) {
            return;
        }
        refresh_tick_snapshot(node);
        // 极值在「bar 收盘前」更新（组件文档 §6.1）：B 在 `on_bar_close` 里读到的
        // `max_position_profit/loss` 已包含刚收这根 bar 的收盘价。放在门禁之前——门禁只
        // 决定「要不要调 B」，不决定台账口径；静默填窗期无持仓，调用是无操作。
        node.ctx.update_profit_extremes(bar.close);
        if (!close_callback_enabled(node.ctx)) {
            return;  // 静默填窗：B 零回调
        }
        if constexpr (manager_internal::HasOnBarClose<Strategy>) {
            invoke_b(node, "on_bar_close", [](Strategy& instance, InstrumentContext& ctx) {
                instance.on_bar_close(ctx);
            });
        }
    }

    /**
     * @brief 回调门禁（组件文档 §5.2）：静默填窗期不调 B
     *
     * 两层判据对应两类事件，差别正是「填满窗口的那一根」：
     *
     * - {@link bar_callback_enabled}（`open` / `update`，推进之后判）：要求
     *   `bar_index >= max_lookback`。索引 `0 … max_lookback-1` 是静默填窗的 `max_lookback`
     *   根，即使最后一根推进后 `size` 已到 `max_lookback`，它自己的 open/update 仍属静默期。
     * - {@link close_callback_enabled}（`close`，推进之前判）：要求 `size == max_lookback`。
     *   被关闭的这根索引是 `max_lookback-1`（静默填窗最后一根），但这次 close 是**新根的前导**，
     *   按 AC 必须交付——B 的第一个回调序列是 `close → open → update`。
     *
     * 回调期恒有 `size == max_lookback`：`bar_index >= max_lookback` 时 `size` 早已封顶。
     */
    [[nodiscard]] static bool bar_callback_enabled(const InstrumentContext& ctx) noexcept {
        return ctx.bar_index_ >= ctx.window_.max_lookback;
    }

    /// @brief `close` 的门禁：窗口已填满（此时被关闭的那根是静默填窗最后一根或更晚）
    [[nodiscard]] static bool close_callback_enabled(const InstrumentContext& ctx) noexcept {
        return ctx.window_.size == ctx.window_.max_lookback;
    }

    /**
     * @brief 把本笔 tick 拷进 ctx 的最近快照（组件文档 §5.1「只有被接受的 tick 才产生事件」）
     *
     * 三个事件入口各调一次。`current_tick_` 只在 `on_tick` 的执行期内非空，因此这里的判空
     * 同时覆盖了「事件从别处进来」这种（当前不存在的）路径：宁可不刷新，也不用一个不在本次
     * tick 生命周期内的快照。
     */
    void refresh_tick_snapshot(InstrumentNode<Strategy>& node) noexcept {
        if (current_tick_ == nullptr) [[unlikely]] {
            return;
        }
        node.ctx.update_tick(*current_tick_);
    }

    /**
     * @brief 进入实盘阶段：对每个存活实例开执行门禁并批量上报逻辑持仓（组件文档 §3/§5.3）
     *
     * `on_start` 的最后一步，顺序是**先上报、再放开**：反过来会让首个 tick 的意图先发一笔单、
     * 再补报一个与它不一致的旧持仓。
     *
     * 分类讨论当下的状态：
     *
     *   - **冷启动（v1 现状，#161 的历史回放未落地）**：回放期没有历史可放，进入 live 时逻辑
     *     台账里只有 `on_init` 阶段建立的持仓（ADR 0030 决策 1：`on_init` 期的意图同样即时
     *     记账）。因此这里必须**照常批量上报**——它可能是 0（无 `on_init` 意图），也可能不是；
     *     硬编码「启动时逻辑持仓为 0」会让 `on_init` 建仓的策略在 UI 上显示空仓；
     *   - **有归档历史时**（#161）：回放期 B 照常下单但只做虚拟成交，结束后同样在这里
     *     `set_live(true)` + `publish_all_positions()` 一次性上报各账户逻辑持仓。
     *
     * #161 只需把 `on_start` 里的调用点挪到回放之后，本函数内容不变。
     */
    void enter_live() {
        for (auto& [instrument_id, node] : instruments_) {
            if (node.ctx.is_failed()) {
                continue;
            }
            // 顺序照 §5.3 第 4 步：**先上报、再放开**。`publish_all_positions()` 只读台账、
            // 不发单，因此它在 live 打开前后调用效果相同；但把顺序写成与文档一致，才让
            // 「之后若有人给上报加上发单/副作用，也不会先发一笔再报一个旧值」这条不变式成立。
            node.ctx.publish_all_positions();
            node.ctx.set_live(true);
        }
        if (!accounts_.empty()) {
            std::cerr << std::format(
                             "account fanout enabled | account_count={} instrument_count={} "
                             "source=ledger_without_warmup_replay ticket=161",
                             accounts_.size(), instruments_.size())
                      << '\n';
        }
    }

    /// @brief 一次 B 回调的调用体（异常边界：捕获后标记该实例失败）
    template <class Callback>
    void invoke_b(InstrumentNode<Strategy>& node,
                  const char* callback,
                  Callback&& callback_body) noexcept {
        try {
            callback_body(node.instance, node.ctx);
        } catch (const std::exception& e) {
            fail_instance(node.ctx, callback, e.what());
        } catch (...) {
            fail_instance(node.ctx, callback, "unknown exception");
        }
    }

    /// @brief 标记实例失败并打印一行诊断（此后该实例不再接收任何回调）
    static void fail_instance(InstrumentContext& ctx, const char* callback, const char* error) {
        ctx.mark_failed();
        std::cerr << std::format("instance failed | instrument={} callback={} error=\"{}\"",
                                 ctx.instrument_id(), callback, error)
                  << '\n';
    }

    /// @brief 调 `on_destroy`（失败实例同样调用；异常只记诊断，不阻止其它实例销毁）
    void dispatch_on_destroy(InstrumentNode<Strategy>& node) {
        if constexpr (manager_internal::HasOnDestroy<Strategy>) {
            try {
                node.instance.on_destroy(node.ctx);
            } catch (const std::exception& e) {
                report_destroy_error(node.ctx, e.what());
            } catch (...) {
                report_destroy_error(node.ctx, "unknown exception");
            }
        }
    }

    /// @brief `on_destroy` 异常诊断（销毁期不再标记失败：实例马上要销毁）
    static void report_destroy_error(const InstrumentContext& ctx, const char* error) {
        std::cerr << std::format("on_destroy threw | instrument={} error=\"{}\"",
                                 ctx.instrument_id(), error)
                  << '\n';
    }

    /**
     * @brief 取配置路径、加载文件，并把结果装进 `ConfigStore`（组件文档 §7）
     *
     * 三种结局：`Loaded` → 用 `load_config_file` 已读到的 document + effective 直接构造
     * `ConfigStore`（**不重读文件**，避免双读与 TOCTOU：文件在两次读之间被改动会让内存镜像
     * 与磁盘不一致），保留给后续票的 `param set` / `account *` 写回用；`GeneratedDefault` →
     * 打印 `result.message` 并以 `config_startup_exit_code()` 非 0 退出（等作者编辑后重启）；
     * 异常（读/解析 `std::runtime_error`、配置非法 `std::invalid_argument`）→ 打印错误并以
     * 非 0 退出。任何一种都不静默继续。
     *
     * @return true = 已得到可用有效配置，可继续启动
     */
    bool load_startup_config() {
        std::filesystem::path path;
        try {
            path = config_path(dz_strategy_home(dz_), dz_strategy_id(dz_));
        } catch (const std::exception& e) {
            report_startup_failure("config_path", e.what());
            return false;
        }
        try {
            ConfigLoadResult result = load_config_file(path);
            if (result.status != ConfigLoadStatus::Loaded) {
                std::cerr << result.message << '\n';
                request_startup_exit(config_startup_exit_code(result.status));
                return false;
            }
            store_ = std::make_unique<ConfigStore>(path, std::move(result.document),
                                                   std::move(*result.effective));
            effective_ = store_->current();
        } catch (const std::exception& e) {
            report_startup_failure("config_load", e.what());
            return false;
        }
        return true;
    }

    /// @brief 启动诊断：有效配置两行 + 退化小节表口径 + 启动摘要（组件文档 §5.5/§7）
    void print_startup_diagnostics() const {
        std::cerr << format_effective_config(effective_) << '\n';
        std::cerr << format_effective_accounts(effective_.config.accounts) << '\n';
        if (degraded_) {
            std::cerr << std::format(
                             "trading session table degraded | mode=builtin sessions={} "
                             "auction_windows={} reason=platform_db_absent",
                             session_table_.session_count, session_table_.auction_count)
                      << '\n';
        }
        std::cerr << std::format(
                         "bar strategy start | instrument_count={} period_sec={} max_lookback={} "
                         "buffer_bars={}",
                         effective_.config.instruments.size(), effective_.config.period_sec,
                         effective_.max_lookback, effective_.buffer_bars)
                  << '\n';
        // 挂靠账户表（`accounts` 可空：0 账户 = 只记逻辑账、不发单、不上报）
        std::cerr << std::format(
                         "account fanout | account_count={} max_scale={} scale_rounding={} "
                         "scale_min_one={}",
                         accounts_.size(),
                         config_internal::format_ratio(effective_.scaling.max_scale),
                         scale_rounding_name(effective_.scaling.rounding),
                         effective_.scaling.min_one ? 1 : 0)
                  << '\n';
        // Tier 1 的浮动盈亏要乘合约乘数、下单价格要按最小变动价位对齐，而合约元数据源属
        // #161 的内部依赖缝：在它落地前 `InstrumentSpec` 的两项都是 0，于是
        //   - `position_profit()` / `contract_profit()` / 极值全线返回 0；
        //   - 下单价格**不做 tick 对齐**（`price_tick == 0` 时跳过对齐，见 execution.h）。
        // **两条都是已知限制，必须在这里说出来**——静默返回 0 会让策略作者以为「浮盈真的是
        // 0」把止损写成永不触发；静默不对齐会让人以为框架按 tick 处理过价格。
        //
        // 无条件打印而不是用「有没有拿到元数据」的标志位：v1 根本没有元数据来源，标志位恒为
        // false，那就是一段只有一个取值的分支。等 #161 接上数据源时，这行自然改成
        // `if (!metadata_loaded) {…}`（那时才有第二个取值）。
        std::cerr << std::format(
                         "instrument metadata unavailable | effect=tier1_profit_zero "
                         "price_alignment_skipped=1 source=absent ticket=161")
                  << '\n';
    }

    /**
     * @brief 一次性订阅配置里的全部品种（组件文档 §8 启动第 2 步）
     *
     * 订阅先于实例创建：保证切换窗口的 tick 不丢（§5.3 步骤 1）。
     *
     * @return true = 订阅请求已写入事件通道；false = 失败（调用方请求非 0 退出）
     */
    bool subscribe_configured_instruments() {
        std::vector<const char*> instruments;
        instruments.reserve(effective_.config.instruments.size());
        for (const std::string& instrument_id : effective_.config.instruments) {
            instruments.push_back(instrument_id.c_str());
        }
        const auto count = static_cast<std::uint32_t>(instruments.size());
        if (!dz_subscribe(dz_, instruments.data(), count, true)) {
            std::cerr << std::format("market data subscribe failed | instrument_count={}",
                                     instruments.size())
                      << '\n';
            return false;
        }
        return true;
    }

    /**
     * @brief 逐品种装配 `InstrumentNode` 并调 `on_init`（组件文档 §8 启动第 3 步）
     *
     * `on_init` 抛异常 = 该品种创建失败：打印诊断、丢弃节点（不调 `on_destroy`），继续创建
     * 其它品种。成功时 ctx 的 `bar_index() == -1`、窗口为空。
     *
     * 账户表与缩放配置在这里随 `InstrumentInit` 注入：ctx 只持账户表的**只读指针**（表由 A
     * 持有、地址稳定，增删改只发生在两次回调之间），缩放配置按值拷一份（启动后不可变）。
     */
    void create_instances() {
        const auto period_sec = effective_.config.period_sec;
        for (const std::string& instrument_id : effective_.config.instruments) {
            InstrumentInit init{
                .instrument_id = instrument_id,
                .max_lookback = effective_.max_lookback,
                .buffer_bars = effective_.buffer_bars,
                // 合约属性：v1 没有元数据来源（#161 接缝），因此 `volume_multiple` 与
                // `price_tick` 都是 0——启动诊断会显式声明这两条已知限制（盈亏恒为 0、价格
                // 不做 tick 对齐）。
                .spec = InstrumentSpec{},
                .dz = dz_,
                .accounts = &accounts_,
                .scaling = effective_.scaling,
            };
            auto [slot, inserted] =
                instruments_.try_emplace(instrument_id, *this, init, session_table_, period_sec);
            if (!inserted) {
                continue;  // 配置层已保证 instruments 不重复
            }
            if constexpr (manager_internal::HasOnInit<Strategy>) {
                // make_series 只在 on_init 内合法；守卫析构自动复位，异常路径无需重复复位
                const InstrumentContext::OnInitScope on_init_scope(slot->second.ctx);
                try {
                    slot->second.instance.on_init(slot->second.ctx);
                } catch (const std::exception& e) {
                    report_init_failure(slot->second.ctx, e.what());
                    instruments_.erase(slot);
                } catch (...) {
                    report_init_failure(slot->second.ctx, "unknown exception");
                    instruments_.erase(slot);
                }
            }
        }
    }

    /// @brief `on_init` 异常诊断（该品种不加入 map，其它品种继续）
    static void report_init_failure(const InstrumentContext& ctx, const char* error) {
        std::cerr << std::format(
                         "instance init failed | instrument={} callback=on_init error=\"{}\"",
                         ctx.instrument_id(), error)
                  << '\n';
    }

    /// @brief 启动失败诊断（配置路径/读取/校验失败，进程随后以非 0 码退出）
    void report_startup_failure(std::string_view stage, const char* error) {
        std::cerr << std::format("strategy startup failed | stage={} error=\"{}\"", stage, error)
                  << '\n';
        request_startup_exit();
    }

    /// @brief 请求引擎优雅退出并给出进程退出码（默认 1：配置/订阅类启动失败）
    void request_startup_exit(std::int32_t code = 1) noexcept {
        startup_failed_ = true;
        startup_exit_code_ = code;
    }

    DzContext* dz_ = nullptr;                      ///< 引擎会话句柄（on_start 注入）
    DzTimerId rollback_timer_ = DZ_TIMER_INVALID;  ///< 18:00 回卷定时器 id（无效 = 注册失败）
    /// 启动加载的配置仓（document + effective；`param set` / `account *` 的写回接缝 #162）
    std::unique_ptr<ConfigStore> store_;
    EffectiveConfig effective_;               ///< 启动快照（构建实例用，进程内不变）
    bar::TradingSessionTable session_table_;  ///< 交易小节表（v1 = 退化表）
    bool degraded_ = true;                    ///< 由 session_table_ 判定（诊断口径，§5.5）
    std::int32_t rollback_clock_ms_ = DEFAULT_ROLLBACK_CLOCK_MS;
    /// 挂靠账户表（启动读配置；#162 起由命令增删改）。**必须在 instruments_ 之前声明**：
    /// 节点里的 ctx 持它的地址，析构顺序保证表比节点活得久。
    AccountTable accounts_;
    /// 本次 `on_tick` 正在分发的 tick（只在分发期内非空；事件入口据此刷新 ctx 快照）
    const DzTick* current_tick_ = nullptr;
    bool startup_failed_ = false;         ///< 启动失败：请求引擎立即退出
    std::int32_t startup_exit_code_ = 0;  ///< 启动失败时的进程退出码
    // 最后声明 → 最先析构：节点不会看到已销毁的配置/会话
    std::map<std::string, InstrumentNode<Strategy>, std::less<>> instruments_;  ///< 地址稳定
};

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_MANAGER_H_
