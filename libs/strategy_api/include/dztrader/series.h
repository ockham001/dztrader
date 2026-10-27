/**
 * @file series.h
 * @brief Series 层：TB 口径的序列视图与窗口几何（ADR 0027 决策 5，组件文档 §4）
 *
 * 每品种一个 {@link WindowState}，价格序列与用户序列共用：物理长度
 * `buffer_bars = max_lookback + reserve_bars`，实例创建时一次分配、永不扩容；
 * 推进只改 `end` / `size` 两个整数，物理缓冲用尽（或盘后主动回卷）时把整窗
 * 搬回数组头部——回卷是纯布局变换，不改值、不改 `size`、不改 `bar_index`
 * （推进与回卷的驱动在内部头 `dztrader/series_internal.h`）。
 *
 * 读接口只有一个口径：`[0]` 是当前 bar、`[n]` 往回 n；`n ∉ [0, size)` 一律抛
 * `std::out_of_range`（不钳位、不产生 NaN、不返回初值），消息含 `n` / `size` /
 * `max_lookback`。零检查快路径是 `span()`：时间正序（最旧→最新）、长度 = `size`。
 *
 * 写接口只有 `series[0] = v`（{@link SeriesRef} 写入代理）；历史位没有写入 API，
 * 代理对 `n != 0` 抛 `std::out_of_range`（防御性兜底）。不提供 `push_back` /
 * `erase` / `resize` / `reserve` / `at`。
 *
 * 公开面约束由静态断言锁定：`Series` / `SeriesView` 只持两个指针、单继承、
 * 无虚函数、无虚析构、平凡可拷贝、标准布局（`sizeof` 相等）。
 *
 * 构造缝：两个类的 `(base, window)` 构造仅对 `series_internal::Access`
 * （A 侧与测试）与 `InstrumentContext`（#157）开放，B 侧只能经
 * `ctx.make_series` / ctx 的公开价格视图取得。
 */
#ifndef DZTRADER_SERIES_H_
#define DZTRADER_SERIES_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace dztrader {

class InstrumentContext;  // 前向：A 侧构造注入（#157）

namespace series_internal {

struct Access;  // 内部构造缝（A 与测试），定义见 dztrader/series_internal.h

/// @brief 越界前的 Debug 断言（Release 与测试构建编译掉）
///
/// 组件文档 §4.2 要求「Debug 构建在抛出前加 assert，便于直接停在出错点」。断言只在
/// 失败路径执行，不影响热路径。测试目标定义 `DZTRADER_SERIES_NO_DEBUG_ASSERT` 关闭它：
/// 断言先于异常，否则 Debug 下 `EXPECT_THROW` 用例会先 abort、永远到不了异常断言
/// （Release 由 `NDEBUG` 自然关闭，故默认门禁不受影响）。
#ifndef DZTRADER_SERIES_ASSERT
#ifdef NDEBUG
#define DZTRADER_SERIES_ASSERT(condition) ((void)0)
#elif defined(DZTRADER_SERIES_NO_DEBUG_ASSERT)
#define DZTRADER_SERIES_ASSERT(condition) ((void)0)
#else
#define DZTRADER_SERIES_ASSERT(condition) assert(condition)
#endif
#endif

}  // namespace series_internal

/**
 * @brief 每品种一个的窗口几何（纯 POD，16 字节）
 *
 * 窗口 = `[end - size, end)`：`size` 是已填充 bar 数（含当前 bar），`end` 是窗口
 * 右开边界兼下一次写入位置。`max_lookback` 是可读历史上限（= 配置值），
 * `buffer_bars` 是物理元素数（= `max_lookback + reserve_bars`，B 不可见）。
 *
 * 不变式：`0 <= size <= max_lookback <= buffer_bars`、`size <= end <= buffer_bars`。
 * 推进与回卷由 `series_internal::prepare_advance` / `commit_advance` /
 * `relocate_window` 驱动，视图只读取本结构。
 */
struct WindowState {
    std::int32_t end = 0;           ///< 写入位置 = 窗口右开边界
    std::int32_t size = 0;          ///< 已填充 bar 数（含当前）
    std::int32_t max_lookback = 0;  ///< 可读历史上限（= 配置 max_lookback）
    std::int32_t buffer_bars = 0;   ///< 物理缓冲元素数（= max_lookback + reserve_bars）
};

static_assert(sizeof(WindowState) == 4 * sizeof(std::int32_t), "WindowState must stay 4 x int32");
static_assert(std::is_trivially_copyable_v<WindowState>, "WindowState must be trivially copyable");
static_assert(std::is_standard_layout_v<WindowState>, "WindowState must be standard layout");

/**
 * @brief 序列只读视图：全部读接口的唯一实现
 *
 * 只持两个指针（物理数组 + 共享窗口几何），构造一次后永不更新：A 推进一次
 * `WindowState`，所有视图（价格 + 用户序列）同时看到新 bar。视图可安全复制，
 * 也可在回调外按值保存（数组与 `WindowState` 地址稳定；内容与长度随推进变化）。
 *
 * @tparam T 元素类型（`double` 或 `std::int64_t`）
 */
template <class T>
class SeriesView {
public:
    /**
     * @brief TB 口径读：`[0]` 当前 bar，`[n]` 往回 n 根
     * @param n 回溯深度（`0 <= n < size()`）
     * @return 第 n 根历史 bar 的值
     * @throw std::out_of_range `n ∉ [0, size)`（负 n 与 `n >= size` 都抛；不钳位）
     */
    [[nodiscard]] T operator[](std::ptrdiff_t n) const {
        check_index(n);
        return base_[static_cast<std::ptrdiff_t>(window_->end) - 1 - n];
    }

    /// @brief 已填充 bar 数（含当前 bar）；`on_init` 期恒为 0
    [[nodiscard]] std::int32_t size() const noexcept {
        return window_ == nullptr ? 0 : window_->size;
    }

    /// @brief 可回读的历史根数上限（= 配置 `max_lookback`）；正常回调期等于 `size()`
    [[nodiscard]] std::int32_t max_lookback() const noexcept {
        return window_ == nullptr ? 0 : window_->max_lookback;
    }

    /**
     * @brief 连续窗口：时间正序（最旧→最新）、长度 = `size()`、零检查
     *
     * 唯一的快路径：指标循环、`std::accumulate`、`std::ranges` 直接吃这段区间。
     * 仅在本次回调内有效（下一根推进/回卷后内容与长度可能变化）。
     */
    [[nodiscard]] std::span<const T> span() const noexcept {
        if (base_ == nullptr || window_ == nullptr) {
            return {};
        }
        const auto offset = static_cast<std::ptrdiff_t>(window_->end - window_->size);
        return {base_ + offset, static_cast<std::size_t>(window_->size)};
    }

protected:
    friend struct series_internal::Access;
    friend class InstrumentContext;

    SeriesView() = default;
    SeriesView(T* base, const WindowState* window)
        : base_(base),
          window_(window) {}

    /**
     * @brief 校验 `n` 落在 `[0, size)`，否则走 Debug 断言并抛 `std::out_of_range`
     *
     * 消息含 `n` / `size` / `max_lookback`，用户可直接定位该调大的配置项。
     */
    void check_index(std::ptrdiff_t n) const {
        const auto current_size = static_cast<std::ptrdiff_t>(this->size());
        if (base_ != nullptr && n >= 0 && n < current_size) {
            return;
        }
        DZTRADER_SERIES_ASSERT(false);
        throw std::out_of_range(
            std::format("series index out of range | n={} size={} max_lookback={}", n, current_size,
                        this->max_lookback()));
    }

    T* base_ = nullptr;                    ///< 物理数组（buffer_bars 个元素）
    const WindowState* window_ = nullptr;  ///< 共享窗口几何（归 InstrumentContext）
};

/**
 * @brief `X[0]` 的写入代理：只允许给当前 bar 赋值
 *
 * 由 `Series::operator[]` 产生；`n != 0` 时赋值抛 `std::out_of_range`——历史位没有
 * 写入 API（与 TB/MC 一致）。
 */
template <class T>
class SeriesRef {
public:
    SeriesRef() = default;

    /// @brief 写当前 bar；`n != 0` 抛 `std::out_of_range`（防御性兜底）
    SeriesRef& operator=(T value) {
        validate_writable();
        *slot_ = value;
        return *this;
    }

    /**
     * @brief `X[0] = X[n]` / `X[0] = X[0]` 走值语义，不是代理指针拷贝
     *
     * 缺了本重载，编译器生成的逐成员拷贝赋值会把「写当前 bar」变成「改写代理对象」，
     * 静默丢失赋值意图。
     */
    SeriesRef& operator=(const SeriesRef& other) { return *this = static_cast<T>(other); }

    /// @brief 读当前 bar 的值（`X[0] = X[1]` 的右侧经此转换）
    /// @throw std::out_of_range 代理未绑定（默认构造后未由 `Series::operator[]` 产生）
    operator T() const {  // NOLINT(google-explicit-constructor)
        if (slot_ == nullptr) {
            DZTRADER_SERIES_ASSERT(false);
            throw std::out_of_range("series ref is unbound");
        }
        return *slot_;
    }

private:
    template <class U>
    friend class Series;

    SeriesRef(T* slot, std::ptrdiff_t n, const WindowState* window, std::int32_t end_snapshot)
        : slot_(slot),
          n_(n),
          window_(window),
          end_snapshot_(end_snapshot) {}

    void validate_writable() const {
        if (slot_ == nullptr) {
            DZTRADER_SERIES_ASSERT(false);
            throw std::out_of_range("series ref is unbound");
        }
        if (n_ != 0) {
            DZTRADER_SERIES_ASSERT(false);
            throw std::out_of_range(std::format("series history is not writable | n={}", n_));
        }
        // 代理可能被保存到下一根 bar（组件文档 §3 允许 B 保存 Series/视图副本）。
        // 那时它绑定的槽位已经变成历史 [1]，n_ 仍是 0；只检查 n_ 会让写历史**静默**成功。
        // 因此代理记住创建时的窗口右边界，写入时要求窗口没有推进过。
        if (window_ != nullptr && window_->end != end_snapshot_) {
            DZTRADER_SERIES_ASSERT(false);
            throw std::out_of_range(std::format("series ref is stale | n={} bar_advanced={}", n_,
                                                window_->end - end_snapshot_));
        }
    }

    T* slot_ = nullptr;                    ///< 当前 bar 的物理槽位
    std::ptrdiff_t n_ = 0;                 ///< 索引（仅 0 可写）
    const WindowState* window_ = nullptr;  ///< 创建代理时的窗口几何（判过期用）
    std::int32_t end_snapshot_ = 0;        ///< 创建代理时的 window.end
};

/**
 * @brief 用户序列：派生只增加「写当前 bar `[0]`」
 *
 * `on_init` 里由 `ctx.make_series<T>(init)` 构造；之后的每根新 bar 由 A 做 carry
 * （复制上一根 `[0]`）。不新增数据成员、无虚函数，`sizeof` 与基类相同。
 *
 * @tparam T 元素类型（`double` 或 `std::int64_t`）
 */
template <class T>
class Series : public SeriesView<T> {
public:
    Series() = default;
    using SeriesView<T>::operator[];

    /**
     * @brief 非 const 对象返回写入代理；读取经代理的 `operator T()`
     *
     * 索引校验与只读视图共用一套口径（`check_index`）：`n ∉ [0, size)` 抛
     * `std::out_of_range`，因此写历史位在到达代理之前就已响亮失败。
     */
    [[nodiscard]] SeriesRef<T> operator[](std::ptrdiff_t n) {
        this->check_index(n);
        return SeriesRef<T>(this->base_ + static_cast<std::ptrdiff_t>(this->window_->end) - 1 - n,
                            n, this->window_, this->window_->end);
    }

private:
    friend struct series_internal::Access;
    friend class InstrumentContext;

    Series(T* base, const WindowState* window)
        : SeriesView<T>(base, window) {}
};

// ---- 公开面不变量（组件文档 §3「零开销约束」）----
static_assert(sizeof(Series<double>) == sizeof(SeriesView<double>),
              "Series must not add data members");
static_assert(sizeof(Series<std::int64_t>) == sizeof(SeriesView<std::int64_t>),
              "Series must not add data members");
static_assert(sizeof(SeriesView<double>) == 2 * sizeof(void*), "SeriesView must stay two pointers");
static_assert(std::is_trivially_copyable_v<Series<double>>, "Series must be trivially copyable");
static_assert(std::is_trivially_copyable_v<Series<std::int64_t>>,
              "Series must be trivially copyable");
static_assert(std::is_trivially_copyable_v<SeriesView<double>>,
              "SeriesView must be trivially copyable");
static_assert(std::is_standard_layout_v<Series<double>>, "Series must be standard layout");
static_assert(std::is_standard_layout_v<Series<std::int64_t>>, "Series must be standard layout");
static_assert(!std::has_virtual_destructor_v<Series<double>>,
              "Series must not have a virtual destructor");
static_assert(!std::is_polymorphic_v<Series<double>>, "Series must not have virtual functions");
static_assert(!std::has_virtual_destructor_v<SeriesView<double>>,
              "SeriesView must not have a virtual destructor");
static_assert(!std::is_polymorphic_v<SeriesView<double>>,
              "SeriesView must not have virtual functions");
static_assert(!std::is_constructible_v<Series<double>, const SeriesView<double>&>,
              "a read-only view must not be upgraded to a writable series");
static_assert(!std::is_convertible_v<SeriesView<double>, Series<double>>,
              "a read-only view must not be upgraded to a writable series");

}  // namespace dztrader

#endif  // DZTRADER_SERIES_H_
