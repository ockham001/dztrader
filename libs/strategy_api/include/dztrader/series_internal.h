/**
 * @file series_internal.h
 * @brief Series 层的内部面：容量推导与窗口几何驱动（ADR 0027 决策 5/6，组件文档 §4）
 *
 * **内部头，不进公开承诺面**：只服务 `InstrumentContext`（#157）与其单元测试。
 * 策略作者不应包含本头——B 侧只能经 `ctx.make_series` 与 ctx 的公开价格视图取序列。
 *
 * 两部分：
 *   - 容量：`resolve_capacity()` 按周期推导 `max_lookback` / `reserve_bars` /
 *     `buffer_bars`，显式覆盖按区间校验；不合法一律抛 `std::invalid_argument`
 *     （消息含字段名 / 当前值 / 合法区间），不静默修正（组件文档 §4.5、§7）。
 *   - 窗口：`prepare_advance()` / `commit_advance()` 成对推进一根 bar，
 *     `relocate_window()` 做纯布局回卷。三者都接收**全部数组**（7 条价格 + 本实例的
 *     用户序列）作为变参指针包，保证一个 `WindowState` 下的所有数组几何永远一致——
 *     这正是「价格序列与用户序列共用一个 WindowState」的实现方式。
 *
 * 回卷是纯布局变换：不改任何元素值、不改 `size`、不改 `bar_index`；任何两次操作之间
 * 执行都合法。推进的写入顺序严格照组件文档 §4.1：回卷 → 写 `base_[end]` → 才 `++end`。
 */
#ifndef DZTRADER_SERIES_INTERNAL_H_
#define DZTRADER_SERIES_INTERNAL_H_

#include <dztrader/series.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>

namespace dztrader::series_internal {

// ============================================================
// 构造缝：A（InstrumentContext）与测试经此绑定视图
// ============================================================

/**
 * @brief 视图构造缝（内部）
 *
 * `Series` / `SeriesView` 的 `(base, window)` 构造是 protected + friend 本类型，
 * 因此 B 侧无法凭空造一条指向外部数组的序列，A 与测试则可以。
 */
struct Access {
    /// @brief 绑定一条只读视图（价格序列）
    template <class T>
    static SeriesView<T> view(T* base, const WindowState* window) {
        return SeriesView<T>(base, window);
    }

    /// @brief 绑定一条可写序列（用户序列；`X[0] = v` 可用）
    template <class T>
    static Series<T> series(T* base, const WindowState* window) {
        return Series<T>(base, window);
    }
};

// ============================================================
// 容量推导与校验（组件文档 §4.5 自动值表 + §7 校验区间）
// ============================================================

/** @brief 可读深度下限（显式覆盖与自动值共用） */
inline constexpr std::int32_t MAX_LOOKBACK_MIN = 32;
/** @brief 可读深度上限 */
inline constexpr std::int32_t MAX_LOOKBACK_MAX = 16384;
/** @brief 自动可读深度上限（clamp 上界，与显式校验上限不同） */
inline constexpr std::int32_t MAX_LOOKBACK_AUTO_LIMIT = 2048;
/** @brief 回卷余量上限 */
inline constexpr std::int32_t RESERVE_BARS_MAX = 8192;
/** @brief 物理长度上限（buffer_bars = max_lookback + reserve_bars） */
inline constexpr std::int32_t BUFFER_BARS_MAX = 24576;
/** @brief 一天的秒数（自动值按 24 小时保守估算每日 bar 数） */
inline constexpr std::int32_t SECS_PER_DAY = 86400;

/**
 * @brief 每日 bar 数（保守估计：86400 / period_sec）
 *
 * v1 无交易小节表数据源，按 24 小时估算（组件文档 §4.5 与 §14 第 4 条）。交易小节表
 * 落地后这里改用真实 session 总分钟数。
 */
[[nodiscard]] inline std::int32_t daily_bars(std::int32_t period_sec) noexcept {
    return period_sec > 0 ? SECS_PER_DAY / period_sec : SECS_PER_DAY;
}

/// @brief 自动可读深度：`clamp(86400/period_sec, 32, 2048)`
[[nodiscard]] inline std::int32_t auto_max_lookback(std::int32_t period_sec) noexcept {
    const std::int32_t bars = daily_bars(period_sec);
    if (bars < MAX_LOOKBACK_MIN) {
        return MAX_LOOKBACK_MIN;
    }
    return bars > MAX_LOOKBACK_AUTO_LIMIT ? MAX_LOOKBACK_AUTO_LIMIT : bars;
}

/**
 * @brief 回卷余量下限：`max(2, ceil(1.1 × 每日 bar 数))`
 *
 * 1.1 倍保证「两次回卷之间至少攒够一整窗」，即正常推进永远走 18:00 主动回卷而不是
 * 盘中兜底（组件文档 §4.4）。整数运算避免浮点边界的取整分歧。
 */
[[nodiscard]] inline std::int32_t min_reserve_bars(std::int32_t period_sec) noexcept {
    const std::int32_t bars = daily_bars(period_sec);
    const std::int32_t ceil_110pct = (11 * bars + 9) / 10;
    return ceil_110pct > 2 ? ceil_110pct : 2;
}

/// @brief 自动回卷余量：`max(2 × 每日 bar 数, R_min)`
[[nodiscard]] inline std::int32_t auto_reserve_bars(std::int32_t period_sec) noexcept {
    const std::int32_t twice = 2 * daily_bars(period_sec);
    const std::int32_t floor_reserve = min_reserve_bars(period_sec);
    return twice > floor_reserve ? twice : floor_reserve;
}

/**
 * @brief 容量推导的输入：周期必填，两个容量量可省略（省略 = 自动）
 */
struct CapacityInput {
    std::int32_t period_sec = 60;              ///< bar 周期（秒）；配置层负责限定 60/180/300/900
    std::optional<std::int32_t> max_lookback;  ///< 省略 = 按周期自动推导
    std::optional<std::int32_t> reserve_bars;  ///< 省略 = 按周期自动推导
};

/**
 * @brief 容量推导的结果（三个量一次算齐，避免调用方各算一半）
 */
struct Capacity {
    std::int32_t max_lookback = 0;  ///< 可读历史上限（B 可见）
    std::int32_t reserve_bars = 0;  ///< 回卷余量（B 不可见）
    std::int32_t buffer_bars = 0;   ///< 物理元素数（= max_lookback + reserve_bars）
};

namespace capacity_internal {

/// @brief 非法容量的统一异常形态：字段名 / 当前值 / 合法区间（组件文档 §7）
[[noreturn]] inline void throw_invalid(const char* field,
                                       std::int32_t value,
                                       std::int32_t lower,
                                       std::int32_t upper) {
    throw std::invalid_argument(std::format("invalid capacity | field={} value={} range=[{},{}]",
                                            field, value, lower, upper));
}

}  // namespace capacity_internal

/**
 * @brief 解析容量：自动值按周期推导，显式值按区间校验，派生值 `buffer_bars` 上限校验
 *
 * 校验顺序与错误消息：
 *   1. `period_sec > 0`（配置层的取值集合校验不在这里，#156）
 *   2. `max_lookback ∈ [32, 16384]`（显式时）
 *   3. `reserve_bars ∈ [R_min, 8192]`（显式时；`R_min` 随周期变化）
 *   4. `buffer_bars = max_lookback + reserve_bars ≤ 24576`
 *
 * @throw std::invalid_argument 任一校验失败（不静默修正）
 */
[[nodiscard]] inline Capacity resolve_capacity(const CapacityInput& input) {
    if (input.period_sec <= 0) {
        capacity_internal::throw_invalid("period_sec", input.period_sec, 1, SECS_PER_DAY);
    }

    // 周期过短会让回卷余量的下限本身越过上限（[R_min, 8192] 成为空区间）：此时该周期
    // 不存在任何合法配置，报周期而不是报一个倒置的区间。
    const std::int32_t reserve_floor = min_reserve_bars(input.period_sec);
    if (reserve_floor > RESERVE_BARS_MAX) {
        capacity_internal::throw_invalid("period_sec", input.period_sec,
                                         (SECS_PER_DAY / RESERVE_BARS_MAX) + 1, SECS_PER_DAY);
    }

    Capacity capacity{};
    capacity.max_lookback =
        input.max_lookback.has_value() ? *input.max_lookback : auto_max_lookback(input.period_sec);
    if (capacity.max_lookback < MAX_LOOKBACK_MIN || capacity.max_lookback > MAX_LOOKBACK_MAX) {
        capacity_internal::throw_invalid("max_lookback", capacity.max_lookback, MAX_LOOKBACK_MIN,
                                         MAX_LOOKBACK_MAX);
    }

    capacity.reserve_bars =
        input.reserve_bars.has_value() ? *input.reserve_bars : auto_reserve_bars(input.period_sec);
    if (capacity.reserve_bars < reserve_floor || capacity.reserve_bars > RESERVE_BARS_MAX) {
        capacity_internal::throw_invalid("reserve_bars", capacity.reserve_bars, reserve_floor,
                                         RESERVE_BARS_MAX);
    }

    // 派生上限：两个字段各自合法时其和必然 ≤ 上限（16384 + 8192 = 24576），因此这里是
    // 防御性校验——为将来放宽单字段区间保留，不是当前可达分支（测试断言了这一等价）。
    capacity.buffer_bars = capacity.max_lookback + capacity.reserve_bars;
    if (capacity.buffer_bars > BUFFER_BARS_MAX) {
        capacity_internal::throw_invalid("buffer_bars", capacity.buffer_bars, 1, BUFFER_BARS_MAX);
    }
    return capacity;
}

// ============================================================
// 窗口几何驱动（组件文档 §4.1 推进 / §4.4 回卷）
// ============================================================

namespace window_internal {

/// @brief 单个数组的回卷搬移：窗口 `[end - size, end)` → `[0, size)`
template <class T>
inline void relocate_array_one(const WindowState& window, T* base) noexcept {
    if (base == nullptr || window.size <= 0 || window.end == window.size) {
        return;
    }
    const auto offset = static_cast<std::ptrdiff_t>(window.end - window.size);
    const auto count = static_cast<std::size_t>(window.size);
    std::memmove(base, base + offset, count * sizeof(T));
}

/// @brief 对全部数组逐个回卷（空指针跳过：未注册的用户序列槽位）
template <class... Arrays>
inline void relocate_all(const WindowState& window, Arrays... arrays) noexcept {
    (relocate_array_one(window, arrays), ...);
}

}  // namespace window_internal

/**
 * @brief 主动回卷（18:00 定时器主路径，也是推进前的兜底）
 *
 * 把窗口 `[end - size, end)` 搬回 `[0, size)` 并把 `end` 置为 `size`；所有数组一起搬，
 * 几何保持一致。纯布局变换：元素值、`size`、`bar_index` 全部不变。
 *
 * @param window 本品种共享的窗口几何
 * @param arrays 本品种的全部数组（7 条价格 + 全部用户序列；允许 nullptr 槽位）
 */
template <class... Arrays>
inline void relocate_window(WindowState& window, Arrays... arrays) noexcept {
    window_internal::relocate_all(window, arrays...);
    window.end = window.size;
}

/**
 * @brief 推进一根 bar 的第一步：必要时回卷，返回新 bar 的写入槽位
 *
 * 组件文档 §4.1 的顺序是「① 回卷 → ② 写 `base_[end]` → ③ `++end` / `size` 递增」，
 * 因此本函数**只**完成 ① 并返回槽位；调用方把价格/量额/用户序列写进该槽位后，
 * 必须调用 {@link commit_advance} 完成 ③。两步成对使用，否则窗口几何与已写数据错位。
 *
 * @return 新 bar 在所有数组中的物理下标（尚未计入 `size`）
 */
template <class... Arrays>
[[nodiscard]] inline std::int32_t prepare_advance(WindowState& window, Arrays... arrays) noexcept {
    if (window.end >= window.buffer_bars) {
        relocate_window(window, arrays...);  // 兜底：定时器错过 / 余量配得过小
    }
    return window.end;
}

/**
 * @brief 校验窗口几何是否自洽（内部断言/测试用）
 *
 * 不变式：`0 <= size <= max_lookback <= buffer_bars` 且 `size <= end <= buffer_bars`。
 */
[[nodiscard]] inline bool window_consistent(const WindowState& window) noexcept {
    return window.size >= 0 && window.size <= window.max_lookback &&
           window.max_lookback <= window.buffer_bars && window.end >= window.size &&
           window.end <= window.buffer_bars;
}

/**
 * @brief 推进一根 bar 的第三步：`++end`，`size` 递增并封顶于 `max_lookback`
 *
 * 封顶后窗口开始淘汰最旧的一根——淘汰只是 `size` 不再增长（零拷贝），窗口左边界随
 * `end` 前移。
 */
inline void commit_advance(WindowState& window) noexcept {
    ++window.end;
    if (window.size < window.max_lookback) {
        ++window.size;
    }
}

/**
 * @brief 校验窗口是否已具备推进条件（`buffer_bars > 0` 且几何自洽）
 *
 * `prepare_advance` 返回的是「可以写入的下标」；调用方在解引用数组前必须确认它不是
 * 越界下标。容量由 `resolve_capacity` 保证 `buffer_bars >= max_lookback + R_min >= 33`，
 * 所以真实实例永远满足；但**默认构造的 `WindowState`**（`buffer_bars == 0`）在 A 完成
 * 注入之前也会走到推进路径，那时 `prepare_advance` 会返回 0 —— 写进零长数组就是越界写。
 * 本函数把这个前提显式化，A 在推进前断言它（内部用法）或按失败处理。
 */
[[nodiscard]] inline bool window_ready_to_advance(const WindowState& window) noexcept {
    return window.buffer_bars > 0 && window_consistent(window);
}

/**
 * @brief 推进一根 bar：写槽位 → commit（调用方已校验 `window_ready_to_advance`）
 *
 * 便利包装：把 `prepare_advance` + 写入 + `commit_advance` 三步收敛到一处，避免调用方
 * 忘记 commit 导致几何与数据错位。`writer` 收到物理下标，负责把本根 bar 的值写进
 * **每个**数组。
 *
 * @param writer 回调：`void(std::int32_t slot)`
 */
template <class Writer, class... Arrays>
inline void advance_bar(WindowState& window, Writer&& writer, Arrays... arrays) noexcept {
    const std::int32_t slot = prepare_advance(window, arrays...);
    writer(slot);
    commit_advance(window);
}

}  // namespace dztrader::series_internal

#endif  // DZTRADER_SERIES_INTERNAL_H_
