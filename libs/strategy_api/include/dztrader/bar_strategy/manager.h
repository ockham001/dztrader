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
 * 小节表，以及**回调门禁 + 静默填窗 + 用户序列 + 18:00 回卷**（§4.4/§5.2）；交易意图与逻辑
 * 台账（#159）、挂靠缩放与上报（#160）、历史预热回放（#161）、**动态管理命令与参数热更新**
 * （#162）。竞价相位（#163）后续接入，接缝见各成员注释。
 *
 * **动态管理（#162，组件文档 §8/§9）**：`on_ui_input` 是本框架唯一的命令入口——按
 * `command_internal::parse_command` 的 token 文法解析，再执行 `instrument *` / `account *` /
 * `param *` / `status` / `quit`。命令走**写盘成功 → 改内存 → 上报 → 可选同步单**的顺序（§8
 * 「写盘顺序」），解析或校验失败时返回单行错误且**内存与磁盘都不变**。结果一律写
 * `DZ_FRAME_OUTPUT_UI`，关键错误额外 `dz_notify_ui`。
 */
#ifndef DZTRADER_BAR_STRATEGY_MANAGER_H_
#define DZTRADER_BAR_STRATEGY_MANAGER_H_

#include <dztrader/api.h>
#include <dztrader/bar/intraday_bar_builder.h>
#include <dztrader/bar/types.h>
#include <dztrader/bar_strategy/command.h>
#include <dztrader/bar_strategy/config.h>
#include <dztrader/bar_strategy/config_file.h>
#include <dztrader/bar_strategy/context.h>
#include <dztrader/bar_strategy/dependencies.h>
#include <dztrader/bar_strategy/params.h>
#include <dztrader/error.h>
#include <dztrader/strategy_engine.h>
#include <dztrader/struct.h>

#include <algorithm>
#include <array>
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

/// @brief B 是否实现 `on_params_update(InstrumentContext&)`（#162；参数热更新后重读）
template <typename T>
concept HasOnParamsUpdate =
    requires(T instance, InstrumentContext& ctx) { instance.on_params_update(ctx); };

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
     * @brief 该品种的回放尾部（缓冲 tick 续接的比较基准 B；组件文档 §5.3）
     *
     * §5.3 的「锚点」指该实例窗口开始接收 bar 的最早一根（回放从它开始）；启动时它由历史
     * 的第一根自然确定（`instrument add`
     * 复用同一条回放路径，#162）。续接过滤只需要**尾部**：最后一根 归档 bar 的 `(begin_sec,
     * is_final)` 决定同桶缓冲 tick 丢弃还是续接。`valid == false`
     * 表示该品种没有可用历史（冷启动或归档不可用），续接规则不生效。
     */
    struct ReplayTail {
        bool valid = false;   ///< false = 该实例没有历史尾部（冷启动）
        bar::Bar last_bar{};  ///< 回放的最后一根归档 bar（续接的比较基准 B）
    };

    /**
     * @brief 就地装配一个品种实例
     * @param manager    A（sink 反向调用）
     * @param init       ctx 构造参数
     * @param table      交易小节表（v1 = 内置退化表；数据源落地后由依赖缝注入）
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
    // ---- A 侧的回放簿记（B 不可见；不参与地址稳定性约束）----
    ReplayTail replay_tail;          ///< 回放尾部（续接过滤用）
    std::int32_t silent_bars = 0;    ///< 该实例静默填窗的历史根数（诊断口径）
    std::int32_t replayed_bars = 0;  ///< 该实例进入回调期的历史根数（诊断口径）
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
     * @brief 构造 A（完整依赖注入形态）
     * @param dependencies 三个内部依赖缝 + 回卷时刻（组件文档 §5.3/§5.5/§8）
     *
     * 「是否退化」由表自身判定（`is_degraded_session_table`），不另设标志位：两个独立状态
     * 会互相矛盾（注入真实表却仍打印 degraded 诊断），而这条诊断正是 §5.5 要保证的口径。
     */
    explicit BarStrategyManager(const bar_strategy_internal::FrameworkDependencies& dependencies)
        : dependencies_(dependencies),
          degraded_(is_degraded_session_table(dependencies.session_table)) {}

    /**
     * @brief 构造 A（只需要换小节表/回卷时刻的简化形态）
     *
     * 历史 bar 与合约元数据走默认归档实现（{@link bar_strategy_internal::query_archive_history}
     * / {@link bar_strategy_internal::load_archive_metadata}）。公开入口 `run_bar_strategy()`
     * 与既有 Seam A 测试用这一形态；注入 fake 数据源的 Seam B 测试用完整形态。
     */
    explicit BarStrategyManager(const bar::TradingSessionTable& session_table,
                                std::int32_t rollback_clock_ms = DEFAULT_ROLLBACK_CLOCK_MS)
        : BarStrategyManager(
              bar_strategy_internal::make_dependencies(session_table, rollback_clock_ms)) {}

    BarStrategyManager(const BarStrategyManager&) = delete;
    BarStrategyManager& operator=(const BarStrategyManager&) = delete;
    BarStrategyManager(BarStrategyManager&&) = delete;
    BarStrategyManager& operator=(BarStrategyManager&&) = delete;

    // ==== 引擎回调（`run_strategy` 鸭子类型接入）====

    /**
     * @brief 启动：读配置 → 订阅 → 查元数据 → 装配实例 → 回放 → 续接 → 进入实盘
     *
     * 顺序即组件文档 §5.3/§8 的启动序列，每一步的存在理由：
     *
     *   1. `load_startup_config()`：配置缺失/非法 → 打印诊断 + 非 0 退出，不静默继续；
     *   2. `subscribe_configured_instruments()`：**订阅先于历史查询**，保证切换窗口的 tick
     *      不丢（它们会在第 6 步被排空并按续接规则处理）；
     *   3. `create_instances()`：先查该品种的合约元数据（缺失 = 该实例创建失败），再就地装配
     *      节点并调 `on_init`；
     *   4. `schedule_daily_rollback()`：注册 18:00 回卷（只做布局，不影响 bar 边界）；
     *   5. `warmup_history_replay()`：逐品种查历史 → 静默填窗 → 合成 tick 回放（**阻塞在启动
     *      阶段**，期间不派发其它事件；回放期 B 照常运行、下单只做虚拟成交）；
     *   6. `drain_buffered_ticks()`：回放完成后排空缓冲 tick，按「最后一根已收/未完成」规则
     *      丢弃或续接；
     *   7. `enter_live()`：全部品种处理完 → 一次性上报各账户逻辑持仓 → 放开执行门禁。
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
        // 账户表与参数仓来自配置（启动快照）。放在最前：启动诊断要打印账户表，实例构造要
        // 取得两者的地址。运行期由命令增删改（#162），启动这里是「加载一次」。
        accounts_ = effective_.config.accounts;
        params_ = ParamStore(effective_.config.params);
        print_startup_diagnostics();
        if (!subscribe_configured_instruments()) {
            request_startup_exit();
            return;
        }
        create_instances();
        print_metadata_diagnostics();
        schedule_daily_rollback();
        warmup_history_replay();
        drain_buffered_ticks();
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
        InstrumentNode<Strategy>* node = live_instance(tick);
        if (node == nullptr) {
            return;  // 未订阅/已删除/失败实例：不接收任何回调（组件文档 §4.6）
        }
        feed_tick(*node, tick);
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

    /// @brief 停止：注销回卷定时器 → 逐品种调 `on_destroy` → 销毁节点 → 关闭归档库句柄
    void on_stop() {
        cancel_daily_rollback();
        for (auto& [instrument_id, node] : instruments_) {
            dispatch_on_destroy(node);
        }
        instruments_.clear();
        release_archive_db();
    }

    /// @brief 自主退出探测：启动失败或收到 `quit` 命令时请求引擎立即优雅退出（组件文档 §7）
    [[nodiscard]] bool should_exit() const noexcept { return startup_failed_ || quit_requested_; }

    /**
     * @brief 自主退出码（`should_exit()` 为 true 时有意义）
     *
     * 启动失败 → 非 0（配置/订阅类）；`quit` 命令 → **0**（引擎自主退出路径，组件文档 §8）：
     * 运维主动停进程不是错误，用 0 让 master 侧不把它记成崩溃。
     */
    [[nodiscard]] std::int32_t exit_code() const noexcept { return startup_exit_code_; }

    /**
     * @brief UI_INPUT 命令入口（组件文档 §7「UI_INPUT 宿主命令」/ §8；#162）
     *
     * 三步，顺序固定：
     *
     *   1. **解析**（`command_internal::parse_command`）：文法/参数个数/取值错误 → 单行错误
     *      写 OUTPUT_UI，**内存与磁盘都不变**；
     *   2. **执行**（`execute_command`）：命令内部的校验失败（账户已存在、品种已删除、写盘
     *      失败等）同样返回单行错误而不改内存；写盘顺序照 §8「先写盘成功、再改内存」；
     *   3. **输出**：成功的结果文本与失败的错误行都写 `DZ_FRAME_OUTPUT_UI`；`quit` 与关键
     *      错误额外 `dz_notify_ui`。
     *
     * 为什么所有结果都走 OUTPUT_UI：它是「对 UI 输入的响应」这条语义的唯一载体（`api.h` 的
     * `dz_output_ui` 注释）；`dz_notify_ui` 是主动告警通道，只用于需要人立刻决策的关键错误。
     * 两者的分工不在本条强行合并——把成功结果也弹成通知会让消息中心被每个 `list` 刷满。
     *
     * 异常边界：命令实现可能抛异常（配置写盘的 `std::runtime_error`、校验的
     * `std::invalid_argument`），引擎侧有 try/catch，但**框架自己要给 A 的状态兜底**——半途
     * 抛出的命令绝不能留下「内存改了、磁盘没改」的半完成状态，因此执行体内部逐条捕获并转成
     * 错误行。这里不再外围包一层：每条命令自己知道「已经改到哪一步」。
     *
     * @param data UI_INPUT 的文本 payload（变长帧；引擎已校验边界与终止符）
     */
    void on_ui_input(std::string_view data) {
        if (startup_failed_ || quit_requested_) {
            return;  // 已在退出路径上：不再受理新命令
        }
        command_internal::ParseResult parsed = command_internal::parse_command(data);
        if (!parsed.command.has_value()) {
            // 解析失败：单行错误 + 内存不变（§7 明文要求）。错误行本身就是响应，不弹通知——
            // 语法错误是操作员当场可改的，不需要「需要立刻决策」级别的打扰。
            output_ui(parsed.error);
            return;
        }
        const command_internal::Command& command = *parsed.command;
        CommandOutcome outcome = execute_command(command);
        output_ui(outcome.text);
        if (outcome.notify) {
            notify_ui(outcome.text, outcome.error_level ? DZ_NOTIFY_ERROR : DZ_NOTIFY_WARN);
        }
    }

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
        rollback_timer_ = dz_schedule_daily(dz_, dependencies_.rollback_clock_ms);
        if (rollback_timer_ == DZ_TIMER_INVALID) {
            std::cerr << std::format(
                             "daily rollback timer unavailable | clock_ms={} reason=\"{}\" "
                             "fallback=inline_before_advance",
                             dependencies_.rollback_clock_ms, dz_errmsg())
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
                                 dependencies_.rollback_clock_ms)
                  << '\n';
    }

    // ==== UI_INPUT 命令的执行（#162；组件文档 §7/§8/§9）====
    //
    // 分层：`on_ui_input` 只做「解析 → 执行 → 输出」的编排，每条命令的实现落在
    // `run_*_command` 里。这样做的理由与 ctx 的「单一漏斗」同构：命令面有 12 条，若每条都在
    // `on_ui_input` 的 switch 里内联，就会长出 12 处各自的「写盘 / 改内存 / 上报 / 发单」顺序，
    // 而这四步的顺序正是 §8 唯一值得被强制的东西。
    //
    // 另两条贯穿所有命令的口径：
    //   - **失败不改内存**：所有校验都在改内存之前完成，写盘也在改内存之前；
    //   - **配置写回后仍能通过启动加载校验**：写回前经 `resolve_effective_config` 全量校验
    //     （`ConfigStore::commit` 内部做），因此 `instrument del` 删掉最后一个品种会被拒——
    //     否则写回的文件下次启动必然报错。

    /// @brief 一条命令的执行结果（文本 + 是否额外推 UI 通知）
    struct CommandOutcome {
        std::string text;          ///< 写 OUTPUT_UI 的单行或多行结果
        bool notify = false;       ///< 是否额外 `dz_notify_ui`（关键错误 / `quit`）
        bool error_level = false;  ///< `notify` 时用 ERROR 还是 WARN：关键错误（实例建不起来）用
                                   ///< ERROR，quit 用 WARN
    };

    /**
     * @brief 把值渲染成满足日志格式的 `k=v` 右侧（含空格/等号/双引号时加双引号）
     *
     * AGENTS §四：值含空格、等号或引号时必须用双引号包裹，否则「按空白切 `k=v`」的日志解析器
     * 会把一个值读成两个字段。命令层的 token 不含空白，但**可以含等号**（`param set tag a=b`
     * 合法），所以这里按同一口径处理。
     *
     * 只用于**日志**（stderr）；`OUTPUT_UI` 是给操作员看的文本，不加引号（加了会让人以为要按
     * 引号再输入一次）。
     */
    [[nodiscard]] static std::string quote_log_value(const std::string& value) {
        if (value.find_first_of(" =\"") == std::string::npos) {
            return value;
        }
        return std::format("\"{}\"", value);
    }

    /**
     * @brief 写回失败的统一错误行（6 条命令共用同一形状）
     *
     * `ConfigStore::commit` 有一组固定的失败面（全量校验、不可变字段守卫、原子写盘），对操作员
     * 而言它们的处置完全相同：**命令失败、内存与磁盘都不变**。把错误行的形状收在一处，是为了
     * 让「写回失败长什么样」不随命令漂移——`stage=config_write` 是运维 grep 的锚点，六处各写
     * 一遍就一定会有一处漏字段。
     *
     * @param command_name 命令名（`instrument add` / `account set` / …）
     * @param field        目标字段名（`instrument` / `account` / `name`）
     * @param target       目标值（品种 id / 账户 id / 参数名）
     * @param error        底层错误消息（来自 `commit_config_with`）
     * @return 可直接返回的单行错误
     */
    [[nodiscard]] static CommandOutcome writeback_failed(std::string_view command_name,
                                                         std::string_view field,
                                                         const std::string& target,
                                                         const std::string& error) {
        return command_error(std::format("{} failed | {}={} stage=config_write error=\"{}\"",
                                         command_name, field, target, error));
    }

    /**
     * @brief 同步单跳过/拒单的追记行（组件文档 §6.2「跳过并输出错误行」）
     *
     * 命令本身**成功**（写盘与逻辑台账都已按命令推进），但同步单没发出去——这是操作员必须
     * 知道的分叉：实际仓位与逻辑口径在这一刻不一致了。返回空串表示没有需要追记的东西。
     *
     * @param tally 该命令发出的全部同步单的结局汇总
     * @return 追加到命令结果后的一行（或空串）
     */
    [[nodiscard]] static std::string sync_skipped_line(const SyncOrderTally& tally) {
        if (!tally.has_problem()) {
            return {};
        }
        return std::format(
            "sync orders incomplete | submitted={} skipped_no_market_data={} "
            "rejected={}",
            tally.submitted, tally.skipped_no_market_data, tally.rejected);
    }

    /**
     * @brief 把「同步单没发全」的追记行拼到命令结果之后
     *
     * 命令结果本身是**成功**行（写盘与逻辑台账都已按命令推进），但若同步单因无行情/手数越界
     * 没发出去，操作员必须看到——组件文档 §6.2 要求「跳过并输出错误行」。两者不合并成一行
     * 错误：命令确实成功了，把成功改写成「失败」会让脚本化运维误判并重试（重试还会因
     * 「已存在/未挂靠」再次失败）。分两行表达两件事。
     *
     * @param result 命令的成功结果行
     * @param tally  该命令发出的全部同步单结局
     * @return 结果行（无问题时原样返回）
     */
    [[nodiscard]] static std::string with_sync_notes(std::string result,
                                                     const SyncOrderTally& tally) {
        const std::string note = sync_skipped_line(tally);
        if (!note.empty()) {
            result += '\n';
            result += note;
        }
        return result;
    }

    /// @brief 失败结果的统一构造（`text` 即给操作员看的单行错误）
    static CommandOutcome command_error(std::string text,
                                        bool notify = false,
                                        bool error_level = false) {
        return CommandOutcome{
            .text = std::move(text), .notify = notify, .error_level = error_level};
    }

    /// @brief 成功结果的统一构造
    static CommandOutcome command_ok(std::string text) {
        return CommandOutcome{.text = std::move(text), .notify = false, .error_level = false};
    }

    /// @brief 按 kind 分派到具体命令实现（`on_ui_input` 与测试都经这里）
    CommandOutcome execute_command(const command_internal::Command& command) {
        switch (command.kind) {
            case command_internal::CommandKind::InstrumentAdd:
                return run_instrument_add(command.target);
            case command_internal::CommandKind::InstrumentDel:
                return run_instrument_del(command.target, command.sync, command.market);
            case command_internal::CommandKind::InstrumentList:
                return run_instrument_list();
            case command_internal::CommandKind::AccountAdd:
                return run_account_add(command.target, command.scale, command.sync, command.market);
            case command_internal::CommandKind::AccountSet:
                return run_account_set(command.target, command.scale, command.sync, command.market);
            case command_internal::CommandKind::AccountDel:
                return run_account_del(command.target, command.sync, command.market);
            case command_internal::CommandKind::AccountList:
                return run_account_list();
            case command_internal::CommandKind::ParamGet:
                return run_param_get(command.target);
            case command_internal::CommandKind::ParamSet:
                return run_param_set(command.target, command.value_token);
            case command_internal::CommandKind::ParamList:
                return run_param_list();
            case command_internal::CommandKind::Status:
                return run_status();
            case command_internal::CommandKind::Quit:
                return run_quit();
        }
        // 12 个分支已全覆盖，无可达的兜底路径。仍然写一行「未知种类」返回而不是让控制流掉出
        // 函数（那是 UB）：将来 `CommandKind` 加值时，编译器会在 `-Wswitch` 上报缺失分支，
        // 而这行保证即便有人加了 `default:` 也不会静默返回一个未初始化的结果。
        return command_error(std::format("invalid command | reason=unknown_kind kind={}",
                                         command_internal::command_kind_name(command.kind)));
    }

    // ---- instrument add / del / list ----

    /**
     * @brief `instrument add <id>`：查元数据 → 订阅 → 建实例（`on_init`）→ 预热 → 补报 → live
     *
     * 顺序照 §8 的 `instrument add` 条目，每一步失败时**进程继续**（与启动期不同：启动期
     * 配置非法直接退出，运行期命令失败只是一条错误行）。
     *
     * 与启动路径共用同一批构件：`load_metadata` 查元数据、`InstrumentNode` 就地装配、
     * `replay_instrument_history` 静默填窗 + 回放、`on_init` 的异常边界。刻意**不**复用
     * `create_instances()`：那个函数遍历整份配置、按配置顺序创建并决定 metadata 计数字段，
     * 复用它会让「单品种创建」带上「重建全部」的语义，而启动期与运行期的失败处置不同
     * （前者计入诊断行，后者回一行错误给操作员）。
     *
     * 写盘顺序：**实例先成功创建再写盘**。反过来（先写盘、创建失败）会让配置文件里留下一个
     * 运行期并不存在的品种，下次启动才暴露——「写盘即承诺该品种可用」是本命令的语义基础。
     * 写盘失败则回滚：销毁刚建的实例（调 `on_destroy`）、退订该品种、内存恢复原状。
     */
    CommandOutcome run_instrument_add(const std::string& instrument_id) {
        if (instrument_id.empty()) {
            return command_error("invalid command | reason=empty_instrument_id");
        }
        if (instruments_.contains(instrument_id)) {
            return command_error(std::format(
                "instrument add failed | instrument={} reason=already_exists", instrument_id));
        }
        // 1. 合约元数据（必需字段缺失 → 命令失败、进程继续；§8）
        InstrumentSpec spec;
        bool metadata_loaded = false;
        const bar_strategy_internal::MetadataResult metadata = load_metadata(instrument_id);
        switch (metadata.status) {
            case bar_strategy_internal::MetadataStatus::Loaded:
                spec = metadata.spec;
                metadata_loaded = true;
                break;
            case bar_strategy_internal::MetadataStatus::SourceUnavailable:
                // 与启动期同口径：数据源不可用是部署级降级，用零元数据继续（两条已知限制
                // 已在启动诊断里声明过，不在每次 add 时重复刷）。
                metadata_source_unavailable_ = true;
                break;
            case bar_strategy_internal::MetadataStatus::Missing:
            case bar_strategy_internal::MetadataStatus::Invalid:
                report_metadata_failure(instrument_id, metadata);
                return command_error(
                    std::format("instrument add failed | instrument={} status={} detail=\"{}\" "
                                "notify=instrument_metadata_unavailable",
                                instrument_id,
                                metadata.status == bar_strategy_internal::MetadataStatus::Missing
                                    ? "missing"
                                    : "invalid",
                                metadata.message),
                    /*notify=*/true, /*error_level=*/true);
        }

        // 2. 订阅（追加，不替换）。失败 → 命令失败、内存不变（§8 第 2 步）。
        if (!subscribe_one(instrument_id)) {
            return command_error(
                std::format("instrument add failed | instrument={} stage=subscribe error=\"{}\"",
                            instrument_id, dz_errmsg()));
        }

        // 3. 就地装配节点并调 on_init。`on_init` 抛异常 = 该实例创建失败（§4.6）。
        InstrumentInit init{
            .instrument_id = instrument_id,
            .max_lookback = effective_.max_lookback,
            .buffer_bars = effective_.buffer_bars,
            .spec = spec,
            .dz = dz_,
            .accounts = &accounts_,
            .params = &params_,
            .scaling = effective_.scaling,
        };
        auto [slot, inserted] = instruments_.try_emplace(
            instrument_id, *this, init, dependencies_.session_table, effective_.config.period_sec);
        if (!inserted) {
            unsubscribe_one(instrument_id);
            return command_error(std::format(
                "instrument add failed | instrument={} reason=node_conflict", instrument_id));
        }
        if (!init_new_instance(slot->second)) {
            instruments_.erase(slot);  // on_init 失败：节点丢弃，不调 on_destroy（§4.6）
            unsubscribe_one(instrument_id);
            return command_error(
                std::format("instrument add failed | instrument={} stage=on_init", instrument_id));
        }

        // 4. 该品种自己的锚点 = 本次加载历史的最早一根；有历史则静默填窗 + 回放（§8）。
        warmup_single_instrument(slot->second);

        // 5. 写盘（把新品种加进 `instruments`）。失败 → 回滚实例与订阅、内存不变。
        //
        // **幂等追加**：该 id 可能已经写在配置里，但启动期创建失败（元数据缺失 / `on_init`
        // 抛异常）——那种情况下实例表里没有它，而配置里保留着它（我们不悄悄删用户的配置）。
        // 运行期 `instrument add` 正是这类品种的补救路径，因此只在缺失时追加；无条件
        // `push_back` 会让重试撞上配置层的 `duplicate` 校验、回一句与真实原因无关的错误。
        std::string writeback_error;
        if (!commit_config_with(
                [&instrument_id](BarStrategyConfig& next) {
                    if (std::ranges::find(next.instruments, instrument_id) ==
                        next.instruments.end()) {
                        next.instruments.push_back(instrument_id);
                    }
                },
                writeback_error)) {
            dispatch_on_destroy(slot->second);
            instruments_.erase(slot);
            unsubscribe_one(instrument_id);
            return writeback_failed("instrument add", "instrument", instrument_id, writeback_error);
        }

        // 6. 补报该品种各账户逻辑持仓 → 进入 live（§8 的最后两步）。
        //
        // 顺序与 `enter_live()` 一致：**先上报、再放开**（反过来会让首个 tick 的意图先发一笔单、
        // 再补报一个与它不一致的旧持仓）。失败实例（B 在回放期抛过异常）同样上报——§4.6 保留
        // 它的逻辑台账，跳过会让 UI 镜像停在旧值；但执行门禁对它保持关闭（失败实例本来就拒单，
        // `set_live` 交给它没有意义）。
        slot->second.ctx.publish_all_positions();
        const bool failed = slot->second.ctx.is_failed();
        if (!failed) {
            slot->second.ctx.set_live(true);
        }

        std::cerr << std::format(
                         "instrument added | instrument={} replayed_bars={} silent_bars={} "
                         "metadata={} failed={}",
                         instrument_id, slot->second.replayed_bars, slot->second.silent_bars,
                         metadata_loaded ? "loaded" : "unavailable", failed ? 1 : 0)
                  << '\n';
        return command_ok(std::format("instrument add ok | instrument={}", instrument_id));
    }

    /**
     * @brief `instrument del <id> [sync|nosync] [market]`：可选 `−L_i` 平仓单 → 退订 →
     *        `on_destroy` → 逐账户清零上报 → 移除节点（§8）
     *
     * 顺序不能换：**先发平仓单再退订/销毁**（`sync` 单需要该品种的行情快照来取对手价，销毁
     * 后就没有了）；**先 `on_destroy` 再清零上报**（B 在 `on_destroy` 里可能读 Tier 1，
     * 上报清零属于框架侧的对账动作，应在用户回调之后做，避免 B 看到已被清空的镜像）。
     *
     * 写盘顺序：**先写盘成功再改内存**。`ConfigStore::commit` 会全量校验，删掉最后一个品种
     * 会让 `instruments` 为空 → 命令失败、实例与订阅都保持原样（§7「写回后可重新加载」）。
     */
    CommandOutcome run_instrument_del(const std::string& instrument_id, bool sync, bool market) {
        if (instrument_id.empty()) {
            return command_error("invalid command | reason=empty_instrument_id");
        }
        const auto node = instruments_.find(instrument_id);
        if (node == instruments_.end()) {
            return command_error(std::format(
                "instrument del failed | instrument={} reason=not_found", instrument_id));
        }
        // 1. 写盘（先从配置里移除该品种）。失败 → 命令失败、内存不变。
        std::string writeback_error;
        if (!commit_config_with(
                [&instrument_id](BarStrategyConfig& next) {
                    std::erase(next.instruments, instrument_id);
                },
                writeback_error)) {
            return writeback_failed("instrument del", "instrument", instrument_id, writeback_error);
        }
        // 2. 可选 sync 平仓单（`qty = −L_i(old)`；先发单，再退订/销毁）
        const SyncOrderTally close_tally = node->second.ctx.submit_sync_close_orders(sync, market);
        // 3. 退订该品种（失败只诊断：实例正在被移除，订阅残留由 master 侧的超集校验兜底）
        unsubscribe_one(instrument_id);
        // 4. 销毁实例（`on_destroy`）
        dispatch_on_destroy(node->second);
        // 5. 逐账户清零上报（§8：移除实例前把 UI 镜像清成 0）
        node->second.ctx.publish_zero_all_accounts();
        // 6. 移除节点
        instruments_.erase(node);

        std::cerr << std::format("instrument removed | instrument={} sync={} market={}",
                                 instrument_id, sync ? 1 : 0, market ? 1 : 0)
                  << '\n';
        return command_ok(with_sync_notes(
            std::format("instrument del ok | instrument={}", instrument_id), close_tally));
    }

    /// @brief `instrument list`：逐品种一行（品种 + bar 序号 + 逻辑持仓 + 各账户 `L_i`）
    CommandOutcome run_instrument_list() const {
        if (instruments_.empty()) {
            return command_ok("instrument list | -");
        }
        std::string text = std::format("instrument list | count={}", instruments_.size());
        for (const auto& [instrument_id, node] : instruments_) {
            text += '\n';
            text += std::format("{} bar_index={} net_position={} failed={}", instrument_id,
                                node.ctx.bar_index(), node.ctx.net_position(),
                                node.ctx.is_failed() ? 1 : 0);
            text += format_account_positions(node.ctx);
        }
        return command_ok(std::move(text));
    }

    // ---- account add / set / del / list ----

    /**
     * @brief `account add <id> <scale> [sync|nosync] [market]`：校验 → 写盘 → 入表 → 重报 →
     *        可选同步单（§8）
     *
     * `sync` 的差额按 `new_l` 计算（新账户 `old_l = 0`，组件文档 §6.2「已确认口径」），
     * 假设该账户在这些品种上的实际持仓为 0——不为 0 时 sync 单会叠加，偏差交账户级一键同步。
     */
    CommandOutcome run_account_add(const std::string& account_id,
                                   double scale,
                                   bool sync,
                                   bool market) {
        if (account_id.empty()) {
            return command_error("invalid command | reason=empty_account_id");
        }
        if (find_account(account_id) != nullptr) {
            return command_error(
                std::format("account add failed | account={} reason=already_attached", account_id));
        }
        if (std::optional<std::string> invalid = validate_scale("account add", account_id, scale);
            invalid.has_value()) {
            return command_error(std::move(*invalid));
        }
        // 1. 写盘（append 到 accounts）
        std::string writeback_error;
        if (!commit_config_with(
                [&account_id, scale](BarStrategyConfig& next) {
                    next.accounts.push_back(
                        AttachedAccount{.account_id = account_id, .scale = scale});
                },
                writeback_error)) {
            return writeback_failed("account add", "account", account_id, writeback_error);
        }
        // 2. 入账户表（写盘成功之后；表地址稳定，元素地址变化不影响 ctx 的指针）
        accounts_.push_back(AttachedAccount{.account_id = account_id, .scale = scale});
        // 3. 重算并上报各品种该账户逻辑持仓
        const AttachedAccount& added = accounts_.back();
        for (auto& [instrument_id, node] : instruments_) {
            node.ctx.publish_position_for(added);
        }
        // 4. 可选 sync（写盘成功之后才发单，§8「sync 发单发生在写盘成功之后」）
        SyncOrderTally tally;
        if (sync) {
            for (auto& [instrument_id, node] : instruments_) {
                const LargeVolume new_l = node.ctx.logical_position_for(added);
                if (new_l == 0) {
                    continue;  // 该品种无逻辑持仓：新账户目标就是 0，无单可发
                }
                tally.record(node.ctx.submit_sync_order(added, new_l, market));
            }
        }
        std::cerr << std::format("account attached | account={} scale={} sync={} market={}",
                                 account_id, config_internal::format_ratio(scale), sync ? 1 : 0,
                                 market ? 1 : 0)
                  << '\n';
        return command_ok(
            with_sync_notes(std::format("account add ok | account={} scale={}", account_id,
                                        config_internal::format_ratio(scale)),
                            tally));
    }

    /**
     * @brief `account set <id> scale <v> [sync|nosync] [market]`：校验 → 写盘 → 改 rate →
     *        重报 → 可选同步单（§8）
     *
     * 差量 `qty = L_i(new) − L_i(old)`：改表之前先用**旧 scale** 算出 `old_l`、用**新 scale**
     * 算出 `new_l`（`logical_position_with_scale`），改表发生在写盘成功之后。
     */
    CommandOutcome run_account_set(const std::string& account_id,
                                   double scale,
                                   bool sync,
                                   bool market) {
        if (account_id.empty()) {
            return command_error("invalid command | reason=empty_account_id");
        }
        AttachedAccount* const target = find_account(account_id);
        if (target == nullptr) {
            return command_error(
                std::format("account set failed | account={} reason=not_attached", account_id));
        }
        if (std::optional<std::string> invalid = validate_scale("account set", account_id, scale);
            invalid.has_value()) {
            return command_error(std::move(*invalid));
        }
        // `target` 指向 `accounts_` 的元素；`commit_config_with` 只写配置文件与 `effective_`，
        // **不碰 `accounts_`**，因此这句指针在写回之后仍然有效。记下旧 scale 供差量计算。
        const double old_scale = target->scale;
        if (old_scale == scale) {
            // 幂等：scale 未变则无差量、无上报、无单（也不算错误——操作员重放同一命令是常态）
            return command_ok(std::format("account set ok | account={} scale={} changed=0",
                                          account_id, config_internal::format_ratio(scale)));
        }
        // 1. 写盘
        std::string writeback_error;
        if (!commit_config_with(
                [&account_id, scale](BarStrategyConfig& next) {
                    for (AttachedAccount& account : next.accounts) {
                        if (account.account_id == account_id) {
                            account.scale = scale;
                        }
                    }
                },
                writeback_error)) {
            return writeback_failed("account set", "account", account_id, writeback_error);
        }
        // 2. 改表（元素地址不变，ctx 只持表地址，因此无需重绑）
        target->scale = scale;
        const AttachedAccount& updated = *target;
        // 3. 重报各品种该账户逻辑持仓
        for (auto& [instrument_id, node] : instruments_) {
            node.ctx.publish_position_for(updated);
        }
        // 4. 可选 sync：差量基于「命令前的 L 与命令后的 L」
        SyncOrderTally tally;
        if (sync) {
            for (auto& [instrument_id, node] : instruments_) {
                const LargeVolume old_l = node.ctx.logical_position_with_scale(old_scale);
                const LargeVolume new_l = node.ctx.logical_position_with_scale(scale);
                const LargeVolume delta = saturating_delta(new_l, old_l);
                if (delta == 0) {
                    continue;
                }
                tally.record(node.ctx.submit_sync_order(updated, delta, market));
            }
        }
        std::cerr << std::format(
                         "account scale changed | account={} old_scale={} new_scale={} "
                         "sync={} market={}",
                         account_id, config_internal::format_ratio(old_scale),
                         config_internal::format_ratio(scale), sync ? 1 : 0, market ? 1 : 0)
                  << '\n';
        return command_ok(
            with_sync_notes(std::format("account set ok | account={} scale={} changed=1",
                                        account_id, config_internal::format_ratio(scale)),
                            tally));
    }

    /**
     * @brief `account del <id> [sync|nosync] [market]`：写盘 → 可选 `−L_i` 平仓单 →
     *        立即移除并通配清零上报（§8）
     *
     * 删除时不再逐品种清零，而是用「账户 + 空品种」通配一次清掉该账户本策略的全部品种
     * （契约 strategy §SET_LOGICAL_POSITION）——账户已不在表内，逐品种遍历它已不可能。
     */
    CommandOutcome run_account_del(const std::string& account_id, bool sync, bool market) {
        if (account_id.empty()) {
            return command_error("invalid command | reason=empty_account_id");
        }
        AttachedAccount* const target = find_account(account_id);
        if (target == nullptr) {
            return command_error(
                std::format("account del failed | account={} reason=not_attached", account_id));
        }
        // 1. 写盘
        std::string writeback_error;
        if (!commit_config_with(
                [&account_id](BarStrategyConfig& next) {
                    std::erase_if(next.accounts, [&account_id](const AttachedAccount& account) {
                        return account.account_id == account_id;
                    });
                },
                writeback_error)) {
            return writeback_failed("account del", "account", account_id, writeback_error);
        }
        // 2. 拷贝一份账户信息：表项即将被移除，而发单/上报都还要用它的 id（与 scale 无关的
        //    部分只有 id，但保留整个结构体让「同一份 AttachedAccount」的语义显式）
        const AttachedAccount removed = *target;
        // 3. 可选 sync：逐品种发 `−L_i(old)` 平仓单（此时账户仍在表内，L_i 仍可算）
        SyncOrderTally tally;
        if (sync) {
            for (auto& [instrument_id, node] : instruments_) {
                const LargeVolume old_l = node.ctx.logical_position_for(removed);
                if (old_l == 0) {
                    continue;
                }
                tally.record(node.ctx.submit_sync_order(removed, -old_l, market));
            }
        }
        // 4. 立即移除账户表项
        std::erase_if(accounts_, [&account_id](const AttachedAccount& account) {
            return account.account_id == account_id;
        });
        // 5. 通配清零上报（账户 + 空品种）
        InstrumentContext::publish_account_wildcard_zero(dz_, removed);
        std::cerr << std::format("account detached | account={} sync={} market={}", account_id,
                                 sync ? 1 : 0, market ? 1 : 0)
                  << '\n';
        return command_ok(
            with_sync_notes(std::format("account del ok | account={}", account_id), tally));
    }

    /// @brief `account list`：逐账户一行（id + scale + 各品种 `L_i`）
    CommandOutcome run_account_list() const {
        if (accounts_.empty()) {
            return command_ok("account list | -");
        }
        std::string text = std::format("account list | count={}", accounts_.size());
        for (const AttachedAccount& account : accounts_) {
            text += '\n';
            text += std::format("{} scale={}", account.account_id,
                                config_internal::format_ratio(account.scale));
            for (const auto& [instrument_id, node] : instruments_) {
                text +=
                    std::format(" {}={}", instrument_id, node.ctx.logical_position_for(account));
            }
        }
        return command_ok(std::move(text));
    }

    // ---- param get / set / list ----

    /// @brief `param get <name>`：打印当前值（名字不存在 → 错误行，不新增参数）
    CommandOutcome run_param_get(const std::string& name) const {
        if (name.empty()) {
            return command_error("invalid command | reason=empty_param_name");
        }
        if (!params_.contains(name)) {
            return command_error(std::format("param get failed | name={} reason=not_found", name));
        }
        return command_ok(std::format(
            "param {} = {}", name, params_internal::format_param_value(params_.values().at(name))));
    }

    /**
     * @brief `param set <name> <value>`：写盘成功 → 更新 → 通知已进入回调的实例（§9）
     *
     * 通知口径（§9 明文）：**已进入回调阶段的实例**当场调 `on_params_update`；还没进入的
     * **挂起**，在它的首根回调之后合并投递一次。
     *
     * 「还没进入」的现实窗口是**静默填窗期**：`on_start` 把回放同步跑完才交回引擎，所以
     * `param set` 不可能真的落在回放中途；但历史不足（冷启动）或行情清淡时，实例可能已经
     * `on_init` 完、窗口却还没填满，这期间 B 收不到任何回调——那时到来的命令必须挂起而不是
     * 丢失（`instrument add` 加进来的新品种同理）。
     *
     * 「合并投递一次」是关键：连续两次 `param set` 只在挂起位置留一个标记，首根回调后只调
     * 一次 `on_params_update`——B 的职责是「重读参数」，读一次就能拿到全部最新值，调两次
     * 会让 B 的自定义状态被重置两次（例如「参数变更时清空指标缓存」被执行两次）。
     */
    CommandOutcome run_param_set(const std::string& name, const std::string& value_token) {
        if (name.empty()) {
            return command_error("invalid command | reason=empty_param_name");
        }
        if (value_token.empty()) {
            return command_error("invalid command | reason=empty_param_value");
        }
        const nlohmann::json value = params_internal::param_value_from_token(value_token);
        // 1. 写盘：只改 `params` 对象里的一项（其余参数与未知顶层字段原样保留）
        std::string writeback_error;
        if (!commit_config_with(
                [&name, &value](BarStrategyConfig& next) { next.params[name] = value; },
                writeback_error)) {
            return writeback_failed("param set", "name", name, writeback_error);
        }
        // 2. 更新内存（写盘成功之后）
        params_.set(name, value);
        // 3. 通知：已进入回调阶段的实例当场回调；未进入的挂起，首根回调后合并投递
        dispatch_params_update();
        // 值必须**加引号**输出：`param set tag a=b` 合法（token 里允许 `=`），不加引号会打出
        // `value=a=b`，破坏 `prose | k=v` 的日志格式（「按空白切 k=v」的解析器读不回它）。
        const std::string rendered = quote_log_value(params_internal::format_param_value(value));
        std::cerr << std::format("param changed | name={} value={} instruments={}", name, rendered,
                                 instruments_.size())
                  << '\n';
        return command_ok(std::format("param set ok | name={} value={}", name, rendered));
    }

    /// @brief `param list`：逐参数一行（升序；空参数 → 单行 `param list | -`）
    CommandOutcome run_param_list() const {
        if (params_.size() == 0) {
            return command_ok("param list | -");
        }
        std::string text = std::format("param list | count={}", params_.size());
        for (const std::string& name : params_.names()) {
            text += '\n';
            text += std::format("{} = {}", name,
                                params_internal::format_param_value(params_.values().at(name)));
        }
        return command_ok(std::move(text));
    }

    // ---- status / quit ----

    /// @brief `status`：一行运行态摘要（品种数 / 账户数 / 周期 / 逻辑持仓合计 / 参数数）
    CommandOutcome run_status() const {
        LargeVolume gross_net = 0;
        std::size_t failed = 0;
        for (const auto& [instrument_id, node] : instruments_) {
            gross_net += node.ctx.net_position();
            if (node.ctx.is_failed()) {
                ++failed;
            }
        }
        return command_ok(std::format(
            "status | instrument_count={} account_count={} period_sec={} max_lookback={} "
            "buffer_bars={} net_position={} failed_instances={} param_count={} live={}",
            instruments_.size(), accounts_.size(), effective_.config.period_sec,
            effective_.max_lookback, effective_.buffer_bars, gross_net, failed, params_.size(),
            live_ ? 1 : 0));
    }

    /**
     * @brief `quit`：走引擎自主退出通道（正常码 0），`on_stop` 仍是最后一个用户回调
     *
     * 只置退出标志，**不在这里调 `on_destroy`/`on_stop`**：引擎在探测到 `should_exit()` 后
     * 自己走退出路径（`dispatch_on_stop` → `on_stop` → 逐品种 `on_destroy`）。若在这里顺手
     * 销毁实例，之后引擎的 `on_stop` 会再走一遍，`on_destroy` 被调两次。
     */
    CommandOutcome run_quit() {
        quit_requested_ = true;
        startup_exit_code_ = 0;  // 正常码（运维主动停止不是故障）
        std::cerr << std::format("strategy quit requested | instrument_count={} account_count={}",
                                 instruments_.size(), accounts_.size())
                  << '\n';
        return CommandOutcome{
            .text = std::format("quit ok | instruments={} accounts={}", instruments_.size(),
                                accounts_.size()),
            .notify = true,
            .error_level = false,
        };
    }

    // ---- 命令层共用的小工具 ----

    /// @brief 追加订阅一个品种（`instrument add` 第 2 步；`replace_previous=false` 是增补）
    ///
    /// 与启动期的 `subscribe_configured_instruments()` 的区别只在 `replace_previous` 与批量：
    /// 启动期一次订阅全部并替换旧集合，运行期只能追加——替换会退订其它品种。
    bool subscribe_one(const std::string& instrument_id) {
        if (dz_ == nullptr) {
            return true;  // 虚拟台账模式（测试缝）：无会话则无订阅可做，视为成功
        }
        const char* instruments[] = {instrument_id.c_str()};
        if (!dz_subscribe(dz_, instruments, 1, /*replace_previous=*/false)) {
            std::cerr << std::format("market data subscribe failed | instrument={} error=\"{}\"",
                                     instrument_id, dz_errmsg())
                      << '\n';
            return false;
        }
        return true;
    }

    /// @brief 退订一个品种（`instrument del`；失败只诊断，不阻断删除）
    ///
    /// 不阻断的理由：实例正在被移除，退订失败的后果只是「多收几笔已无人消费的 tick」，
    /// 而它们会被 `on_tick` 的 `live_instance()` 按「不在实例表」直接丢弃。反过来若因退订
    /// 失败而中止删除，会让一个操作员明确要求移除的品种留在进程里。
    void unsubscribe_one(const std::string& instrument_id) {
        if (dz_ == nullptr) {
            return;
        }
        const char* instruments[] = {instrument_id.c_str()};
        if (!dz_unsubscribe(dz_, instruments, 1)) {
            std::cerr << std::format("market data unsubscribe failed | instrument={} error=\"{}\"",
                                     instrument_id, dz_errmsg())
                      << '\n';
        }
    }

    /**
     * @brief 对一个刚装配好的节点调 `on_init`（`instrument add` 第 3 步；返回是否成功）
     *
     * 与 `create_instances()` 里的同款逻辑抽出来共用：两处都要「`OnInitScope` + 异常边界 +
     * 失败时丢弃节点」，抽出来避免两次实现里漏掉作用域守卫（漏掉会让 `make_series` 在之后
     * 的每次回调里都被放行）。
     *
     * 失败**不**调 `on_destroy`：实例从未成功初始化，销毁回调没有对应的创建（§4.6）。
     */
    bool init_new_instance(InstrumentNode<Strategy>& node) {
        if constexpr (!manager_internal::HasOnInit<Strategy>) {
            return true;  // 未实现 on_init：无事可做
        }
        const InstrumentContext::OnInitScope on_init_scope(node.ctx);
        try {
            node.instance.on_init(node.ctx);
            return true;
        } catch (const std::exception& e) {
            report_init_failure(node.ctx, e.what());
        } catch (...) {
            report_init_failure(node.ctx, "unknown exception");
        }
        return false;
    }

    /**
     * @brief 对单个新品种做「查历史 → 静默填窗 + 回放」（`instrument add` 第 4 步；§8）
     *
     * 复用启动期的同一个 `replay_instrument_history`：锚点（该品种本次加载历史的最早一根）、
     * 静默填窗根数、`is_final` 续接基全部由它写进节点的回放簿记，运行期与启动期因此没有第二套
     * 回放口径。历史不可用（归档域未实现）→ 冷启动，从 live 开始静默填窗（§5.3 的降级）。
     *
     * 预热成本估算不在这里打 WARN：那是**启动期**的一行预算诊断（`report_warmup_plan`），
     * 运行期 `instrument add` 的阻塞时长由操作员当场观察，重复打印只会让日志里出现两次估算。
     */
    void warmup_single_instrument(InstrumentNode<Strategy>& node) {
        if (node.ctx.is_failed()) {
            return;  // on_init 已把它判死（调用方仍会继续写盘/回滚，见调用点）
        }
        bar_strategy_internal::HistoryQueryResult result = query_history(node.ctx.instrument_id());
        if (result.status != bar_strategy_internal::HistoryStatus::Loaded) {
            const char* status = result.status == bar_strategy_internal::HistoryStatus::Failed
                                     ? "query_failed"
                                     : "source_unavailable";
            std::cerr << std::format(
                             "historical bars unavailable | instrument={} status={} "
                             "error=\"{}\" fallback=cold_start",
                             node.ctx.instrument_id(), status, result.message)
                      << '\n';
            return;
        }
        replay_instrument_history(node, result);
    }

    /// @brief 该实例各账户的 `L_i`（`instrument list` 的行尾；无账户时为空串）
    [[nodiscard]] std::string format_account_positions(const InstrumentContext& ctx) const {
        if (accounts_.empty()) {
            return {};
        }
        std::string text;
        for (const AttachedAccount& account : accounts_) {
            text += std::format(" {}={}", account.account_id, ctx.logical_position_for(account));
        }
        return text;
    }

    /**
     * @brief 校验一个挂靠比率（`account add` / `account set` 共用；`nullopt` = 合法）
     *
     * 两条命令的比率校验逐字相同（非空/正有限/`<= max_scale`），只有错误行的命令前缀不同。
     * 抽出来不是为了省几行，而是为了让「什么算合法比率」只有一处定义——`max_scale` 盘中不可变
     * （ADR 0027 决策 11），若某一处漏掉上界检查，`account set` 就能绕过启动期的上限。
     *
     * @param command_name 错误行前缀（`account add` / `account set`）
     * @param account_id   账户标识（错误行用）
     * @param scale        待校验比率
     * @return `nullopt` = 合法；有值 = 可直接返回的单行错误
     */
    [[nodiscard]] std::optional<std::string> validate_scale(std::string_view command_name,
                                                            const std::string& account_id,
                                                            double scale) const {
        if (!(scale > 0.0) || !std::isfinite(scale)) {
            return std::format("{} failed | account={} reason=invalid_scale scale={}", command_name,
                               account_id, scale);
        }
        if (scale > effective_.scaling.max_scale) {
            // max_scale 盘中不可变（ADR 0027 决策 11）：命令只能提示，不能顺带放宽上限。
            return std::format(
                "{} failed | account={} reason=scale_exceeds_max_scale scale={} max_scale={}",
                command_name, account_id, config_internal::format_ratio(scale),
                config_internal::format_ratio(effective_.scaling.max_scale));
        }
        return std::nullopt;
    }

    /// @brief 按 id 找账户表项（未找到返回 nullptr；表量级个位数，线性查找足够）
    [[nodiscard]] AttachedAccount* find_account(std::string_view account_id) noexcept {
        for (AttachedAccount& account : accounts_) {
            if (account.account_id == account_id) {
                return &account;
            }
        }
        return nullptr;
    }

    /// @brief 把 `next` 改动经 `ConfigStore::commit` 写回（失败返回 false + 错误消息）
    ///
    /// 统一入口：所有命令都以「`current().config` 的副本 + 一处改动」的形态写回，因此未知顶层
    /// 字段与原省略态（`config_file.h` 的 `apply_config`）对所有命令一致保留。写失败时
    /// `commit` 抛 `runtime_error`（内存与文件都不变），这里转成错误消息。
    template <class Mutator>
    bool commit_config_with(Mutator&& mutate, std::string& error) {
        if (!store_) {
            error = "config store unavailable";
            return false;
        }
        try {
            BarStrategyConfig next = store_->current().config;
            mutate(next);
            // commit 内部做全量校验 + 不可变字段守卫 + 原子写盘；成功后才更新内存镜像
            EffectiveConfig committed = store_->commit(next);
            // `effective_` 的**推导值**（`max_lookback` / `buffer_bars` / `scaling` / `warmup`）
            // 是启动期架构参数，运行期命令一律不改它们（`ConfigStore::commit` 也守卫了
            // `period_sec` / `max_scale`），因此只同步落盘字段：`instruments` 是 `instrument add`
            // 的「是否已在配置里」判据，`accounts` 是诊断行与重启后的依据。
            effective_.config = committed.config;
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    /// @brief 输出一行（或多行）文本到 `DZ_FRAME_OUTPUT_UI`（失败只诊断：UI 已不可达时无补救）
    void output_ui(const std::string& text) {
        if (dz_ == nullptr) {
            return;  // 虚拟台账模式 / 测试缝
        }
        // dz_output_ui 要求 NUL 结尾；超长由 SDK 按页上限截断（§7「超长按 SDK 页上限截断」）
        if (!dz_output_ui(dz_, text.c_str())) {
            std::cerr << std::format("ui output failed | bytes={} error=\"{}\"", text.size(),
                                     dz_errmsg())
                      << '\n';
        }
    }

    /// @brief 关键错误/退出通知（`dz_notify_ui`；失败只诊断）
    void notify_ui(const std::string& text, DzNotifyLevel level) {
        if (dz_ == nullptr) {
            return;
        }
        if (!dz_notify_ui(dz_, level, text.c_str(), /*popup=*/false)) {
            std::cerr << std::format("ui notify failed | bytes={} error=\"{}\"", text.size(),
                                     dz_errmsg())
                      << '\n';
        }
    }

    /// @brief 该品种在**已进入回调阶段**的实例上调 `on_params_update`（§9 的投递门禁）
    ///
    /// 两条路径共用：`param set` 的当场投递，以及首根回调之后的合并投递
    /// （`flush_pending_params_update`）。失败实例跳过（它不再接收任何 B 回调）。
    void invoke_params_update(InstrumentNode<Strategy>& node) {
        if constexpr (manager_internal::HasOnParamsUpdate<Strategy>) {
            invoke_b(node, "on_params_update", [](Strategy& instance, InstrumentContext& ctx) {
                instance.on_params_update(ctx);
            });
        }
    }

    /// @brief `param set` 之后的通知：已进入回调阶段的当场投递，其余挂起
    void dispatch_params_update() {
        for (auto& [instrument_id, node] : instruments_) {
            if (node.ctx.is_failed()) {
                continue;
            }
            if (node.ctx.request_params_update()) {
                continue;  // 还没进入回调阶段：已挂起，首根回调后由 on_callback_delivered 补投
            }
            invoke_params_update(node);
        }
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
        on_callback_delivered(node);
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
        on_callback_delivered(node);
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
        on_callback_delivered(node);
    }

    /**
     * @brief 一次 B 回调交付之后：标记「已进入回调阶段」并补投挂起的参数更新（§9）
     *
     * 门禁保证 B 的第一次回调出现在 `bar_index == max_lookback` 的那根 bar 上；在那之前
     * `param set` 只能挂起。这里在**每一次**回调之后检查一遍挂起标记，而不是只在第一次：
     * 挂起可能发生在任意时刻（`param set` 可以在静默填窗期、也可以在两个回调之间），
     * 用一个「已投递过」状态去推「不会再挂起」是错的。
     *
     * 与 B 回调的顺序：先交付本轮 bar 事件，再补参数更新——B 的 `on_params_update` 语义是
     * 「参数变了，重读」，把它插在 bar 事件中间会让同一根 bar 的部分计算用旧参数、部分用
     * 新参数。放在一次回调之后，B 看到的是「这根 bar 用旧参数算完，接下来请重读」。
     */
    void on_callback_delivered(InstrumentNode<Strategy>& node) {
        if (node.ctx.is_failed()) {
            return;  // 回调抛异常：实例已死，不再投递任何回调
        }
        // 注意：本函数**不在** `if constexpr (HasOnBarXxx)` 里面。它标记的是「**实例**已进入
        // 回调阶段」（窗口已满、框架开始交付事件），与 B 恰好实现了哪一个 bar 回调无关。
        // 放进 if constexpr 里会让「只实现 on_init + on_params_update」的策略（`BarStrategy`
        // concept 允许）永远不进入回调阶段，于是 `param set` 的挂起更新永不投递。
        if (node.ctx.note_callback_delivered()) {
            node.ctx.params_update_delivered();
            invoke_params_update(node);
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
     * @brief 取该 tick 对应的存活实例（on_tick 与排空路径共用）
     *
     * 「存活」= 未启动失败、在本实例表里、且实例未失败（§4.6）。失败/未知品种返回 nullptr，
     * 调用方据此丢弃——A 不做任何 bar 边界计算。
     *
     * @param tick 行情快照（属于本进程订阅的某个合约）
     * @return 实例指针（nullptr = 丢弃这笔 tick）
     */
    InstrumentNode<Strategy>* live_instance(const DzTick& tick) {
        if (startup_failed_) {
            return nullptr;
        }
        const auto node = instruments_.find(manager_internal::instrument_id_of(tick));
        if (node == instruments_.end() || node->second.ctx.is_failed()) {
            return nullptr;
        }
        return &node->second;
    }

    /**
     * @brief 把一笔 tick 喂给实例的 builder（on_tick 与排空路径共用）
     *
     * `current_tick_` 的挂载/清空只在这里出现一次：事件适配器据此刷新最近 tick 快照，
     * 而排空路径与 live 路径对「哪些 tick 刷新快照」的口径必须一致（§5.1）。
     *
     * @param node 实例（调用方保证存活）
     * @param tick 行情快照（只在本次调用内有效）
     */
    void feed_tick(InstrumentNode<Strategy>& node, const DzTick& tick) {
        current_tick_ = &tick;
        node.builder.on_tick(tick);
        current_tick_ = nullptr;
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
     * `on_start` 的最后一步（预热回放与缓冲 tick 续接之后），顺序是**先上报、再放开**：
     * 反过来会让首个 tick 的意图先发一笔单、再补报一个与它不一致的旧持仓。
     *
     * 分类讨论当下的状态：
     *
     *   - **有归档历史（#161 已落地）**：回放期 B 照常下单但只做虚拟成交（`live_ == false`），
     *     结束后在这里 `set_live(true)` + `publish_all_positions()` 一次性上报各账户逻辑持仓；
     *   - **冷启动（归档不可用）**：回放期没有历史可放，进入 live 时逻辑台账里只有 `on_init`
     *     阶段建立的持仓（ADR 0030 决策 1：`on_init` 期的意图同样即时记账）。因此这里必须
     *     **照常批量上报**——它可能是 0（无 `on_init` 意图），也可能不是；硬编码「启动时逻辑
     *     持仓为 0」会让 `on_init` 建仓的策略在 UI 上显示空仓。
     */
    void enter_live() {
        for (auto& [instrument_id, node] : instruments_) {
            // 顺序照 §5.3 第 4 步：**先上报、再放开**。`publish_all_positions()` 只读台账、
            // 不发单，因此它在 live 打开前后调用效果相同；但把顺序写成与文档一致，才让
            // 「之后若有人给上报加上发单/副作用，也不会先发一笔再报一个旧值」这条不变式成立。
            //
            // 失败实例也上报：§4.6 保留它的逻辑台账（不清仓），回放期可能已建仓；跳过上报
            // 会让 UI 镜像停留在旧值，与 §6「全量对账对全部账户各报一条」冲突。执行门禁保持
            // 关闭（失败实例本来就拒单，set_live 对它没有意义）。
            node.ctx.publish_all_positions();
            if (!node.ctx.is_failed()) {
                node.ctx.set_live(true);
            }
        }
        if (!accounts_.empty()) {
            std::cerr << std::format(
                             "account fanout enabled | account_count={} instrument_count={} "
                             "warmup={}",
                             accounts_.size(), instruments_.size(),
                             warmup_replayed_ ? "replayed" : "cold_start")
                      << '\n';
        }
        live_ = true;  // 命令层的 `status` 观测口径（`instrument add` 的单品种路径自己置节点）
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
                             dependencies_.session_table.session_count,
                             dependencies_.session_table.auction_count)
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
        // 合约元数据的已知限制诊断在**查到元数据之后**打印（`print_metadata_diagnostics`）：
        // 有没有元数据来源是运行期事实（platform.db 在不在），不能在配置阶段断言。
    }

    // ==== 合约元数据来源（#161 接缝 2；组件文档 §3/§8）====

    /**
     * @brief 懒开默认归档库句柄（`dz_db_open_default`：td 库按配置解析打开）
     *
     * 历史 bar 与合约元数据都在同一个 td 库里（平台库），因此只开一次、进程内复用。打开失败
     * 只记一次（`archive_db_attempted_`），不反复尝试——v1 无 platform.db 是常态，每次
     * 启动尝试一次足够；数据源恢复要重启进程（与配置加载同口径）。
     *
     * @return 库句柄；nullptr = 库不可用（调用方据此降级）
     */
    DzDatabase* archive_db() {
        if (!archive_db_attempted_) {
            archive_db_attempted_ = true;
            archive_db_ = dz_db_open_default();
        }
        return archive_db_;
    }

    /// @brief 关闭归档库句柄（`on_stop` 调用；NULL 是安全 no-op）
    void release_archive_db() noexcept {
        if (archive_db_ != nullptr) {
            (void)dz_db_close(archive_db_);
            archive_db_ = nullptr;
        }
    }

    /**
     * @brief 查一个品种的合约元数据（注入的 fake 优先，否则走默认归档实现）
     *
     * @param instrument_id 合约代码
     * @return 元数据结果（四态，见 `MetadataStatus`）
     */
    [[nodiscard]] bar_strategy_internal::MetadataResult load_metadata(
        std::string_view instrument_id) {
        if (dependencies_.metadata) {
            return dependencies_.metadata(instrument_id);
        }
        return bar_strategy_internal::load_archive_metadata(archive_db(), instrument_id);
    }

    /// @brief 合约元数据缺失/非法的诊断（该实例创建失败，进程与其它品种继续）
    void report_metadata_failure(const std::string& instrument_id,
                                 const bar_strategy_internal::MetadataResult& metadata) const {
        std::cerr << std::format(
                         "instance creation skipped | instrument={} stage=instrument_metadata "
                         "status={} detail=\"{}\"",
                         instrument_id,
                         metadata.status == bar_strategy_internal::MetadataStatus::Missing
                             ? "missing"
                             : "invalid",
                         metadata.message)
                  << '\n';
    }

    /**
     * @brief 合约元数据口径诊断（组件文档 §3/§6）
     *
     * 元数据齐全时只报一行摘要；数据源不可用（v1 无 platform.db 的常态）时打印两条**已知
     * 限制**：
     *
     *   - `position_profit()` / `contract_profit()` / 极值全线返回 0（乘数 0）；
     *   - 下单价格**不做 tick 对齐**（`price_tick == 0`，见 execution.h）。
     *
     * 必须说出来——静默返回 0 会让策略作者以为「浮盈真的是 0」把止损写成永不触发；静默不
     * 对齐会让人以为框架按 tick 处理过价格。
     */
    void print_metadata_diagnostics() const {
        if (metadata_source_unavailable_) {
            std::cerr << std::format(
                             "instrument metadata unavailable | effect=tier1_profit_zero "
                             "price_alignment_skipped=1 reason=platform_db_unavailable "
                             "instrument_count={}",
                             instruments_.size())
                      << '\n';
            return;
        }
        std::cerr << std::format("instrument metadata loaded | instrument_count={} failed={}",
                                 metadata_loaded_count_, metadata_failed_count_)
                  << '\n';
    }

    // ==== 历史预热回放 + 缓冲 tick 续接（#161；组件文档 §5.3）====

    /**
     * @brief 按预热配置估算「计划回放根数」（成本估算的输入）
     *
     * `warmup.bars` 直接用；`warmup.from` 按「日期跨度 × 每日 bar 数上限（`86400/period`）」
     * 取**上界**（真实交易日 bar 数更少，估算偏保守——启动估算宁可报大不报小）。
     *
     * @return 计划回放根数（按品种；0 = 无法估算，不打印 WARN 阈值）
     */
    [[nodiscard]] std::int64_t planned_warmup_bars() const {
        if (effective_.warmup_bars.has_value()) {
            return *effective_.warmup_bars;
        }
        if (!effective_.warmup_from.has_value()) {
            return 0;
        }
        try {
            const Date from = Date::from_string(*effective_.warmup_from);
            // `Date::days_to` 的口径是 `d - *this`：要算「从起始日到今天过了几天」必须
            // 以起始日为左操作数
            const std::int64_t days =
                static_cast<std::int64_t>(from.days_to(DateTime::local_now().date()));
            if (days <= 0) {
                return 0;
            }
            const std::int64_t bars_per_day = 86400 / effective_.config.period_sec;
            return days * bars_per_day + 1;
        } catch (const std::exception&) {
            return 0;  // 配置层已校验过；这里只做防御，估算失败不影响启动
        }
    }

    /**
     * @brief 打印回放成本估算并按阈值告警（组件文档 §5.3：启动前估算 + 两级 WARN）
     *
     * 成本 = 合成 tick × 0.1µs + 回放期 B 回调 × 0.3µs（`estimate_replay_seconds`）。阈值
     * 60s/300s 提醒运维「这次启动会卡住多久」——回放阻塞在启动阶段（期间不派发其它事件），
     * 没有估算就只能靠事后发现。
     */
    void report_warmup_plan() const {
        const std::int64_t planned = planned_warmup_bars();
        const auto instrument_count = static_cast<std::int64_t>(instruments_.size());
        const std::int64_t total_bars = planned * instrument_count;
        const std::int64_t silent_bars =
            std::min<std::int64_t>(planned, effective_.max_lookback) * instrument_count;
        const std::int64_t replay_bars = std::max<std::int64_t>(0, total_bars - silent_bars);
        const double seconds = bar_strategy_internal::estimate_replay_seconds(
            total_bars * bar_strategy_internal::REPLAY_TICKS_PER_BAR,
            replay_bars * bar_strategy_internal::REPLAY_CALLBACKS_PER_BAR);
        std::cerr << std::format(
                         "warmup replay plan | instrument_count={} warmup_bars={} silent_bars={} "
                         "replay_bars={} estimated_seconds={:.3f}",
                         instruments_.size(), planned, silent_bars, replay_bars, seconds)
                  << '\n';
        std::int32_t threshold = 0;
        if (seconds > static_cast<double>(bar_strategy_internal::REPLAY_HARD_WARN_SECONDS)) {
            threshold = bar_strategy_internal::REPLAY_HARD_WARN_SECONDS;
        } else if (seconds > static_cast<double>(bar_strategy_internal::REPLAY_SLOW_WARN_SECONDS)) {
            threshold = bar_strategy_internal::REPLAY_SLOW_WARN_SECONDS;
        }
        if (threshold > 0) {
            std::cerr << std::format(
                             "warmup replay slow | estimated_seconds={:.3f} threshold={} "
                             "effect=startup_blocked",
                             seconds, threshold)
                      << '\n';
        }
    }

    /**
     * @brief 查一个品种的历史 bar（注入的 fake 优先，否则走默认归档实现）
     *
     * @param instrument_id 合约代码
     * @return 历史查询结果（含诊断消息）
     */
    [[nodiscard]] bar_strategy_internal::HistoryQueryResult query_history(
        std::string_view instrument_id) {
        bar_strategy_internal::HistoryQuery query;
        query.instrument_id = instrument_id;
        query.period_sec = effective_.config.period_sec;
        query.max_lookback = effective_.max_lookback;
        query.warmup_bars = effective_.warmup_bars;
        if (effective_.warmup_from.has_value()) {
            try {
                query.warmup_from = static_cast<DzDate>(
                    Date::from_string(*effective_.warmup_from).days_since_epoch());
            } catch (const std::exception&) {
                // 配置层已校验；防御分支保持「无起始日期」语义（查询全部历史）
            }
        }
        if (dependencies_.history) {
            return dependencies_.history(query);
        }
        return bar_strategy_internal::query_archive_history(archive_db(), query);
    }

    /**
     * @brief 逐品种查历史、静默填窗、合成 tick 回放（组件文档 §5.3；阻塞在启动阶段）
     *
     * **逐品种顺序执行**（不是先全部查完再全部回放）：每个品种的历史在回放后立即释放，进程
     * 峰值内存只多一个品种的历史；各品种的锚点/尾部天然独立，不存在跨实例拼接。
     *
     * 回放不另写一条 tick→bar 路径：4 个合成 tick 喂进该实例**同一个** `IntradayBarBuilder`，
     * 回调门禁（§5.2）自动把前 `max_lookback` 根变成静默填窗、其余变成 B 的真实回调。因此
     * 回放事件序与 live 同构，不需要第二套事件分发。
     */
    void warmup_history_replay() {
        report_warmup_plan();
        std::int64_t replayed_bars = 0;
        std::int64_t silent_bars = 0;
        std::size_t replayed_instruments = 0;
        std::size_t degraded_instruments = 0;
        for (auto& [instrument_id, node] : instruments_) {
            if (node.ctx.is_failed()) {
                continue;
            }
            bar_strategy_internal::HistoryQueryResult result = query_history(instrument_id);
            if (result.status != bar_strategy_internal::HistoryStatus::Loaded) {
                ++degraded_instruments;
                const char* status = result.status == bar_strategy_internal::HistoryStatus::Failed
                                         ? "query_failed"
                                         : "source_unavailable";
                std::cerr << std::format(
                                 "historical bars unavailable | instrument={} status={} "
                                 "error=\"{}\" fallback=cold_start",
                                 instrument_id, status, result.message)
                          << '\n';
                continue;
            }
            ++replayed_instruments;
            replay_instrument_history(node, result);
            replayed_bars += node.replayed_bars;
            silent_bars += node.silent_bars;
        }
        std::cerr << std::format(
                         "warmup replay complete | instrument_count={} replayed_instruments={} "
                         "degraded_instruments={} silent_bars={} replayed_bars={}",
                         instruments_.size(), replayed_instruments, degraded_instruments,
                         silent_bars, replayed_bars)
                  << '\n';
    }

    /**
     * @brief 按预热深度裁剪 + 回放一个品种的历史 bar
     *
     * 顺序：按 `warmup.bars` 裁剪到最近 N 根 → 记录回放尾部（续接基准 B）→ 逐根喂 4 个
     * 合成 tick。历史 bar 的自洽性（升序、OHLC/量额自洽、`last_tick_sec` 归属本桶）是
     * **归档域的数据契约**（#4，组件文档 §5.3 已声明“缺口/拼接契约归归档域”），本票不做
     * 清洗——在数据源还是 stub 时写一套不可达的校验，等于没有测试的假防线。
     *
     * @param node   该品种节点（回放尾部与回放根数写回这里）
     * @param result 历史查询结果（`bars` 升序）
     */
    void replay_instrument_history(InstrumentNode<Strategy>& node,
                                   const bar_strategy_internal::HistoryQueryResult& result) {
        std::vector<bar::Bar> bars = result.bars;
        // 按预热深度裁剪（归档层已按查询深度返回，这里幂等兜底：`from` 模式也按日期查）
        if (effective_.warmup_bars.has_value()) {
            const auto limit = static_cast<std::size_t>(*effective_.warmup_bars);
            if (bars.size() > limit) {
                bars.erase(bars.begin(), bars.end() - static_cast<std::ptrdiff_t>(limit));
            }
        }
        if (bars.empty()) {
            std::cerr << std::format(
                             "historical bars unavailable | instrument={} status=empty "
                             "fallback=cold_start",
                             node.ctx.instrument_id())
                      << '\n';
            return;
        }
        if (!result.message.empty()) {
            // is_final 列缺失等数据源缺口：归档域落地后必须补齐（组件文档 §5.3）
            std::cerr << std::format("warmup history partial | instrument={} note=\"{}\"",
                                     node.ctx.instrument_id(), result.message)
                      << '\n';
        }

        const auto total = static_cast<std::int32_t>(bars.size());
        node.silent_bars = std::min(total, effective_.max_lookback);
        node.replayed_bars = total - node.silent_bars;
        node.replay_tail.valid = true;
        node.replay_tail.last_bar = bars.back();
        warmup_replayed_ = true;

        // 无成交的桶不生成 bar（ADR 0031）：begin_sec 跳跃是正常现象，只记 INFO 便于诊断；
        // 跨交易日的跳跃不算 gap（新交易日从自己的第一个桶开始）。
        std::int32_t gap_count = 0;
        for (std::size_t i = 1; i < bars.size(); ++i) {
            const bar::Bar& previous = bars[i - 1];
            const bar::Bar& current = bars[i];
            if (current.trading_day == previous.trading_day &&
                current.begin_sec != previous.begin_sec + previous.period_sec) {
                ++gap_count;
            }
        }
        if (gap_count > 0) {
            std::cerr << std::format("warmup history gaps | instrument={} gap_count={}",
                                     node.ctx.instrument_id(), gap_count)
                      << '\n';
        }
        if (node.silent_bars < effective_.max_lookback) {
            std::cerr << std::format(
                             "warmup history insufficient | instrument={} available_bars={} "
                             "max_lookback={} fallback=live_fill",
                             node.ctx.instrument_id(), total, effective_.max_lookback)
                      << '\n';
        }

        for (const bar::Bar& bar : bars) {
            if (node.ctx.is_failed()) {
                return;  // B 在回放期抛异常：该实例失败，停止回放（尾部保留但不收 tick）
            }
            const auto ticks = bar_strategy_internal::make_replay_ticks(bar);
            for (const DzTick& tick : ticks) {
                if (node.ctx.is_failed()) {
                    return;
                }
                node.builder.on_tick(tick);
            }
        }
    }

    /**
     * @brief 排空回放期间缓冲的 tick，按续接规则丢弃或续接（组件文档 §5.3）
     *
     * 回放阻塞在 `on_start` 里，这期间到达的 tick 全在行情通道中；进入主循环前主动排空，
     * 否则它们会被当成「新 tick」直接推进 bar，与归档尾部重叠：
     *
     *   - 最后一根**已收（final）**：丢弃目标桶 `begin_sec <= B` 的 tick——那根（及更早）已经
     *     由归档覆盖，再喂一次会把同一根 bar 重复计一次；
     *   - 最后一根**未完成（forming）**：丢弃 `begin_sec < B`、保留 `== B`——forming bar 还没
     *     收，缓冲 tick 是它的真实续接；保留后由 builder 原地 update，不产生重复 close/open。
     *
     * 「桶」的判定与 builder 共用 `bar::resolve_tick_bucket`（唯一实现），不是另一套算术。
     * 没有历史尾部的品种（冷启动/历史不可用）不参与过滤，缓冲 tick 直接当 live tick 处理。
     *
     * 排空完成后打一行汇总：丢了多少、留了多少是「续接规则是否生效」的唯一可观测证据。
     */
    void drain_buffered_ticks() {
        if (dz_ == nullptr) {
            return;
        }
        std::size_t drained = 0;
        std::size_t retained = 0;
        std::size_t dropped = 0;
        std::size_t unknown_instrument = 0;
        for (;;) {
            const void* frame = dz_next_md(dz_);
            if (frame == nullptr) {
                break;
            }
            const auto* bytes = static_cast<const std::byte*>(frame);
            if (strategy_engine_internal::frame_header(bytes).frame_type != DZ_FRAME_TICK) {
                continue;  // 行情通道里的非 tick 帧忽略（引擎侧同口径）
            }
            if (!strategy_engine_internal::payload_size_matches<DzTick>(bytes)) {
                continue;  // 布局错配由引擎侧诊断；这里丢弃不可解析的帧
            }
            ++drained;
            const DzTick& tick = strategy_engine_internal::frame_payload<DzTick>(bytes);
            InstrumentNode<Strategy>* node = live_instance(tick);
            if (node == nullptr) {
                // 未订阅/失败实例：与会话无关的行情帧，不计入「续接」的 retain/drop 口径
                ++unknown_instrument;
                continue;
            }
            if (should_drop_continuation_tick(*node, tick)) {
                ++dropped;
                continue;
            }
            ++retained;
            feed_tick(*node, tick);
        }
        if (drained > 0) {
            std::cerr << std::format(
                             "warmup continuation | drained={} retained={} dropped={} "
                             "unknown_instrument={}",
                             drained, retained, dropped, unknown_instrument)
                      << '\n';
        }
    }

    /**
     * @brief 续接过滤：这笔缓冲 tick 是否应丢弃（组件文档 §5.3 的两条规则）
     *
     * 判定顺序：早于尾部交易日 → 丢；更晚交易日 → 留；同交易日按 `resolve_tick_bucket` 解析
     * 目标桶后比较 `begin_sec`：
     *
     *   - 目标桶 < B：丢（归档已覆盖更早的 bar）；
     *   - 目标桶 > B：留（新 bar 的 tick，由 builder 先 close 尾部 bar 再开新根）；
     *   - 目标桶 == B：**最后一根已收 → 丢**、**未完成 → 留**（原地续接 forming bar）。
     *
     * 非交易时段/时间越界的 tick 交给 builder 丢弃并计数（它本来就会做，且计数是运维口径），
     * 这里不重复判定。
     *
     * @param node 该品种节点
     * @param tick 缓冲 tick
     * @return true = 丢弃
     */
    [[nodiscard]] bool should_drop_continuation_tick(const InstrumentNode<Strategy>& node,
                                                     const DzTick& tick) const {
        if (!node.replay_tail.valid) {
            return false;  // 冷启动：没有归档尾部可重叠，按 live tick 处理
        }
        const bar::Bar& tail = node.replay_tail.last_bar;
        if (tick.date < tail.trading_day) {
            return true;
        }
        if (tick.date > tail.trading_day) {
            return false;
        }
        if (tick.time < 0 || tick.time > bar::MAX_TRADING_SEC) {
            return false;  // 非法时间：builder 计 out_of_session，这里不越俎代庖
        }
        const bar::TickBucket bucket =
            bar::resolve_tick_bucket(bar::TradingTime::from_clock(tick.time),
                                     dependencies_.session_table, effective_.config.period_sec);
        if (!bucket.in_session) {
            return false;  // 非交易时段：builder 丢弃并计数
        }
        if (bucket.begin_sec < tail.begin_sec) {
            return true;
        }
        if (bucket.begin_sec > tail.begin_sec) {
            return false;
        }
        // 同桶：最后一根已收 → 丢（归档已覆盖）；未完成 → 留（原地续接 forming bar）
        return tail.is_final;
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
     * @brief 逐品种查元数据、装配 `InstrumentNode` 并调 `on_init`（组件文档 §8 启动第 3 步）
     *
     * 元数据先于实例：`InstrumentSpec` 是 ctx 的构造参数（Tier 1 盈亏与下单价格对齐都读它）。
     * 三种结局：
     *
     *   - `Loaded` → 用元数据构造实例；
     *   - `SourceUnavailable` → 部署级降级：零元数据 + 启动诊断声明两条已知限制（先纸上跑通）；
     *   - `Missing` / `Invalid` → **该实例创建失败**（打印诊断、跳过该品种，进程与其它品种
     *     继续）——静默用 0 乘数会让浮动盈亏恒为 0、止损永不触发，比不启动更危险。
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
            InstrumentSpec spec;
            bool metadata_loaded = false;
            const bar_strategy_internal::MetadataResult metadata = load_metadata(instrument_id);
            switch (metadata.status) {
                case bar_strategy_internal::MetadataStatus::Loaded:
                    spec = metadata.spec;
                    metadata_loaded = true;
                    break;
                case bar_strategy_internal::MetadataStatus::SourceUnavailable:
                    // 数据源缺失是**部署级**状态：所有品种一起降级，不是单品种配置错误
                    metadata_source_unavailable_ = true;
                    break;
                case bar_strategy_internal::MetadataStatus::Missing:
                case bar_strategy_internal::MetadataStatus::Invalid:
                    ++metadata_failed_count_;
                    report_metadata_failure(instrument_id, metadata);
                    continue;
            }
            InstrumentInit init{
                .instrument_id = instrument_id,
                .max_lookback = effective_.max_lookback,
                .buffer_bars = effective_.buffer_bars,
                .spec = spec,
                .dz = dz_,
                .accounts = &accounts_,
                .params = &params_,
                .scaling = effective_.scaling,
            };
            auto [slot, inserted] = instruments_.try_emplace(
                instrument_id, *this, init, dependencies_.session_table, period_sec);
            if (!inserted) {
                continue;  // 配置层已保证 instruments 不重复
            }
            // make_series 只在 on_init 内合法；作用域守卫与异常边界在 init_new_instance 里
            // 与 `instrument add` 共用同一份实现（漏掉守卫会让 make_series 永久放行）。
            if (!init_new_instance(slot->second)) {
                instruments_.erase(slot);
            }
            if (metadata_loaded && instruments_.contains(instrument_id)) {
                // 只统计「实例真的建成」的品种：on_init 抛异常时该品种已从 map 移除
                ++metadata_loaded_count_;
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
    EffectiveConfig effective_;  ///< 启动快照（构建实例用，进程内不变）
    /// 三个内部依赖缝 + 回卷时刻（#161；测试经它注入 fake 历史/元数据，§5.3）
    /// 小节表/回卷时刻/数据源**只在这里保存一份**——派生副本（`session_table_` 等）会让两个
    /// 状态有机会互相矛盾，而诊断与续接过滤都读这张表。
    bar_strategy_internal::FrameworkDependencies dependencies_;
    bool degraded_ = true;  ///< 由依赖聚合的小节表判定（诊断口径，§5.5）
    /// 默认归档库句柄（懒开：`dz_db_open_default`；`on_stop` 关闭）。历史与元数据共用。
    DzDatabase* archive_db_ = nullptr;
    bool archive_db_attempted_ = false;         ///< 已尝试打开（失败也只试一次）
    bool metadata_source_unavailable_ = false;  ///< 元数据源不可用（部署级降级口径）
    std::size_t metadata_loaded_count_ = 0;     ///< 元数据齐全的实例数（诊断）
    std::size_t metadata_failed_count_ = 0;     ///< 元数据缺失/非法而跳过的实例数（诊断）
    bool warmup_replayed_ = false;              ///< 至少一个品种完成历史回放（诊断口径）
    /// 进程级参数仓（启动读配置的 `params`；#162 起由 `param set` 改写）。**必须在
    /// instruments_ 之前声明**：节点里的 ctx 持它的地址，析构顺序保证它比节点活得久。
    ParamStore params_;
    /// 挂靠账户表（启动读配置；#162 起由命令增删改）。**必须在 instruments_ 之前声明**：
    /// 节点里的 ctx 持它的地址，析构顺序保证表比节点活得久。
    AccountTable accounts_;
    /// 本次 `on_tick` 正在分发的 tick（只在分发期内非空；事件入口据此刷新 ctx 快照）
    const DzTick* current_tick_ = nullptr;
    bool startup_failed_ = false;         ///< 启动失败：请求引擎立即退出
    std::int32_t startup_exit_code_ = 0;  ///< 启动失败时的进程退出码（`quit` 把它置 0）
    bool quit_requested_ = false;         ///< 收到 `quit` 命令：请求引擎自主退出（正常码）
    bool live_ = false;                   ///< 已进入 live（`status` 的观测口径）
    // 最后声明 → 最先析构：节点不会看到已销毁的配置/会话
    std::map<std::string, InstrumentNode<Strategy>, std::less<>> instruments_;  ///< 地址稳定
};

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_MANAGER_H_
