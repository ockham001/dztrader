/**
 * @file series_window_test.cpp
 * @brief 容量推导与窗口几何：推进、回卷等价性、非法容量报错（issue #155 AC5/AC6，
 *        组件文档 §4.1/§4.4/§4.5）
 *
 * 断言只落在可观察契约上：`[n]` 与 `span()` 的逐位内容、`size` / `end` / `bar_index`
 * 等价性、异常类型与消息字段。回卷用例的 oracle 是**独立的影子模型**（`std::deque`
 * 追加 + 裁剪），不与被测实现共享几何推导，避免同义反复。
 */
#include <dztrader/series.h>
#include <dztrader/series_internal.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using dztrader::Series;
using dztrader::SeriesView;
using dztrader::WindowState;
using dztrader::series_internal::Access;
using dztrader::series_internal::Capacity;
using dztrader::series_internal::CapacityInput;
using dztrader::series_internal::commit_advance;
using dztrader::series_internal::prepare_advance;
using dztrader::series_internal::relocate_window;
using dztrader::series_internal::resolve_capacity;
using dztrader::series_internal::window_consistent;

/// @brief 一个品种的窗口夹具：价格数组 + 用户序列数组 + 共享几何
///
/// 模拟 InstrumentContext 里「价格序列与用户序列共用一个 WindowState」的布局，
/// 推进/回卷时把**全部**数组一起交给驱动函数——这是 AC5 的核心断言对象。
class WindowFixture {
public:
    WindowFixture(std::int32_t max_lookback, std::int32_t reserve_bars)
        : capacity_{.max_lookback = max_lookback,
                    .reserve_bars = reserve_bars,
                    .buffer_bars = max_lookback + reserve_bars},
          close_(static_cast<std::size_t>(capacity_.buffer_bars), 0.0),
          high_(static_cast<std::size_t>(capacity_.buffer_bars), 0.0),
          user_(static_cast<std::size_t>(capacity_.buffer_bars), -1) {
        window_.max_lookback = capacity_.max_lookback;
        window_.buffer_bars = capacity_.buffer_bars;
    }

    /// @brief 推进一根 bar：写价格/用户序列 → commit（复刻 A 的推进序列）
    void advance(double close, double high, std::int64_t user) {
        const auto slot = prepare_advance(window_, close_.data(), high_.data(), user_.data());
        const auto index = static_cast<std::size_t>(slot);
        close_[index] = close;
        high_[index] = high;
        user_[index] = user;
        commit_advance(window_);
        ++bar_index_;
    }

    // 视图绑定的是「数组首地址 + 窗口」，与被绑对象的 const 性无关；作为只读视图，
    // 它就是要在 const 夹具上可读，因此这里从 const 成员取非 const 地址是刻意的。
    [[nodiscard]] SeriesView<double> close_view() const { return close_view_impl(); }
    [[nodiscard]] SeriesView<double> high_view() const { return high_view_impl(); }
    [[nodiscard]] SeriesView<std::int64_t> user_view() const { return user_view_impl(); }
    [[nodiscard]] Series<std::int64_t> user_series() {
        return Access::series(user_.data(), &window_);
    }

    [[nodiscard]] const WindowState& window() const { return window_; }
    [[nodiscard]] std::int64_t bar_index() const { return bar_index_; }
    [[nodiscard]] const Capacity& capacity() const { return capacity_; }
    [[nodiscard]] const std::vector<double>& close_storage() const { return close_; }
    [[nodiscard]] const std::vector<double>& high_storage() const { return high_; }
    [[nodiscard]] const std::vector<std::int64_t>& user_storage() const { return user_; }

    /// @brief 主动回卷（18:00 主路径）：全部数组一起搬
    void relocate() { relocate_window(window_, close_.data(), high_.data(), user_.data()); }

private:
    [[nodiscard]] SeriesView<double> close_view_impl() const {
        return Access::view(const_cast<double*>(close_.data()), &window_);
    }
    [[nodiscard]] SeriesView<double> high_view_impl() const {
        return Access::view(const_cast<double*>(high_.data()), &window_);
    }
    [[nodiscard]] SeriesView<std::int64_t> user_view_impl() const {
        return Access::view(const_cast<std::int64_t*>(user_.data()), &window_);
    }

    Capacity capacity_{};
    WindowState window_{};
    std::int64_t bar_index_ = 0;
    std::vector<double> close_;
    std::vector<double> high_;
    std::vector<std::int64_t> user_;
};

/// @brief 影子模型：语义等价于「每根 bar push_back + 满了 pop_front」
class ShadowWindow {
public:
    explicit ShadowWindow(std::int32_t max_lookback)
        : max_lookback_(max_lookback) {}

    void advance(double close, double high, std::int64_t user) {
        close_.push_back(close);
        high_.push_back(high);
        user_.push_back(user);
        if (static_cast<std::int32_t>(close_.size()) > max_lookback_) {
            close_.pop_front();
            high_.pop_front();
            user_.pop_front();
        }
    }

    [[nodiscard]] std::vector<double> close_snapshot() const {
        return {close_.begin(), close_.end()};
    }
    [[nodiscard]] std::vector<double> close_reverse_snapshot() const {
        return {close_.rbegin(), close_.rend()};
    }
    [[nodiscard]] std::int32_t size() const { return static_cast<std::int32_t>(close_.size()); }
    [[nodiscard]] std::vector<std::int64_t> user_snapshot() const {
        return {user_.begin(), user_.end()};
    }

private:
    std::int32_t max_lookback_;
    std::deque<double> close_;
    std::deque<double> high_;
    std::deque<std::int64_t> user_;
};

// ---------------- AC5：buffer_bars = max_lookback + reserve_bars ----------------

TEST(SeriesBufferGeometryTest, BufferBarsIsLookbackPlusReserve) {
    WindowFixture fixture(/*max_lookback=*/5, /*reserve_bars=*/3);

    EXPECT_EQ(fixture.capacity().buffer_bars, 8);
    EXPECT_EQ(fixture.capacity().max_lookback, 5);
    EXPECT_EQ(fixture.capacity().reserve_bars, 3);
    EXPECT_TRUE(window_consistent(fixture.window()));
}

TEST(SeriesBufferGeometryTest, WindowSlidesWithoutMovingData) {
    WindowFixture fixture(/*max_lookback=*/3, /*reserve_bars=*/5);
    for (int i = 0; i < 3; ++i) {
        fixture.advance(100.0 + i, 200.0 + i, i);
    }
    const auto before = fixture.close_storage();

    fixture.advance(999.0, 999.0, 99);

    // 推进只改两个整数：前 3 根的物理字节一字未动
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(fixture.close_storage()[i], before[i]);
    }
    EXPECT_EQ(fixture.window().size, 3);  // 已满，不再增长
    EXPECT_EQ(fixture.window().end, 4);
}

TEST(SeriesBufferGeometryTest, SizeStopsAtMaxLookbackAndEvictsOldest) {
    WindowFixture fixture(/*max_lookback=*/3, /*reserve_bars=*/5);
    ShadowWindow shadow(3);
    for (int i = 0; i < 7; ++i) {
        fixture.advance(static_cast<double>(i), static_cast<double>(i), i);
        shadow.advance(static_cast<double>(i), static_cast<double>(i), i);
    }

    const auto view = fixture.close_view();
    ASSERT_EQ(view.size(), shadow.size());
    EXPECT_EQ(view.size(), 3);
    EXPECT_EQ(view[0], 6.0);
    EXPECT_EQ(view[1], 5.0);
    EXPECT_EQ(view[2], 4.0);
    EXPECT_EQ(std::vector<double>(view.span().begin(), view.span().end()), shadow.close_snapshot());
}

// ---------------- AC5：回卷前后逐位一致 ----------------

/// @brief 断言整窗在 `[n]` 与 `span()` 两个口径上都与影子模型逐位一致
void expect_window_matches_shadow(const WindowFixture& fixture, const ShadowWindow& shadow) {
    const auto close_view = fixture.close_view();
    const auto user_view = fixture.user_view();

    ASSERT_EQ(close_view.size(), shadow.size());
    ASSERT_EQ(static_cast<std::int32_t>(close_view.span().size()), shadow.size());

    // span()：时间正序
    EXPECT_EQ(std::vector<double>(close_view.span().begin(), close_view.span().end()),
              shadow.close_snapshot());
    // [n]：往回数，与 span() 互为镜像
    for (std::int32_t n = 0; n < shadow.size(); ++n) {
        SCOPED_TRACE(n);
        EXPECT_EQ(close_view[static_cast<std::ptrdiff_t>(n)],
                  shadow.close_reverse_snapshot()[static_cast<std::size_t>(n)]);
    }
    // 用户序列与价格序列共享几何：长度与内容必须同步
    EXPECT_EQ(std::vector<std::int64_t>(user_view.span().begin(), user_view.span().end()),
              shadow.user_snapshot());
}

TEST(SeriesRelocateTest, RelocatePreservesEveryReadValue) {
    WindowFixture fixture(/*max_lookback=*/5, /*reserve_bars=*/6);
    ShadowWindow shadow(5);
    for (int i = 0; i < 8; ++i) {
        fixture.advance(static_cast<double>(i), static_cast<double>(i) * 2, i * 10);
        shadow.advance(static_cast<double>(i), static_cast<double>(i) * 2, i * 10);
    }
    // 前置：确实处于「窗口已滑动」状态（end > size），否则回卷是空操作
    ASSERT_GT(fixture.window().end, fixture.window().size);

    const auto size_before = fixture.window().size;
    const auto end_before = fixture.window().end;
    const auto bar_index_before = fixture.bar_index();

    fixture.relocate();

    EXPECT_EQ(fixture.window().size, size_before) << "回卷不得改 size";
    EXPECT_EQ(fixture.window().end, size_before) << "回卷后 end 必须等于 size";
    EXPECT_LT(fixture.window().end, end_before) << "回卷确实把窗口搬回了头部";
    EXPECT_EQ(fixture.bar_index(), bar_index_before) << "回卷不得改 bar_index";
    expect_window_matches_shadow(fixture, shadow);
}

TEST(SeriesRelocateTest, RelocateIsIdempotentWhenWindowAlreadyAtHead) {
    WindowFixture fixture(/*max_lookback=*/4, /*reserve_bars=*/4);
    ShadowWindow shadow(4);
    for (int i = 0; i < 3; ++i) {
        fixture.advance(static_cast<double>(i), 0.0, i);
        shadow.advance(static_cast<double>(i), 0.0, i);
    }
    ASSERT_EQ(fixture.window().end, fixture.window().size);  // 已无滑动

    fixture.relocate();
    EXPECT_EQ(fixture.window().end, fixture.window().size);
    expect_window_matches_shadow(fixture, shadow);
}

TEST(SeriesRelocateTest, RelocateOfEmptyWindowIsHarmless) {
    WindowFixture fixture(/*max_lookback=*/4, /*reserve_bars=*/4);

    fixture.relocate();

    EXPECT_EQ(fixture.window().size, 0);
    EXPECT_EQ(fixture.window().end, 0);
    EXPECT_TRUE(window_consistent(fixture.window()));
}

TEST(SeriesRelocateTest, RelocateThenAdvanceContinuesCleanly) {
    WindowFixture fixture(/*max_lookback=*/4, /*reserve_bars=*/8);
    ShadowWindow shadow(4);
    for (int i = 0; i < 6; ++i) {
        fixture.advance(static_cast<double>(i), 0.0, i);
        shadow.advance(static_cast<double>(i), 0.0, i);
    }
    fixture.relocate();

    // 回卷后继续推进：新 bar 落在 size 位置，旧值照常淘汰
    for (int i = 6; i < 12; ++i) {
        fixture.advance(static_cast<double>(i), 0.0, i);
        shadow.advance(static_cast<double>(i), 0.0, i);
        expect_window_matches_shadow(fixture, shadow);
        EXPECT_TRUE(window_consistent(fixture.window()));
    }
}

TEST(SeriesRelocateTest, RelocateMidFillLeavesPartialWindowIntact) {
    // 静默填窗期（窗口未满）回卷同样合法（组件文档 §4.4：任何两次操作之间）
    WindowFixture fixture(/*max_lookback=*/8, /*reserve_bars=*/8);
    ShadowWindow shadow(8);
    for (int i = 0; i < 3; ++i) {
        fixture.advance(static_cast<double>(i), 0.0, i);
        shadow.advance(static_cast<double>(i), 0.0, i);
    }

    fixture.relocate();

    EXPECT_EQ(fixture.window().size, 3);
    EXPECT_EQ(fixture.window().end, 3);
    expect_window_matches_shadow(fixture, shadow);
}

// ---------------- AC5：物理缓冲用尽时自动兜底回卷 ----------------

TEST(SeriesRelocateTest, FallbackRelocateHappensWhenPhysicalBufferRunsOut) {
    // reserve_bars 配到最小：推进到 buffer_bars 边界必须自动回卷，否则越界写
    WindowFixture fixture(/*max_lookback=*/4, /*reserve_bars=*/1);
    ShadowWindow shadow(4);

    for (int i = 0; i < 40; ++i) {
        fixture.advance(static_cast<double>(i), 0.0, i);
        shadow.advance(static_cast<double>(i), 0.0, i);
        ASSERT_TRUE(window_consistent(fixture.window())) << "i=" << i;
        ASSERT_LE(fixture.window().end, fixture.window().buffer_bars) << "i=" << i;
        expect_window_matches_shadow(fixture, shadow);
    }
}

TEST(SeriesRelocateTest, AdvanceAfterRelocateNeverWritesPastBuffer) {
    WindowFixture fixture(/*max_lookback=*/2, /*reserve_bars=*/1);
    // 逐根检查：end 永远不超过 buffer_bars（回卷兜底生效）
    for (int i = 0; i < 20; ++i) {
        fixture.advance(static_cast<double>(i), 0.0, i);
        EXPECT_LE(fixture.window().end, fixture.window().buffer_bars);
    }
    EXPECT_EQ(fixture.close_view().size(), 2);
}

// ---------------- AC5：回卷可发生在任意两次操作之间（随机推进 fuzz） ----------------

TEST(SeriesRelocateFuzzTest, RandomRelocationsMatchShadowModel) {
    std::mt19937 rng(20261027U);
    std::uniform_int_distribution<int> bar_count_dist(1, 500);
    std::uniform_int_distribution<int> relocate_chance_dist(0, 99);

    // 多组容量配比，覆盖「余量极大 / 余量最小 / 窗口未满」三类
    const std::vector<std::pair<std::int32_t, std::int32_t>> configs = {
        {32, 2}, {32, 33}, {96, 1}, {96, 200}, {2048, 1}, {7, 3}, {1, 1}};

    for (const auto& [max_lookback, reserve_bars] : configs) {
        SCOPED_TRACE(std::to_string(max_lookback) + "/" + std::to_string(reserve_bars));
        WindowFixture fixture(max_lookback, reserve_bars);
        ShadowWindow shadow(max_lookback);

        std::int64_t bar = 0;
        for (int round = 0; round < 20; ++round) {
            const int bars = bar_count_dist(rng);
            for (int i = 0; i < bars; ++i) {
                const double close = static_cast<double>(bar) * 1.5 + 0.25;
                fixture.advance(close, close + 1.0, bar * 3);
                shadow.advance(close, close + 1.0, bar * 3);
                ++bar;

                ASSERT_TRUE(window_consistent(fixture.window()));
                ASSERT_LE(fixture.window().end, fixture.window().buffer_bars);
            }
            // 回卷可发生在任意两次操作之间：随机插入主动回卷
            if (relocate_chance_dist(rng) < 50) {
                fixture.relocate();
                ASSERT_TRUE(window_consistent(fixture.window()));
                ASSERT_EQ(fixture.window().end, fixture.window().size);
            }
            expect_window_matches_shadow(fixture, shadow);
        }
    }
}

TEST(SeriesRelocateFuzzTest, RelocateAtEveryPossiblePointIsInvisible) {
    // 穷举：同一 tick 序列下，「每根都回卷」与「从不回卷」的可读状态必须完全相同
    constexpr std::int32_t kMaxLookback = 8;
    constexpr std::int32_t kReserve = 3;
    constexpr int kBars = 60;

    WindowFixture eager(kMaxLookback, kReserve);
    WindowFixture lazy(kMaxLookback, kReserve);
    ShadowWindow shadow(kMaxLookback);

    for (int i = 0; i < kBars; ++i) {
        const double value = static_cast<double>(i) * 0.5;
        eager.advance(value, value, i);
        lazy.advance(value, value, i);
        shadow.advance(value, value, i);
        eager.relocate();  // 每根 bar 之间都主动回卷（B 不可见）

        ASSERT_TRUE(window_consistent(eager.window()));
        expect_window_matches_shadow(eager, shadow);
        expect_window_matches_shadow(lazy, shadow);
        EXPECT_EQ(eager.close_view().size(), lazy.close_view().size());
        EXPECT_EQ(eager.bar_index(), lazy.bar_index());
    }
}

// ---------------- AC6：容量自动值按周期推导 ----------------

TEST(SeriesCapacityTest, AutoValuesMatchPerPeriodTable) {
    // 组件文档 §4.5 自动值表的四个 v1 周期，逐列对照（表值来自文档，不由实现推导）
    struct Row {
        std::int32_t period_sec;
        std::int32_t max_lookback;
        std::int32_t reserve_bars;
        std::int32_t buffer_bars;
    };
    const Row rows[] = {
        {60, 1440, 2880, 4320},
        {180, 480, 960, 1440},
        {300, 288, 576, 864},
        {900, 96, 192, 288},
    };

    for (const Row& row : rows) {
        SCOPED_TRACE(row.period_sec);
        CapacityInput input{};
        input.period_sec = row.period_sec;
        const auto capacity = resolve_capacity(input);

        EXPECT_EQ(capacity.max_lookback, row.max_lookback);
        EXPECT_EQ(capacity.reserve_bars, row.reserve_bars);
        EXPECT_EQ(capacity.buffer_bars, row.buffer_bars);
        EXPECT_EQ(capacity.buffer_bars, capacity.max_lookback + capacity.reserve_bars);
    }
}

TEST(SeriesCapacityTest, AutoMaxLookbackIsClampedIntoAutoRange) {
    // 周期短到每日 bar 数超过自动上界：86400/42 = 2057 → clamp 到 2048
    CapacityInput short_period{};
    short_period.period_sec = 42;
    EXPECT_EQ(resolve_capacity(short_period).max_lookback, 2048);

    // 周期长到每日 bar 数不足下限：86400/7200 = 12 → clamp 到 32
    CapacityInput long_period{};
    long_period.period_sec = 7200;
    EXPECT_EQ(resolve_capacity(long_period).max_lookback, 32);
}

TEST(SeriesCapacityTest, ReserveFloorIsAtLeastTwoAndCoversOneDay) {
    // R_min = max(2, ceil(1.1 × 每日 bar 数))
    CapacityInput one_minute{};
    one_minute.period_sec = 60;
    const auto minute_capacity = resolve_capacity(one_minute);
    EXPECT_GE(minute_capacity.reserve_bars, 1584) << "1.1 × 1440 = 1584";
    EXPECT_EQ(minute_capacity.reserve_bars, 2880) << "自动值是 2 × 每日 bar 数";

    // 周期长于一天时 R_min 退化为下限 2
    CapacityInput slow{};
    slow.period_sec = 86400;
    const auto slow_capacity = resolve_capacity(slow);
    EXPECT_EQ(slow_capacity.reserve_bars, 2);
}

// ---------------- AC6：显式覆盖按区间校验 ----------------

TEST(SeriesCapacityTest, ExplicitValuesOverrideAuto) {
    CapacityInput input{};
    input.period_sec = 60;
    input.max_lookback = 500;   // 自动值应为 1440
    input.reserve_bars = 1584;  // 自动值应为 2880，1584 恰为 R_min
    const auto capacity = resolve_capacity(input);

    EXPECT_EQ(capacity.max_lookback, 500);
    EXPECT_EQ(capacity.reserve_bars, 1584);
    EXPECT_EQ(capacity.buffer_bars, 2084);
}

TEST(SeriesCapacityTest, MaxLookbackBelowMinimumIsRejected) {
    CapacityInput input{};
    input.period_sec = 60;
    input.max_lookback = 31;
    EXPECT_THROW((void)resolve_capacity(input), std::invalid_argument);
}

TEST(SeriesCapacityTest, MaxLookbackAboveMaximumIsRejected) {
    CapacityInput input{};
    input.period_sec = 60;
    input.max_lookback = 16385;
    EXPECT_THROW((void)resolve_capacity(input), std::invalid_argument);
}

TEST(SeriesCapacityTest, MaxLookbackBoundsAreInclusive) {
    CapacityInput low{};
    low.period_sec = 900;  // R_min = 106
    low.max_lookback = 32;
    low.reserve_bars = 106;
    EXPECT_EQ(resolve_capacity(low).max_lookback, 32);

    CapacityInput high{};
    high.period_sec = 900;
    high.max_lookback = 16384;
    high.reserve_bars = 8192;  // 和恰为上限 24576
    EXPECT_EQ(resolve_capacity(high).max_lookback, 16384);
}

TEST(SeriesCapacityTest, ReserveBarsBelowPeriodFloorIsRejected) {
    // R_min 随周期变化：1 分钟周期下 1584 是下限
    CapacityInput input{};
    input.period_sec = 60;
    input.reserve_bars = 1583;
    EXPECT_THROW((void)resolve_capacity(input), std::invalid_argument);

    CapacityInput at_floor{};
    at_floor.period_sec = 60;
    at_floor.reserve_bars = 1584;
    EXPECT_EQ(resolve_capacity(at_floor).reserve_bars, 1584);
}

TEST(SeriesCapacityTest, ReserveBarsAboveMaximumIsRejected) {
    CapacityInput input{};
    input.period_sec = 60;
    input.reserve_bars = 8193;
    EXPECT_THROW((void)resolve_capacity(input), std::invalid_argument);
}

TEST(SeriesCapacityTest, BufferBarsLimitIsSubsumedByPerFieldBounds) {
    // 文档要求 buffer_bars ≤ 24576，但两个字段各自合法时和永远不超过它：
    // max_lookback ≤ 16384、reserve_bars ≤ 8192，最大值相加恰为 24576。
    // 因此派生上限是**防御性**校验（为将来放宽单字段区间保留），不是可达分支；
    // 这里断言的是「上界恰好在字段上限处取到」这个事实，而不是伪造一个非法组合。
    static_assert(
        dztrader::series_internal::MAX_LOOKBACK_MAX + dztrader::series_internal::RESERVE_BARS_MAX ==
            dztrader::series_internal::BUFFER_BARS_MAX,
        "buffer_bars limit must equal the sum of the per-field limits");

    CapacityInput at_limit{};
    at_limit.period_sec = 900;
    at_limit.max_lookback = 16384;
    at_limit.reserve_bars = 8192;
    const auto capacity = resolve_capacity(at_limit);
    EXPECT_EQ(capacity.buffer_bars, dztrader::series_internal::BUFFER_BARS_MAX);
    EXPECT_EQ(capacity.buffer_bars, 24576);
}

TEST(SeriesCapacityTest, BufferBarsIsAlwaysDerivedFromTheTwoFields) {
    // 派生的唯一来源是两个字段之和，不接受独立配置（避免三处定义同一事实）
    for (const std::int32_t period : {60, 180, 300, 900}) {
        SCOPED_TRACE(period);
        CapacityInput input{};
        input.period_sec = period;
        const auto capacity = resolve_capacity(input);
        EXPECT_EQ(capacity.buffer_bars, capacity.max_lookback + capacity.reserve_bars);
    }
}

TEST(SeriesCapacityTest, AutoValueOutOfRangeIsRejectedNotSilentlyClamped) {
    // 组件文档 §7：「所有自动值也走同一套校验，越界 → 报错并提示显式配置，不静默越界」。
    CapacityInput input{};
    input.period_sec = 10;  // R_auto = 2 × 8640 = 17280 > 8192
    EXPECT_THROW((void)resolve_capacity(input), std::invalid_argument);
}

TEST(SeriesCapacityTest, PeriodWithEmptyReserveIntervalIsRejectedAsPeriodError) {
    // 周期短到 R_min 本身超过 8192 时，[R_min, 8192] 是空区间：该周期没有任何合法配置。
    // 显式覆盖救不了它（下限是「至少攒一天 bar」的物理要求），所以报的是 period_sec 而不是
    // 一个倒置的区间——错误信息必须给出调用方真正能改的那个字段。
    CapacityInput overridden{};
    overridden.period_sec = 10;
    overridden.reserve_bars = 8192;

    try {
        (void)resolve_capacity(overridden);
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("field=period_sec"), std::string::npos) << message;
        EXPECT_NE(message.find("value=10"), std::string::npos) << message;
        // 合法区间必须是正区间（lower < upper），不能给出倒置的 [9504,8192]
        const auto range_pos = message.find("range=[");
        ASSERT_NE(range_pos, std::string::npos) << message;
        const auto comma_pos = message.find(',', range_pos);
        ASSERT_NE(comma_pos, std::string::npos) << message;
        const auto close_pos = message.find(']', comma_pos);
        ASSERT_NE(close_pos, std::string::npos) << message;
        const auto lower = std::stoi(message.substr(range_pos + 7, comma_pos - range_pos - 7));
        const auto upper = std::stoi(message.substr(comma_pos + 1, close_pos - comma_pos - 1));
        EXPECT_LT(lower, upper) << "错误信息给出了倒置区间: " << message;
    }
}

TEST(SeriesCapacityTest, NonPositivePeriodIsRejected) {
    CapacityInput zero{};
    zero.period_sec = 0;
    EXPECT_THROW((void)resolve_capacity(zero), std::invalid_argument);

    CapacityInput negative{};
    negative.period_sec = -60;
    EXPECT_THROW((void)resolve_capacity(negative), std::invalid_argument);
}

TEST(SeriesCapacityTest, ErrorMessageCarriesFieldNameCurrentValueAndRange) {
    CapacityInput input{};
    input.period_sec = 60;
    input.max_lookback = 10;  // < 32

    try {
        (void)resolve_capacity(input);
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("field=max_lookback"), std::string::npos) << message;
        EXPECT_NE(message.find("value=10"), std::string::npos) << message;
        EXPECT_NE(message.find("range=[32,16384]"), std::string::npos) << message;
    }
}

TEST(SeriesCapacityTest, ReserveErrorCarriesPeriodDependentFloor) {
    CapacityInput input{};
    input.period_sec = 60;   // R_min = 1584
    input.reserve_bars = 1;  // 远低于下限

    try {
        (void)resolve_capacity(input);
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("field=reserve_bars"), std::string::npos) << message;
        EXPECT_NE(message.find("value=1"), std::string::npos) << message;
        EXPECT_NE(message.find("range=[1584,8192]"), std::string::npos) << message;
    }
}

// ---------------- AC5：回卷不改变 bar_index（绝对序号单调） ----------------

TEST(SeriesRelocateTest, BarIndexIsIndependentOfRelocation) {
    WindowFixture fixture(/*max_lookback=*/3, /*reserve_bars=*/2);
    std::int64_t expected = 0;
    for (int i = 0; i < 30; ++i) {
        fixture.advance(static_cast<double>(i), 0.0, i);
        ++expected;
        EXPECT_EQ(fixture.bar_index(), expected);
        if (i % 7 == 0) {
            fixture.relocate();
            EXPECT_EQ(fixture.bar_index(), expected) << "回卷不得重置绝对序号";
        }
    }
}

}  // namespace
