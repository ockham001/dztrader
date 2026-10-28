/**
 * @file context.h
 * @brief 品种上下文：品种数据 + B 的只读 API 面（ADR 0027 决策 5，组件文档 §3）
 *
 * 每品种一个 `InstrumentContext`，由 `BarStrategyManager` 在稳定的 map 节点内就地构造
 * （`InstrumentNode`）: 其它品种增删不会搬移本节点，B 持有的价格视图因此在实例存活期内
 * 始终有效。
 *
 * 窗口几何、价格数组与用户序列数组的所有权都在 ctx 内：`window_` 是价格序列与用户序列共用
 * 的窗口几何，`price_` 是 7 条定长物理数组，`series_d_` / `series_i_` 是 B 在 `on_init` 里
 * 注册的用户序列，全部容量在构造/注册时一次分配、永不扩容。推进与回卷由 A 经
 * `advance_bar()` / `relocate()` 驱动（内部头 `dztrader/series_internal.h` 的
 * `commit_advance()` 与 `relocate_array()` / `finish_relocate()`，顺序：回卷 → 写槽位 →
 * `++end`）。
 *
 * **B 可见面**只有 public 区：7 条只读价格视图、`make_series()`、`bar_index()`、
 * `instrument_id()`、四个交易意图（#159）、Tier 1 查询（#159）。窗口推进、回卷、失败标记
 * 等运行期状态是 private，仅 `BarStrategyManager` 与 `InstrumentNode` 可访问（friend），
 * 热路径上没有虚函数。
 *
 * **交易意图与台账（#159）**：四个意图函数在 ctx 内完成「开平规则 → 虚拟成交 → Tier 1」的
 * 单一漏斗（ADR 0030 决策 1），不等待成交回报、不持有账户表。
 *
 * **执行与上报（#160）**：同一漏斗在台账推进之后接上「挂靠缩放 → 每账户差量单 → 逻辑持仓
 * 上报」：账户 `i` 的 `qty_i = L_i(P_new) − L_i(P_old)`，`L_i = sign(P) × f(scale_i × |P|)`
 * （`scaling.h` 的唯一实现），价格按 tick 对齐 + 涨跌停夹取（`execution.h` 的唯一实现）。
 * 0 个挂靠账户时同样只记逻辑账——这正是「先纸上跑通策略」的口径。
 *
 * 台账语义在 `dztrader/bar_strategy/position.h`（纯状态与纯转移，可独立单测），缩放与价格
 * 在 `scaling.h` / `execution.h`（同样是纯函数），本头只负责把 B 的调用翻成一次记账 + 一次
 * fanout + 分级诊断。**下单与上报各只有一个出口**（`submit_order` / `publish_position`）：
 * 正常单、删除平仓、回放后的批量上报全部走它们，框架不出现第二条拼帧路径。
 *
 * 本头是 SDK 公开头（header-only 框架的一部分）：只依赖标准库与 SDK 公开头，不依赖
 * 任何内部库或日志库（ADR 0020/0021）。
 */
#ifndef DZTRADER_BAR_STRATEGY_CONTEXT_H_
#define DZTRADER_BAR_STRATEGY_CONTEXT_H_

#include <dztrader/api.h>
#include <dztrader/bar/types.h>
#include <dztrader/bar_strategy/execution.h>
#include <dztrader/bar_strategy/position.h>
#include <dztrader/bar_strategy/scaling.h>
#include <dztrader/series.h>
#include <dztrader/series_internal.h>

#include <concepts>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dztrader {

template <class Strategy>
class BarStrategyManager;  // 前向：A 通过 friend 驱动窗口推进与失败标记

template <class Strategy>
struct InstrumentNode;  // 前向：节点就地构造 ctx（传 InstrumentInit）

namespace context_internal {

/// 前向：ctx 的内部构造缝。**定义在内部头** `dztrader/bar_strategy/context_internal.h`
/// （与 `series.h` 声明 `series_internal::Access`、`series_internal.h` 定义它的先例一致），
/// 因此本公开头只多一行 friend 声明，不把测试缝的成员函数摆进公开承诺面。
struct ContextAccess;

}  // namespace context_internal

/**
 * @brief 7 条价格序列的物理数组（A 内部；B 只经只读视图读）
 *
 * 每条数组物理长度都是 `buffer_bars = max_lookback + reserve_bars`，实例创建时一次分配、
 * 之后永不 realloc——B 持有的视图全程指向同一块内存（回卷只搬内容、不换地址）。
 */
struct PriceArrays {
    std::unique_ptr<double[]> open;           ///< 开盘价
    std::unique_ptr<double[]> high;           ///< 最高价
    std::unique_ptr<double[]> low;            ///< 最低价
    std::unique_ptr<double[]> close;          ///< 收盘价
    std::unique_ptr<double[]> volume;         ///< 成交量
    std::unique_ptr<double[]> turnover;       ///< 成交额
    std::unique_ptr<double[]> open_interest;  ///< 持仓量
};

/**
 * @brief `InstrumentContext` 构造参数聚合（按依赖分组，避免位置参数传错）
 *
 * 生命周期二分（组件文档 §3）：值语义项（`spec` / `scaling`）按值拷贝进 ctx；指针语义项
 * （`dz` / `accounts`）指向 A 持有的对象，A 保证其在 ctx 之后析构（`instruments_` 是 A 的
 * 最后一个成员）。
 *
 * `dz == nullptr` 表示「脱离引擎的虚拟台账模式」：`on_init` 期与单元测试用，此时不发单、
 * 不上报（没有会话句柄可写）。这也是 `context_internal::ContextAccess` 这条测试缝的形状。
 */
struct InstrumentInit {
    std::string instrument_id;               ///< 合约代码（平台唯一键）
    std::int32_t max_lookback = 0;           ///< 可读历史根数上限（= 配置 max_lookback）
    std::int32_t buffer_bars = 0;            ///< 物理数组长度（= max_lookback + reserve_bars）
    InstrumentSpec spec;                     ///< 合约属性（乘数供 Tier 1、`price_tick` 供价格对齐）
    DzContext* dz = nullptr;                 ///< 引擎会话句柄（nullptr = 虚拟台账，不发单不上报）
    const AccountTable* accounts = nullptr;  ///< A 的挂靠账户表（只读；nullptr = 0 个账户）
    ScalingConfig scaling;                   ///< 缩放配置副本（启动后不可变，按值持有）
};

/**
 * @brief 一条用户序列的所有权记录（数组 + 声明初值；#158）
 *
 * B 在 `on_init` 里经 `ctx.make_series<T>(init)` 注册，所有权留在 `InstrumentContext`
 * （`series_d_` / `series_i_` 容器），B 拿到的是指向 `base` 的 `Series<T>` 视图。数组在
 * 注册时按 `buffer_bars` 一次分配、永不 realloc，因此视图地址在实例存活期内稳定。
 *
 * `init` 是静默填窗阶段框架逐根写入的声明初值（carry 语义起点，组件文档 §4.2）；其后每根
 * 新 bar 复制上一根的 `[0]`。本类型只在 ctx 内部使用，不进 B 可见面。
 *
 * @tparam T 元素类型（`double` 或 `std::int64_t`）
 */
template <class T>
struct SeriesStorage {
    std::unique_ptr<T[]> base;  ///< 物理 buffer_bars 个元素
    T init{};                   ///< 声明初值（静默填窗逐根写入）
};

/**
 * @brief 用户序列的注册类型约束（组件文档 §4.1「创建」：只支持 `double` / `int64_t`）
 *
 * 两种元素类型各有一份独立数组容器（`series_d_` / `series_i_`），因此类型是**编译期**约束
 * 而不是运行期校验：其它类型在 `make_series` 的签名处直接编译失败，报错指向调用点。
 */
template <class T>
concept UserSeriesElement = std::same_as<T, double> || std::same_as<T, std::int64_t>;

/**
 * @brief 每品种一个：品种数据 + B 的 API 面合并为同一对象（组件文档 §3）
 *
 * 公开面（B 可见）：7 条只读价格视图、`make_series()`、`bar_index()`、`instrument_id()`。
 * 回调参数统一是 `InstrumentContext&`，B 不接触引擎回调、不持有 A 指针。
 *
 * 视图在构造时绑定自身 `window_` / `price_`，之后永不重绑；`WindowState` 一推进，全部视图
 * 同时看到新 bar。**声明顺序即构造顺序**：`window_` / `price_` 必须先于视图成员声明，
 * 否则视图会绑定到尚未构造完成的数组。
 */
class InstrumentContext {
private:
    WindowState window_;  ///< 价格序列与用户序列共用的窗口几何
    PriceArrays price_;   ///< 7 条价格数组（物理 `buffer_bars` 个元素）

public:
    // ==== B 可见面：只读价格视图（绑定自身 window_ / price_，构造后永不重绑）====
    SeriesView<double> open;           ///< `[0]` 当前 bar、`[n]` 往回 n，越界抛异常
    SeriesView<double> high;           ///< 最高价序列
    SeriesView<double> low;            ///< 最低价序列
    SeriesView<double> close;          ///< 收盘价序列
    SeriesView<double> volume;         ///< 成交量序列
    SeriesView<double> turnover;       ///< 成交额序列
    SeriesView<double> open_interest;  ///< 持仓量序列

    /**
     * @brief 当前 bar 的绝对序号（0 起、单调；classic TB `CurrentBar` 同口径）
     *
     * `-1` = 尚未有 bar（`on_init` 期间恒为 -1；静默填窗期 B 不被回调，故 B 永远看不到
     * `0 … max_lookback-1` 这些索引，第一个可见值是 `max_lookback`——组件文档 §5.2）。
     */
    [[nodiscard]] std::int64_t bar_index() const noexcept { return bar_index_; }

    /// @brief 合约代码（平台唯一键）
    [[nodiscard]] const std::string& instrument_id() const noexcept { return instrument_id_; }

    /**
     * @brief 注册一条用户序列（组件文档 §4.1「创建」；仅 `on_init` 可调用）
     *
     * 注册发生在实例创建期（`on_init` 里），框架按 `buffer_bars` 一次分配、永不 realloc，
     * 因此返回的 `Series<T>` 在实例存活期内始终有效——B 可以把它存进自己的成员，之后每根
     * bar 直接 `my_series[0] = v`。
     *
     * **静默填窗期（回调门禁生效前）框架逐根写入 `init`**，这是 carry 语义的起点；进入回调期
     * 后每根新 bar 复制上一根的 `[0]`。因此第一个回调到来时，B 的序列已经和价格序列一样填满
     * `max_lookback` 根，历史位全是 `init`——不需要读时兜底，也不会拿到 NaN。
     *
     * 非 `on_init` 调用是用法错误：静默填窗已按当时的注册集合跑过，中途新增的序列无法补写
     * 历史初值，只会得到一条「前 N 根是未初始化内存」的序列。这里**响亮失败**（抛
     * `std::logic_error`），而不是默默返回一条语义残缺的序列。
     *
     * @tparam T 元素类型（`double` 或 `std::int64_t`）
     * @param init 静默填窗阶段逐根写入的声明初值
     * @return 可写序列（`[0]` 可写；历史位只读，越界抛 `std::out_of_range`）
     * @throw std::logic_error 在非 `on_init` 阶段调用
     */
    template <UserSeriesElement T>
    Series<T> make_series(T init) {
        // 用法错误（错在调用时机），不是 §4.2 的读越界，因此**不**用
        // DZTRADER_SERIES_ASSERT：那个宏的契约是「抛 std::out_of_range 前先断言」，只在
        // series 层测试里被关闭；本函数所在的框架测试不关它，Debug/ASan 下断言会把
        // std::logic_error 变成 abort——「清晰失败」就变成了进程直接死在测试里。
        if (!in_on_init_) [[unlikely]] {
            throw std::logic_error(
                std::format("make_series must be called from on_init | instrument={} bar_index={}",
                            instrument_id_, bar_index_));
        }
        SeriesStorage<T>& storage = series_storage<T>().emplace_back();
        storage.base =
            std::make_unique_for_overwrite<T[]>(static_cast<std::size_t>(window_.buffer_bars));
        storage.init = init;
        return series_internal::Access::series(storage.base.get(), &window_);
    }

    // ==== 交易意图（v1 仅限价；返回 void，B 不感知执行结果；组件文档 §6）====
    //
    // 四个方法是**开平意图**，不是方向别名（与 TB/MC 默认图表信号一致）：
    //   - `buy` / `sell_short`：反向时先平对手仓、再按 `volume` 建仓（目标化 `P = ±n`）；
    //   - `sell` / `buy_to_cover`：只减不增，`volume` 超过逻辑持仓时钳制到全平；
    //   - `volume == 0` 是**模式值**而非数量（只平不建 / 全平）：买方向平空、卖方向平多，
    //     因此 `buy(0) ≡ buy_to_cover(0)`、`sell(0) ≡ sell_short(0)`。
    //
    // 每笔意图**即时**记账到本品种的逻辑持仓与 Tier 1（ADR 0030 决策 1）：不等待成交回报、
    // 不跟踪在途、不建成交台账，所以同一个回调里 `buy()` 之后立刻 `market_position()` 就能
    // 看到结果。挂靠账户的手数缩放与下单不在本票（#160）：v1 这里只推进虚拟台账。

    // 注：四个方法的参数名用 `strategy_volume` 而不是 `volume`——`InstrumentContext` 自己有
    // 一个 `volume` 成交量序列成员，同名参数会触发 MSVC C4458（/W4 /WX 下是硬错误，Linux 的
    // 告警集拦不到）。`strategy_volume` 也正好是组件文档 §6 的说法（「策略手数」，与挂靠
    // 账户的每账户手数区分）。

    /// @brief 买方向开/加多；反向（持有空头）时先平全部空再开 `strategy_volume` 多
    void buy(Volume strategy_volume, double price) {
        execute_intent(position_internal::Intent::Buy, strategy_volume, price);
    }

    /// @brief 卖方向平多（只减不增；超出逻辑持仓钳制到全平）
    void sell(Volume strategy_volume, double price) {
        execute_intent(position_internal::Intent::Sell, strategy_volume, price);
    }

    /// @brief 卖方向开/加空；反向（持有多头）时先平全部多再开 `strategy_volume` 空
    void sell_short(Volume strategy_volume, double price) {
        execute_intent(position_internal::Intent::SellShort, strategy_volume, price);
    }

    /// @brief 买方向平空（只减不增；超出逻辑持仓钳制到全平）
    void buy_to_cover(Volume strategy_volume, double price) {
        execute_intent(position_internal::Intent::BuyToCover, strategy_volume, price);
    }

    // ==== Tier 1 持仓/进出场查询（语义与口径见组件文档 §6.1；只读）====

    /// @brief 净持仓方向：-1（空）/ 0（无仓）/ 1（多）；由净持仓符号派生
    [[nodiscard]] std::int32_t market_position() const noexcept {
        return position_.market_position();
    }

    /// @brief 持仓手数（≥ 0 的绝对值；方向看 `market_position()`，TB 同口径）
    [[nodiscard]] LargeVolume current_contracts() const noexcept {
        return position_internal::abs_volume(position_.net_volume);
    }

    /// @brief 当前持仓加权平均建仓价（部分平仓不变、反手重置；无仓 0）
    [[nodiscard]] double avg_entry_price() const noexcept { return position_.avg_entry_price; }

    /// @brief 当前持仓建仓段数（每次开仓/加仓 +1，反手新开 +1，部分平仓不变；无仓 0）
    [[nodiscard]] std::int32_t current_entries() const noexcept {
        return position_.current_entries;
    }

    /// @brief 本实例历史最大持仓手数（`|P|` 峰值，≥ 0，永不回落）
    [[nodiscard]] LargeVolume max_contracts() const noexcept { return position_.max_contracts; }

    /// @brief 本实例历史最大建仓段数（≥ 0，永不回落）
    [[nodiscard]] std::int32_t max_entries() const noexcept { return position_.max_entries; }

    /// @brief 当前持仓第一笔建仓的成交价（价格为传入限价）
    [[nodiscard]] double entry_price() const noexcept { return position_.entry_price; }

    /// @brief 当前持仓第一笔建仓的日期（对应 bar 的交易日）
    [[nodiscard]] DzDate entry_date() const noexcept { return position_.entry_date; }

    /// @brief 当前持仓第一笔建仓的时间（对应 bar 的标准结束时间）
    [[nodiscard]] DzTime entry_time() const noexcept { return position_.entry_time; }

    /// @brief 最近一笔加仓的成交价（无持仓时保留最近一次建仓值，与 TB 同口径）
    [[nodiscard]] double last_entry_price() const noexcept { return position_.last_entry_price; }

    /// @brief 最近一笔加仓的日期
    [[nodiscard]] DzDate last_entry_date() const noexcept { return position_.last_entry_date; }

    /// @brief 最近一笔加仓的时间
    [[nodiscard]] DzTime last_entry_time() const noexcept { return position_.last_entry_time; }

    /// @brief 最近一笔平仓的成交价（含部分平仓与反手平仓段）
    [[nodiscard]] double exit_price() const noexcept { return position_.exit_price; }

    /// @brief 最近一笔平仓的日期
    [[nodiscard]] DzDate exit_date() const noexcept { return position_.exit_date; }

    /// @brief 最近一笔平仓的时间
    [[nodiscard]] DzTime exit_time() const noexcept { return position_.exit_time; }

    /// @brief 距首次建仓的 bar 数（同 bar = 0；当前无持仓 = -1）
    [[nodiscard]] std::int64_t bars_since_entry() const noexcept {
        return bars_since(position_.entry_bar);
    }

    /// @brief 距最近加仓的 bar 数（同 bar = 0；当前无持仓 = -1）
    [[nodiscard]] std::int64_t bars_since_last_entry() const noexcept {
        return bars_since(position_.last_entry_bar);
    }

    /// @brief 距最近平仓的 bar 数（同 bar = 0；从未平仓 = -1）
    [[nodiscard]] std::int64_t bars_since_exit() const noexcept {
        return bars_since(position_.exit_bar);
    }

    /// @brief 浮动盈亏（钱）：`(最新价 − 均价) × P × volume_multiple`；无仓 0
    [[nodiscard]] double position_profit() const noexcept {
        return position_profit_at(current_price());
    }

    /// @brief 每手浮动盈亏（钱）：`(最新价 − 均价) × sign(P) × volume_multiple`；无仓 0
    ///
    /// 按 §6.1 的公式**直接乘**，不由 `position_profit()` 除以手数得出：除法会引入一次多余的
    /// 浮点舍入（均价是加权平均值，通常不是二进制可表示的数），使每手盈亏与文档公式在末位
    /// 上不一致——而用户拿它做止损阈值比较时，末位正是容易被踩到的地方。
    [[nodiscard]] double contract_profit() const noexcept {
        const double direction = position_.net_volume > 0 ? 1.0 : -1.0;
        return profit_at(current_price(), direction);
    }

    /// @brief 当前持仓期间浮盈极值（≥ 0；bar 收盘前更新，开仓重置、平仓归零）
    [[nodiscard]] double max_position_profit() const noexcept {
        return position_.max_position_profit;
    }

    /// @brief 当前持仓期间浮亏极值（≤ 0；口径同上）
    [[nodiscard]] double max_position_loss() const noexcept { return position_.max_position_loss; }

    InstrumentContext(const InstrumentContext&) = delete;
    InstrumentContext& operator=(const InstrumentContext&) = delete;
    InstrumentContext(InstrumentContext&&) = delete;
    InstrumentContext& operator=(InstrumentContext&&) = delete;

private:
    template <class>
    friend class BarStrategyManager;  // A 通过 friend 驱动推进 / 标记失败
    template <class>
    friend struct InstrumentNode;  // 节点就地构造（传 InstrumentInit）

    friend struct context_internal::ContextAccess;  // 内部构造缝（#159 单元测试）

    /**
     * @brief A 内部构造：按 `buffer_bars` 一次分配 7 条价格数组并绑定视图
     * @param init 构造参数（会话句柄 / 合约代码 / 窗口容量）
     */
    explicit InstrumentContext(const InstrumentInit& init)
        : window_{.end = 0,
                  .size = 0,
                  .max_lookback = init.max_lookback,
                  .buffer_bars = init.buffer_bars,
                  .advance_seq = 0},
          price_{
              .open = make_array(init.buffer_bars),
              .high = make_array(init.buffer_bars),
              .low = make_array(init.buffer_bars),
              .close = make_array(init.buffer_bars),
              .volume = make_array(init.buffer_bars),
              .turnover = make_array(init.buffer_bars),
              .open_interest = make_array(init.buffer_bars),
          },
          open(series_internal::Access::view(price_.open.get(), &window_)),
          high(series_internal::Access::view(price_.high.get(), &window_)),
          low(series_internal::Access::view(price_.low.get(), &window_)),
          close(series_internal::Access::view(price_.close.get(), &window_)),
          volume(series_internal::Access::view(price_.volume.get(), &window_)),
          turnover(series_internal::Access::view(price_.turnover.get(), &window_)),
          open_interest(series_internal::Access::view(price_.open_interest.get(), &window_)),
          instrument_id_(init.instrument_id),
          spec_(init.spec),
          dz_(init.dz),
          accounts_(init.accounts),
          scaling_(init.scaling) {}

    /// @brief 分配一条价格数组（`buffer_bars` 个元素；首次写入前不读，无需零初始化）
    [[nodiscard]] static std::unique_ptr<double[]> make_array(std::int32_t buffer_bars) {
        return std::make_unique_for_overwrite<double[]>(static_cast<std::size_t>(buffer_bars));
    }

    // ==== 交易意图与 Tier 1 的内部面（#159；只有 ctx 自己与 A 用）====

    /**
     * @brief 一笔交易意图的单一漏斗：拒单门禁 → 虚拟成交记账 → 挂靠 fanout → 分级诊断
     *
     * B 的四个意图方法都走到这里，因此「哪条路径都不许绕过门禁」是结构性保证，而不是
     * 四个方法各写一遍的纪律。
     *
     * 门禁只有一条：**失败实例不再受理下单意图**（§4.6）。执行门禁 `live_`（回放/静默填窗
     * 只做虚拟成交）不拦意图——虚拟台账口径下每笔意图都照常推进 P 与 Tier 1，`live_` 决定的是
     * 「要不要落地成实盘单与上报」，判断在 {@link fanout_positions} 里。
     *
     * @param intent          意图种类
     * @param strategy_volume 策略手数（负数会被拒）
     * @param price           传入限价（虚拟成交价；实盘单的对齐/夹取在 fanout 里做）
     */
    void execute_intent(position_internal::Intent intent, Volume strategy_volume, double price) {
        if (failed_) [[unlikely]] {
            // 失败实例拒绝一切意图；但 `volume == 0` 且无可平仓是「无操作」，与 §6 同一条
            // 理由（平仓语句逐 bar 无条件调用）——不用为它刷一行拒单诊断。
            if (!is_zero_volume_noop(intent, strategy_volume)) {
                report_intent_rejected(intent, "instance_failed");
            }
            return;
        }
        const LargeVolume net_before = position_.net_volume;
        const position_internal::LedgerOutcome outcome =
            position_internal::apply_intent(position_, intent, strategy_volume, price, bar_stamp());
        if (outcome.negative_volume) [[unlikely]] {
            report_intent_rejected(intent, "negative_volume");
            return;
        }
        if (outcome.invalid_price) [[unlikely]] {
            report_intent_rejected(intent, "invalid_price");
            return;
        }
        if (outcome.applied()) {
            fanout_positions(net_before, price);
        }
        report_intent_outcome(intent, strategy_volume, outcome);
    }

    /**
     * @brief 逻辑持仓变化后对每个挂靠账户发差量单并上报（组件文档 §6「执行」/「上报」）
     *
     * 这是**唯一**的 per-account 出口：正常策略单、反手、全平都走到这里，因此「反手只发一张
     * 差量单」「`qty_i == 0` 跳过发单与上报」「effect 恒 AUTO」「上报只在 `L_i` 变化时」这四条
     * 是一处实现，不是四条路径各自的纪律。
     *
     * 差量而不是比例：`qty_i = L_i(P_new) − L_i(P_old)`，取整落在**目标持仓**上，因此取整
     * 误差不随逐单累积（ADR 0030 决策 8）。方向取差量符号，effect 恒 `AUTO`（开平/锁仓由 td
     * 按账户解析，issue #82）：框架不推导 effect、不拆平开、不基于实际持仓吸附。
     *
     * **执行门禁**：`live_ == false`（回放/静默填窗期）或 `dz_ == nullptr`（虚拟台账模式）时
     * 只记逻辑账——不发单、不上报。回放结束后的批量上报由 A 经
     * {@link publish_all_positions} 触发（§5.3）。
     *
     * @param net_before 本次意图记账前的逻辑持仓 P_old
     * @param price      策略传入的限价（发单价格的原始值，对齐/夹取在这里做）
     */
    void fanout_positions(LargeVolume net_before, double price) {
        const LargeVolume net_after = position_.net_volume;
        if (net_after == net_before) {
            return;  // 逻辑持仓没变：既不发单也不上报
        }
        if (!live_ || dz_ == nullptr) {
            return;  // 虚拟台账模式：只记账
        }
        if (accounts_ == nullptr) {
            return;  // 0 个挂靠账户：只记逻辑账（「先纸上跑通策略」口径）
        }
        for (const AttachedAccount& account : *accounts_) {
            const LargeVolume old_l =
                scaling_internal::logical_position(net_before, account.scale, scaling_);
            const LargeVolume new_l =
                scaling_internal::logical_position(net_after, account.scale, scaling_);
            const LargeVolume delta = saturating_delta(new_l, old_l);
            if (delta == 0) {
                continue;  // 该账户的 L_i 未变：不发单、不上报
            }
            submit_order(account, delta, price);
            publish_position(account, new_l);
        }
    }

    /**
     * @brief **唯一的** `dz_place_order` 出口（组件文档 §3「唯一出口」）
     *
     * 正常策略单、删除平仓、同步单（#162）共用本函数：价格处理、effect 固定值、失败诊断都
     * 只写一遍。A 不参与下单，因此不存在第二条拼单路径。
     *
     * 拒单有两种，都不影响逻辑账（逻辑持仓已按意图/命令推进）：
     *   - 价格非法（NaN/Inf/≤ 0）：打 `order rejected`；
     *   - 数量超出 `DzVolume` 范围：打 `order rejected`（静默截断会发一张手数完全不同的单）。
     * 写帧失败（`dz_place_order < 0`）也记诊断——它是「单没出去」的唯一证据。
     *
     * @param account 目标挂靠账户
     * @param delta   该账户的差量手数（正 = 买、负 = 卖；调用方保证非 0）
     * @param price   策略传入的限价（对齐/夹取在本函数内完成）
     */
    void submit_order(const AttachedAccount& account, LargeVolume delta, double price) {
        const Volume order_qty = order_volume(delta).value_or(0);
        const DzDirection direction = direction_of(delta);
        if (order_qty == 0) [[unlikely]] {
            // 只有「差量幅值超出 DzVolume」能走到这里（0 已在 fanout 跳过）
            report_order_rejected(account, delta, "volume_out_of_range");
            return;
        }
        const PricePlan plan = resolve_order_price(price, spec_.price_tick, direction, tick_);
        if (!plan.valid) [[unlikely]] {
            report_order_rejected(account, delta, "invalid_price");
            return;
        }
        if (plan.adjustment != PriceAdjustment::None) {
            report_price_adjusted(account, price, plan);
        }
        const DzOrderId order_id =
            dz_place_order(dz_, account.account_id.c_str(), instrument_id_.c_str(), direction,
                           DZ_PRICE_LIMIT, plan.price, order_qty, DZ_POSITION_EFFECT_AUTO);
        if (order_id < 0) [[unlikely]] {
            report_order_failed(account, plan.price, order_qty, direction);
        }
    }

    /**
     * @brief **唯一的** `dz_set_logical_position` 出口（组件文档 §3「唯一出口」）
     *
     * 上报的 `net_volume` 是账户逻辑持仓 `L_i`（不是策略级 P：策略逻辑持仓按账户各报一条，
     * ADR 0027 决策 15）。写帧失败记诊断——上报丢失会让 UI 侧的逻辑/实际偏差对比失真。
     *
     * @param account 目标挂靠账户
     * @param net_l   该账户的逻辑持仓（= 目标净额）
     */
    void publish_position(const AttachedAccount& account, LargeVolume net_l) {
        if (dz_ == nullptr) {
            return;  // 虚拟台账模式：无会话句柄
        }
        if (!dz_set_logical_position(dz_, account.account_id.c_str(), instrument_id_.c_str(),
                                     net_l)) {
            report_publish_failed(account, net_l);
        }
    }

    /**
     * @brief 对全部账户重报当前逻辑持仓（A 在回放结束 / `instrument add` / 账户命令后调用）
     *
     * 与 {@link fanout_positions} 的区别是**不发单**：它只把当前 `P` 映射成各账户的 `L_i` 报
     * 一次。回放期 B 照常下单但只做虚拟成交（不逐笔上报），结束后由这里一次性补齐（§5.3）。
     *
     * 同时用于「账户表刚变过」的场景：新加入的账户需要立刻看到当前目标值，删除的账户由
     * A 打「账户 + 空品种」通配清零（本函数不处理删除，那是命令层的语义）。
     *
     * @param include_zero true = 连 `L_i == 0` 的账户也上报（删除/清零后需要显式落地 0）
     */
    void publish_all_positions(bool include_zero = false) {
        if (dz_ == nullptr || accounts_ == nullptr) {
            return;
        }
        for (const AttachedAccount& account : *accounts_) {
            const LargeVolume net_l =
                scaling_internal::logical_position(position_.net_volume, account.scale, scaling_);
            if (net_l == 0 && !include_zero) {
                continue;
            }
            publish_position(account, net_l);
        }
    }

    /**
     * @brief 用本笔 tick 刷新快照（A 在事件入口调用；只对**已被 builder 接受**的 tick）
     *
     * 价格夹取与后续的同步单对手价都读它。被 builder 丢弃的 tick（乱序/非交易时段/量额倒退）
     * 不刷新：否则一次乱序 tick 就能把涨跌停换成过期值，后续单子会夹到错误价格。
     *
     * @param tick 本笔已接受的行情快照
     */
    void update_tick(const DzTick& tick) noexcept { tick_ = snapshot_from_tick(tick); }

    /**
     * @brief 设置执行门禁（A 在回放/静默填窗结束、进入 live 时调用；组件文档 §3）
     *
     * `live_ == false` 时交易意图只做虚拟成交、不发单、不上报。这是两个执行开关里唯一会被
     * A 拨动的一个（另一个是 `failed_`，由异常边界设置）。
     *
     * @param live true = 进入实盘（发单 + 上报）
     */
    void set_live(bool live) noexcept { live_ = live; }

    /// @brief 当前是否处于实盘阶段（诊断与测试用；`false` = 虚拟台账模式）
    [[nodiscard]] bool is_live() const noexcept { return live_; }

    /// @brief 最近一笔已接受 tick 的快照（只读；A 与测试用）
    [[nodiscard]] const TickSnapshot& tick_snapshot() const noexcept { return tick_; }

    /// @brief `volume == 0` 且本方向没有可平仓（= 无操作，按 §6 不诊断）
    [[nodiscard]] bool is_zero_volume_noop(position_internal::Intent intent,
                                           Volume strategy_volume) const noexcept {
        if (strategy_volume != 0) {
            return false;
        }
        const bool buying_side = intent == position_internal::Intent::Buy ||
                                 intent == position_internal::Intent::BuyToCover;
        return buying_side ? position_.net_volume >= 0 : position_.net_volume <= 0;
    }

    /// @brief 当前 bar 的记账时间戳（Tier 1 的进出场日期/时间/bar 序号来源）
    [[nodiscard]] position_internal::BarStamp bar_stamp() const noexcept {
        return position_internal::BarStamp{
            .date = current_date_, .time = current_time_, .bar_index = bar_index_};
    }

    /**
     * @brief 用被关闭 bar 的收盘价更新当前持仓期间浮盈/浮亏极值（组件文档 §6.1）
     *
     * 调用点在 A 的 `on_bar_close` 分发里、**B 的回调之前**：「bar 收盘前更新」意味着 B 在
     * `on_bar_close` 里读到的极值已包含刚收这根 bar 的收盘价。A 不判持仓、不算钱——口径
     * （无仓不动、`volume_multiple` 换算）只在这里写一次。
     *
     * @param bar_close 被关闭 bar 的收盘价
     */
    void update_profit_extremes(double bar_close) noexcept {
        if (position_.net_volume == 0) {
            return;  // 无仓：极值在平仓时已归零，没有可更新的持仓期间
        }
        const double profit = position_profit_at(bar_close);
        if (profit > position_.max_position_profit) {
            position_.max_position_profit = profit;
        }
        if (profit < position_.max_position_loss) {
            position_.max_position_loss = profit;
        }
    }

    /**
     * @brief 浮动盈亏（钱）在给定价格下的取值（`position_profit` / `contract_profit` / 极值
     *        更新三处共用的唯一公式）
     *
     * 权重 `volume_weight` 是「乘什么手数」：传 `P` 得总浮动盈亏，传 `sign(P)` 得每手浮动
     * 盈亏（组件文档 §6.1 的两条公式本来只差这一个因子）。
     *
     * 两种情形返回 0：
     *   - **无仓**（`P == 0`）：没有持仓就没有浮动盈亏；
     *   - **`price <= 0`**：表示「还没有任何价格」（`on_init` 期或无 bar 的 ctx），而不是
     *     「价格真的是 0」。若不挡这一条，`(0 − 均价) × P × 乘数` 会算出一笔巨大的虚假浮亏
     *     （均价 100、2 手、乘数 10 → −2000），而 B 在 `on_init` 里读到的就是它。
     *
     * @param price         用来计价的现价（bar 收盘价）
     * @param volume_weight 手数权重：`P`（总额）或 `sign(P)`（每手）
     */
    [[nodiscard]] double profit_at(double price, double volume_weight) const noexcept {
        if (position_.net_volume == 0 || price <= 0.0) {
            return 0;
        }
        return (price - position_.avg_entry_price) * volume_weight * spec_.volume_multiple;
    }

    /// @brief 总浮动盈亏在给定价格下的取值（极值更新用）
    [[nodiscard]] double position_profit_at(double price) const noexcept {
        return profit_at(price, static_cast<double>(position_.net_volume));
    }

    /// @brief 最新价（当前 bar 的收盘价 = 最近一笔已接受 tick 的价；无 bar 时为 0）
    ///
    /// 返回 0 是「没有价格」的哨兵值，由 {@link position_profit_at} 识别并返回 0 盈亏；
    /// 不在这里另设 optional/NaN，避免把「没有价格」变成 B 可见的第二种状态。
    [[nodiscard]] double current_price() const noexcept {
        if (bar_index_ < 0 || window_.size == 0) {
            return 0;
        }
        return price_.close[static_cast<std::size_t>(window_.end - 1)];
    }

    /**
     * @brief 距某个参考 bar 的 bar 数（`bars_since_*` 的唯一实现）
     *
     * 同 bar 返回 0、无参考返回 -1。参考值为 -1 有两种来源，语义相同（都表示「无可参照的
     * 建仓/平仓」）：从未发生，或当前已无持仓（台账在全平时把 entry/last_entry 的 bar 序号
     * 复位为 -1）。
     *
     * `on_bar_close` 里调用时窗口尚未推进，`bar_index_` 仍是被关闭那根 bar 的序号，因此
     * 「同 bar = 0」在三个回调里自动成立，不需要额外参数（组件文档 §6.1 的口径）。
     *
     * @param reference_bar 参考 bar 序号（-1 = 无参考）
     * @return bar 差值；任一侧无效返回 -1
     */
    [[nodiscard]] std::int64_t bars_since(std::int64_t reference_bar) const noexcept {
        if (reference_bar < 0 || bar_index_ < 0) {
            return -1;
        }
        return bar_index_ - reference_bar;
    }

    /// @brief 拒单诊断（负手数 / 非法价格 / 失败实例）：逻辑持仓与 Tier 1 完全不变
    void report_intent_rejected(position_internal::Intent intent, std::string_view reason);
    /// @brief 记账结果的分级诊断（WARN 无效方向与钳制、INFO 由 0 触发平仓；无变化不诊断）
    void report_intent_outcome(position_internal::Intent intent,
                               Volume strategy_volume,
                               const position_internal::LedgerOutcome& outcome);

    // ==== 执行诊断（#160；全部写 stderr，prose 区分等级，见 .cpp 段注释）====

    /// @brief 挂靠账户差量单被拒（价格非法 / 数量超出 `DzVolume`）
    void report_order_rejected(const AttachedAccount& account,
                               LargeVolume delta,
                               std::string_view reason);
    /// @brief 下单价格被调整（tick 对齐 / 涨跌停夹取）：记原始价、最终价与原因
    void report_price_adjusted(const AttachedAccount& account,
                               double requested,
                               const PricePlan& plan);
    /// @brief 写帧失败（`dz_place_order < 0`）：单子没出去的唯一证据
    void report_order_failed(const AttachedAccount& account,
                             double price,
                             Volume order_qty,
                             DzDirection direction);
    /// @brief 逻辑持仓上报写帧失败
    void report_publish_failed(const AttachedAccount& account, LargeVolume net_l);

    /**
     * @brief 推进一根 bar：回卷兜底 → 把 `bar` 写进新槽位 → 用户序列写初值/carry → 收尾
     *
     * 顺序严格照组件文档 §4.1（回卷 → 写 `base_[end]` → 才 `++end`）。公开价格视图绑定在
     * 构造时完成，推进只改窗口几何，视图无需重绑。
     *
     * 回卷兜底（§4.4 第二条路径）就在这里：`end` 用尽时先把**价格与全部用户序列**一起搬回
     * 头部再写。正常由 18:00 定时器提前搬（{@link relocate}），这里是定时器被积压延迟/错过时
     * 的兜底，语义完全相同（纯布局）。
     *
     * @param bar 新开的 bar（builder 已用本笔 tick 播种 OHLC）
     */
    void advance_bar(const bar::Bar& bar) noexcept {
        if (window_.end >= window_.buffer_bars) {
            relocate();  // 兜底：定时器错过 / 余量配得过小
        }
        const std::int32_t slot = window_.end;
        write_bar(slot, bar);
        write_user_series(slot);
        series_internal::commit_advance(window_);
        ++bar_index_;
        // Tier 1 的进出场时间 = 该 bar 的**标准结束时间**（§6.1）。在推进时写一次、bar 内不变：
        // 同一根 bar 内多次建仓/平仓都归到同一个时间戳，与 `bars_since_*` 的 bar 序号自洽。
        // v1 只支持日内周期（60/180/300/900），`period_sec == 0` 的日线不在范围内，不做兜底。
        current_date_ = bar.trading_day;
        current_time_ = bar::TradingTime::to_clock(bar.standard_end_sec());
    }

    /**
     * @brief 把形成中 bar 的最新字段写进当前槽位（`[0]`，即 `end - 1`）
     *
     * builder 的事件模型保证 `update` 之前必有一次 `open`（首根 bar、换桶换日、竞价首根都
     * 先 `begin_bar` 再 `notify_update`），因此 `end >= 1` 恒成立。这里仍以 `bar_index_`
     * 兜底：`update` 先于任何 `open` 到达属框架内部不变式被破坏，此时宁可不写，也不能让
     * `end - 1` 变成 -1 去越界写数组（UB）。
     *
     * @param bar builder 给出的形成中 bar
     */
    void update_current_bar(const bar::Bar& bar) noexcept {
        if (bar_index_ < 0) [[unlikely]] {
            return;
        }
        write_bar(window_.end - 1, bar);
    }

    /**
     * @brief 回卷（§4.4）：把窗口整体搬回数组头部，纯布局变换
     *
     * 两条路径共用本函数，语义完全一致——不改任何元素值、不改 `size`、不改 `bar_index`：
     *   1. **18:00 定时器主路径**（A 的 `on_schedule` 识别自己的 `timer_id` 后调用）；
     *   2. **推进前内联兜底**（{@link advance_bar} 在 `end` 用尽时调用）。
     *
     * 全部数组必须一起搬（价格 7 条 + 每条用户序列），否则共享的 `window_` 与某一组数组
     * 的几何会错位。搬完才把 `end` 置为 `size`（`finish_relocate`），中途不会出现「部分数组
     * 已搬、`end` 已改」的半完成状态。
     */
    void relocate() noexcept {
        series_internal::relocate_array<double>(window_, price_.open.get());
        series_internal::relocate_array<double>(window_, price_.high.get());
        series_internal::relocate_array<double>(window_, price_.low.get());
        series_internal::relocate_array<double>(window_, price_.close.get());
        series_internal::relocate_array<double>(window_, price_.volume.get());
        series_internal::relocate_array<double>(window_, price_.turnover.get());
        series_internal::relocate_array<double>(window_, price_.open_interest.get());
        for_each_user_series([this](auto& storage) {
            series_internal::relocate_array(window_, storage.base.get());
        });
        series_internal::finish_relocate(window_);
    }

    /**
     * @brief 给新槽位写用户序列：首根写声明初值，其后复制上一根 `[0]`（carry）
     *
     * 静默填窗期 B 不被回调、不会写序列，因此「逐根写 `init`」与「首根 `init` + 其后 carry」
     * 的观测结果完全相同（`[0]` 始终是 `init`）；用 carry 实现同时满足进入回调期后的语义：
     * 每根新 bar 的当前值从上一根的当前值（可能是 B 写的值）开始。
     *
     * @param slot 新 bar 的物理槽位（尚未计入 `size`；上一根此刻在 `slot - 1`）
     */
    void write_user_series(std::int32_t slot) noexcept {
        const bool first_bar = bar_index_ < 0;
        const auto index = static_cast<std::size_t>(slot);
        for_each_user_series([&](auto& storage) {
            storage.base[index] = first_bar ? storage.init : storage.base[index - 1];
        });
    }

    /**
     * @brief 遍历两条用户序列容器里**全部**序列（`double` 与 `int64_t` 各一遍）
     *
     * 用户序列按类型分装在两个容器里（`series_d_` / `series_i_`），而「对每条序列都做同一件
     * 事」出现在两个地方：推进时写初值/carry、回卷时搬数组。没有本函数，两处都要各写一对
     * `for` 循环——将来加第三种元素类型时会漏改其中一处，而漏掉的那处不会编译失败（少搬/
     * 少写一条数组是静默错误）。泛型 lambda 让同类操作只写一次。
     *
     * @param visit 接收 `SeriesStorage<T>&` 的泛型可调用对象
     */
    template <class Visitor>
    void for_each_user_series(Visitor&& visit) {
        for (SeriesStorage<double>& storage : series_d_) {
            visit(storage);
        }
        for (SeriesStorage<std::int64_t>& storage : series_i_) {
            visit(storage);
        }
    }

    /// @brief 按元素类型选择用户序列容器（两种类型各一份，容器本身不进 B 可见面）
    template <UserSeriesElement T>
    [[nodiscard]] std::vector<SeriesStorage<T>>& series_storage() noexcept {
        if constexpr (std::same_as<T, double>) {
            return series_d_;
        } else {
            return series_i_;
        }
    }

    /// @brief 把一根 bar 的 7 个公开字段写进指定物理槽位
    void write_bar(std::int32_t slot, const bar::Bar& bar) noexcept {
        const auto index = static_cast<std::size_t>(slot);
        price_.open[index] = bar.open;
        price_.high[index] = bar.high;
        price_.low[index] = bar.low;
        price_.close[index] = bar.close;
        price_.volume[index] = static_cast<double>(bar.volume);
        price_.turnover[index] = bar.turnover;
        price_.open_interest[index] = static_cast<double>(bar.open_interest);
    }

    /// @brief 实例是否已失败（失败后不再接收任何 B 回调；组件文档 §4.6）
    ///
    /// 内部面而非 B 可见面：B 无法据此改变行为，失败后它本来就不再被回调。
    [[nodiscard]] bool is_failed() const noexcept { return failed_; }

    /// @brief 标记实例失败：此后不再接收任何 B 回调（组件文档 §4.6）
    void mark_failed() noexcept { failed_ = true; }

    /**
     * @brief `on_init` 作用域守卫：构造即进入、析构即退出（A 在调 B 的 `on_init` 时持有）
     *
     * `make_series` 只在这段时间内合法（组件文档 §4.1「创建」）。用显式作用域而不是
     * 「`bar_index_ < 0`」判断：回调门禁生效后 `bar_index_ < 0` 也会覆盖静默填窗期，若某次
     * 误调用发生在静默期，序列就会在窗口推进后才注册、前几根是未初始化内存——语义残缺却
     * 不报错。作用域精确对应「实例创建期」，没有这种模糊区间。
     *
     * 做成 RAII 而不是成对的 `begin/end`：`on_init` 里 B 可能抛异常，成对调用要在每条
     * catch 分支里各写一遍 `end_on_init()`（漏一处就把标记永久留在 true，之后任何回调里
     * `make_series` 都会被放行）。析构自动复位，没有可以漏写的那一处。
     */
    class OnInitScope {
    public:
        /// @brief 进入 `on_init` 作用域
        explicit OnInitScope(InstrumentContext& ctx) noexcept
            : ctx_(ctx) {
            ctx_.in_on_init_ = true;
        }

        /// @brief 退出作用域（异常展开时同样执行）
        ~OnInitScope() { ctx_.in_on_init_ = false; }

        OnInitScope(const OnInitScope&) = delete;
        OnInitScope& operator=(const OnInitScope&) = delete;
        OnInitScope(OnInitScope&&) = delete;
        OnInitScope& operator=(OnInitScope&&) = delete;

    private:
        InstrumentContext& ctx_;
    };

    std::int64_t bar_index_ = -1;  ///< 当前 bar 序号（0 起、单调；-1 = 无 bar）
    std::string instrument_id_;    ///< 合约代码（平台唯一键）
    /// 用户序列（只放 B 经 `make_series` 注册的序列；框架内部状态不进这里，组件文档 §3）
    std::vector<SeriesStorage<double>> series_d_;
    std::vector<SeriesStorage<std::int64_t>> series_i_;
    InstrumentSpec spec_;     ///< 合约属性（乘数供 Tier 1、`price_tick` 供价格对齐）
    PositionState position_;  ///< 策略虚拟持仓 + Tier 1（只在 `execute_intent` 漏斗里更新）
    /// 最近一笔**已接受** tick 的快照（价格夹取 / 同步单对手价；组件文档 §3）
    TickSnapshot tick_;
    DzContext* dz_ = nullptr;                 ///< 引擎会话句柄（nullptr = 虚拟台账模式）
    const AccountTable* accounts_ = nullptr;  ///< A 的挂靠账户表只读指针（nullptr = 0 个账户）
    ScalingConfig scaling_;                   ///< 缩放配置副本（启动后不可变）
    /// 当前 bar 的记账时间戳（A 在推进窗口时从 `bar::Bar` 写入；Tier 1 进出场时间来源）
    DzDate current_date_ = DZ_DATE_NA;
    DzTime current_time_ = 0;
    bool in_on_init_ = false;  ///< 是否正处在 `on_init` 内（限制 `make_series` 的调用时机）
    bool live_ = false;        ///< false = 回放/静默填窗：只做虚拟成交、不发单、不上报
    bool failed_ = false;      ///< 实例失败：此后不再接收任何 B 回调、不再受理下单意图
};

/**
 * @brief 意图诊断（stderr；header-only 公开代码不引入 spdlog，ADR 0020/0021）
 *
 * 文案口径与 `strategy_engine.h` 一致：小写开头、无句号、`prose | k=v k=v`、全英文。分级照
 * 组件文档 §6，用四段不同的 prose 表达：
 *   - 拒单（负手数 / 非法价格 / 失败实例）= `intent rejected | ...`；
 *   - `n > 0` 方向无效、`n > |P|` 钳制 = `intent ignored` / `intent clamped`；
 *   - `volume == 0` 实际触发平仓 = `intent flat_by_zero`（便于追溯非预期清仓）；
 *   - 无变化且无诊断依据（`volume == 0` 且无可平仓）**不诊断**，避免平仓语句逐 bar 刷屏。
 *
 * **这里没有「级别」**：框架是 header-only、只能写 stderr，而 master 把子进程 stderr 一律按
 * WARN 转发——三条路径在 master 日志里级别相同，靠 prose 区分。所以注释与文档都不写
 * 「ERROR 级 / WARN 级」，避免让人以为有级别分流。
 *
 * 三个分级用不同的 prose 而不是同一个 prose 加 `level=` 字段：master 把子进程 stderr 一律
 * 按 WARN 转发，日志级别只看得到 prose；prose 相同就等于三个级别在日志里无法区分。
 */
inline void InstrumentContext::report_intent_rejected(position_internal::Intent intent,
                                                      std::string_view reason) {
    std::cerr << std::format("intent rejected | instrument={} intent={} reason={}", instrument_id_,
                             position_internal::intent_name(intent), reason)
              << '\n';
}

inline void InstrumentContext::report_intent_outcome(
    position_internal::Intent intent,
    Volume strategy_volume,
    const position_internal::LedgerOutcome& outcome) {
    const std::string_view name = position_internal::intent_name(intent);
    if (outcome.direction_invalid) {
        std::cerr << std::format(
                         "intent ignored | instrument={} intent={} volume={} net_position={}",
                         instrument_id_, name, strategy_volume, outcome.net_before)
                  << '\n';
        return;
    }
    if (outcome.clamped) {
        std::cerr << std::format(
                         "intent clamped | instrument={} intent={} volume={} net_position={}",
                         instrument_id_, name, strategy_volume, outcome.net_before)
                  << '\n';
        return;
    }
    if (!outcome.applied()) {
        return;  // `volume == 0` 且无可平仓：静默（§6 明确要求不诊断）
    }
    if (outcome.zero_volume_closed) {
        std::cerr << std::format(
                         "intent flat_by_zero | instrument={} intent={} net_before={} net_after={}",
                         instrument_id_, name, outcome.net_before, outcome.net_after)
                  << '\n';
    }
}

/**
 * @brief 执行诊断（#160；同样写 stderr、无级别，prose 区分四类事件）
 *
 * 两条硬约束决定了这里的形状：
 *
 *   - **热路径不打日志**（组件文档 §10）：`submit_order` / `publish_position` 只在拒单、
 *     价格被调整、写帧失败时输出——正常发单与上报是静默的。否则每笔交易意图会刷 2N 行，
 *     而 1us 的性能预算根本容不下 `std::format`；
 *   - **prose 就是级别**：master 把子进程 stderr 一律按 WARN 转发，级别分流要等 SDK 给策略
 *     一条日志通道（ADR 0021），所以四类事件各用一段不同的 prose：
 *       - `order rejected`：价格或数量非法，单没发出去（逻辑账已推进）；
 *       - `order price adjusted`：单发出去了，但价格被对齐/夹取（可追溯「为什么成交价不同」）；
 *       - `order submit failed`：`dz_place_order` 返回 < 0（写帧失败）；
 *       - `logical position publish failed`：上报写帧失败。
 *
 * `order price adjusted` 是 INFO 级事件却和拒单一样落在 stderr：它不改变任何逻辑状态，但
 * 是「策略传入价与交易所实际挂单价不一致」的唯一解释来源，静默会让迁移用户以为框架偷价。
 */
inline void InstrumentContext::report_order_rejected(const AttachedAccount& account,
                                                     LargeVolume delta,
                                                     std::string_view reason) {
    std::cerr << std::format("order rejected | instrument={} account={} qty={} reason={}",
                             instrument_id_, account.account_id, delta, reason)
              << '\n';
}

inline void InstrumentContext::report_price_adjusted(const AttachedAccount& account,
                                                     double requested,
                                                     const PricePlan& plan) {
    // 浮点用 {:.10g}：tick 对齐的调整量常常在末位，默认 6 位有效数字会把「100 → 100.1」这类
    // 正好看得出的差异也抹掉。
    std::cerr << std::format(
                     "order price adjusted | instrument={} account={} requested={:.10g} "
                     "final={:.10g} reason={}",
                     instrument_id_, account.account_id, requested, plan.price,
                     price_adjustment_name(plan.adjustment))
              << '\n';
}

inline void InstrumentContext::report_order_failed(const AttachedAccount& account,
                                                   double price,
                                                   Volume order_qty,
                                                   DzDirection direction) {
    std::cerr << std::format(
                     "order submit failed | instrument={} account={} direction={} qty={} "
                     "price={:.10g} error=\"{}\"",
                     instrument_id_, account.account_id, direction_name(direction), order_qty,
                     price, dz_errmsg())
              << '\n';
}

inline void InstrumentContext::report_publish_failed(const AttachedAccount& account,
                                                     LargeVolume net_l) {
    std::cerr << std::format(
                     "logical position publish failed | instrument={} account={} net_volume={} "
                     "error=\"{}\"",
                     instrument_id_, account.account_id, net_l, dz_errmsg())
              << '\n';
}

static_assert(!std::is_copy_constructible_v<InstrumentContext>,
              "InstrumentContext holds self-referential views and must not be copied");
static_assert(!std::is_copy_assignable_v<InstrumentContext>,
              "InstrumentContext holds self-referential views and must not be copied");
static_assert(!std::is_move_constructible_v<InstrumentContext>,
              "InstrumentContext holds self-referential views and must not be moved");
static_assert(!std::is_move_assignable_v<InstrumentContext>,
              "InstrumentContext holds self-referential views and must not be moved");

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_CONTEXT_H_
