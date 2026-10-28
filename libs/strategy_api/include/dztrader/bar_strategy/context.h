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
 * `instrument_id()`。窗口推进、回卷、失败标记等运行期状态是 private，仅 `BarStrategyManager`
 * 与 `InstrumentNode` 可访问（friend），热路径上没有虚函数。
 *
 * 本头是 SDK 公开头（header-only 框架的一部分）：只依赖标准库与 SDK 公开头，不依赖
 * 任何内部库或日志库（ADR 0020/0021）。
 */
#ifndef DZTRADER_BAR_STRATEGY_CONTEXT_H_
#define DZTRADER_BAR_STRATEGY_CONTEXT_H_

#include <dztrader/api.h>
#include <dztrader/bar/types.h>
#include <dztrader/series.h>
#include <dztrader/series_internal.h>

#include <concepts>
#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace dztrader {

template <class Strategy>
class BarStrategyManager;  // 前向：A 通过 friend 驱动窗口推进与失败标记

template <class Strategy>
struct InstrumentNode;  // 前向：节点就地构造 ctx（传 InstrumentInit）

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
 * #157 只注入骨架所需的三项；合约属性（`InstrumentSpec`）、挂靠账户表、`ParamStore`、
 * 缩放配置、引擎会话句柄随后续票（#159/#160/#162）加进本结构，构造点集中在节点内一处
 * ——到那时才加，现在留空字段等于把「以后可能要」写成接口承诺。
 */
struct InstrumentInit {
    std::string instrument_id;      ///< 合约代码（平台唯一键）
    std::int32_t max_lookback = 0;  ///< 可读历史根数上限（= 配置 max_lookback）
    std::int32_t buffer_bars = 0;   ///< 物理数组长度（= max_lookback + reserve_bars）
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

    InstrumentContext(const InstrumentContext&) = delete;
    InstrumentContext& operator=(const InstrumentContext&) = delete;
    InstrumentContext(InstrumentContext&&) = delete;
    InstrumentContext& operator=(InstrumentContext&&) = delete;

private:
    template <class>
    friend class BarStrategyManager;  // A 通过 friend 驱动推进 / 标记失败

    template <class>
    friend struct InstrumentNode;  // 节点就地构造（传 InstrumentInit）

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
          instrument_id_(init.instrument_id) {}

    /// @brief 分配一条价格数组（`buffer_bars` 个元素；首次写入前不读，无需零初始化）
    [[nodiscard]] static std::unique_ptr<double[]> make_array(std::int32_t buffer_bars) {
        return std::make_unique_for_overwrite<double[]>(static_cast<std::size_t>(buffer_bars));
    }

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
    bool in_on_init_ = false;  ///< 是否正处在 `on_init` 内（限制 `make_series` 的调用时机）
    bool failed_ = false;      ///< 实例失败：此后不再接收任何 B 回调
};

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
