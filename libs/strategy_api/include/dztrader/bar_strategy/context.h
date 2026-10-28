/**
 * @file context.h
 * @brief 品种上下文：品种数据 + B 的只读 API 面（ADR 0027 决策 5，组件文档 §3）
 *
 * 每品种一个 `InstrumentContext`，由 `BarStrategyManager` 在稳定的 map 节点内就地构造
 * （`InstrumentNode`）: 其它品种增删不会搬移本节点，B 持有的价格视图因此在实例存活期内
 * 始终有效。
 *
 * 窗口几何与价格数组的所有权都在 ctx 内：`window_` 是价格序列（与将来的用户序列）共用的
 * 窗口几何，`price_` 是 7 条定长物理数组，容量在构造时一次分配、永不扩容。推进与回卷由 A
 * 经 `advance_bar()` 驱动（内部头 `dztrader/series_internal.h` 的
 * `prepare_advance()` / `commit_advance()`，顺序：回卷 → 写槽位 → `++end`）。
 *
 * **B 可见面**只有 public 区：7 条只读价格视图、`bar_index()`、`instrument_id()`。窗口推进、
 * 失败标记等运行期状态是 private，仅 `BarStrategyManager` 与 `InstrumentNode` 可访问
 * （friend），热路径上没有虚函数。
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

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

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
 * #157 只注入骨架所需的四项；合约属性（`InstrumentSpec`）、挂靠账户表、`ParamStore`、
 * 缩放配置随后续票（#159/#160/#162）加进本结构，构造点集中在节点内一处。
 */
struct InstrumentInit {
    DzContext* dz = nullptr;        ///< 引擎会话句柄（on_start 取得，进程内不变）
    std::string instrument_id;      ///< 合约代码（平台唯一键）
    std::int32_t max_lookback = 0;  ///< 可读历史根数上限（= 配置 max_lookback）
    std::int32_t buffer_bars = 0;   ///< 物理数组长度（= max_lookback + reserve_bars）
};

/**
 * @brief 每品种一个：品种数据 + B 的 API 面合并为同一对象（组件文档 §3）
 *
 * 公开面（B 可见）：7 条只读价格视图、`bar_index()`、`instrument_id()`。
 * 回调参数统一是 `InstrumentContext&`，B 不接触引擎回调、不持有 A 指针。
 *
 * 视图在构造时绑定自身 `window_` / `price_`，之后永不重绑；`WindowState` 一推进，全部视图
 * 同时看到新 bar。**声明顺序即构造顺序**：`window_` / `price_` 必须先于视图成员声明，
 * 否则视图会绑定到尚未构造完成的数组。
 */
class InstrumentContext {
private:
    WindowState window_;  ///< 价格（与将来的用户序列）共用的窗口几何
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
     * `-1` = 尚未有 bar（`on_init` 期间恒为 -1；静默填窗/回调门禁属 #158）。
     */
    [[nodiscard]] std::int64_t bar_index() const noexcept { return bar_index_; }

    /// @brief 合约代码（平台唯一键）
    [[nodiscard]] const std::string& instrument_id() const noexcept { return instrument_id_; }

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
          dz_(init.dz),
          instrument_id_(init.instrument_id) {}

    /// @brief 分配一条价格数组（`buffer_bars` 个元素；首次写入前不读，无需零初始化）
    [[nodiscard]] static std::unique_ptr<double[]> make_array(std::int32_t buffer_bars) {
        return std::make_unique_for_overwrite<double[]>(static_cast<std::size_t>(buffer_bars));
    }

    /**
     * @brief 推进一根 bar：回卷兜底 → 把 `bar` 写进新槽位 → `++end` / `++size` → `++bar_index`
     *
     * 顺序严格照组件文档 §4.1（回卷 → 写 `base_[end]` → 才 `++end`）。公开价格视图绑定在
     * 构造时完成，推进只改窗口几何，视图无需重绑。
     *
     * @param bar 新开的 bar（builder 已用本笔 tick 播种 OHLC）
     */
    void advance_bar(const bar::Bar& bar) noexcept {
        const std::int32_t slot = series_internal::prepare_advance(
            window_, price_.open.get(), price_.high.get(), price_.low.get(), price_.close.get(),
            price_.volume.get(), price_.turnover.get(), price_.open_interest.get());
        write_bar(slot, bar);
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

    std::int64_t bar_index_ = -1;  ///< 当前 bar 序号（0 起、单调；-1 = 无 bar）
    DzContext* dz_ = nullptr;      ///< 引擎会话句柄（on_start 注入，进程内不变）
    std::string instrument_id_;    ///< 合约代码（平台唯一键）
    bool failed_ = false;          ///< 实例失败：不再回调、不再受理下单意图
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
