/**
 * @file series_test.cpp
 * @brief Series 层读/写语义与公开面不变量（issue #155 AC1/AC2/AC3/AC4，组件文档 §4.2）
 *
 * 断言只落在公开契约上：`[n]` 的读值、越界异常类型与消息内容、`span()` 的顺序与长度、
 * `[0]` 写入，以及静态断言锁定的零开销面。构造走内部缝
 * `series_internal::Access`（与将来的 `InstrumentContext` 同一入口）。
 *
 * 真实窗口推进/回卷的等价性在 series_window_test.cpp。
 */
#include <dztrader/series.h>
#include <dztrader/series_internal.h>

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using dztrader::Series;
using dztrader::SeriesRef;
using dztrader::SeriesView;
using dztrader::WindowState;
using dztrader::series_internal::Access;

// ---------------- 成员探测：锁定「不提供 vector 式写接口」 ----------------

template <class T, class = void>
struct has_push_back : std::false_type {};
template <class T>
struct has_push_back<T, std::void_t<decltype(std::declval<T&>().push_back(0.0))>> : std::true_type {
};

template <class T, class = void>
struct has_resize : std::false_type {};
template <class T>
struct has_resize<T, std::void_t<decltype(std::declval<T&>().resize(1))>> : std::true_type {};

template <class T, class = void>
struct has_reserve : std::false_type {};
template <class T>
struct has_reserve<T, std::void_t<decltype(std::declval<T&>().reserve(1))>> : std::true_type {};

template <class T, class = void>
struct has_at : std::false_type {};
template <class T>
struct has_at<T, std::void_t<decltype(std::declval<T&>().at(0))>> : std::true_type {};

template <class T, class = void>
struct has_erase : std::false_type {};
template <class T>
struct has_erase<T, std::void_t<decltype(std::declval<T&>().erase(0))>> : std::true_type {};

/// @brief Series 层用例的公共夹具：装 Debug 断言钩子（断言先于抛出，否则会 abort）
class SeriesTest : public ::testing::Test {
protected:
    void SetUp() override { dztrader::series_internal::debug_assert_hook() = &count_assert_; }
    void TearDown() override { dztrader::series_internal::debug_assert_hook() = nullptr; }

    /// @brief 断言计数（静态：钩子是无捕获函数指针，不能绑成员）
    static int& assert_hits() {
        static int hits = 0;
        return hits;
    }
    static void count_assert_() { ++assert_hits(); }

    /// @brief 造一个已填 `size` 根的窗口，值 = 物理下标
    void bind_window(std::int32_t size, std::int32_t max_lookback, std::int32_t buffer_bars) {
        storage_.assign(static_cast<std::size_t>(buffer_bars), 0.0);
        for (std::int32_t i = 0; i < buffer_bars; ++i) {
            storage_[static_cast<std::size_t>(i)] = static_cast<double>(i);
        }
        window_ = WindowState{
            .end = size, .size = size, .max_lookback = max_lookback, .buffer_bars = buffer_bars};
    }

    std::vector<double> storage_;
    WindowState window_{};
};

// ---------------- AC1：读值、size、max_lookback ----------------

TEST_F(SeriesTest, IndexZeroIsCurrentBarAndIndexNGrowsBackwards) {
    bind_window(/*size=*/5, /*max_lookback=*/5, /*buffer_bars=*/8);
    const auto view = Access::view(storage_.data(), &window_);

    EXPECT_EQ(view[0], 4.0);  // [0] = 当前 bar = 最后写入的一根
    EXPECT_EQ(view[1], 3.0);
    EXPECT_EQ(view[4], 0.0);
    EXPECT_EQ(view.size(), 5);
    EXPECT_EQ(view.max_lookback(), 5);
}

TEST_F(SeriesTest, WindowExcludesPhysicalSlotsBeforeLeftEdge) {
    // end > size：窗口物理占 [3, 6)，[size-1] 是最旧可读的一根
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    window_.end = 6;
    const auto view = Access::view(storage_.data(), &window_);

    EXPECT_EQ(view[0], 5.0);
    EXPECT_EQ(view[2], 3.0);
    EXPECT_EQ(view.size(), 3);
}

// ---------------- AC2：越界一律抛，消息含 n / size / max_lookback ----------------

TEST_F(SeriesTest, NegativeIndexThrowsOutOfRange) {
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    const auto view = Access::view(storage_.data(), &window_);

    EXPECT_THROW((void)view[-1], std::out_of_range);
}

TEST_F(SeriesTest, IndexAtSizeThrowsOutOfRange) {
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    const auto view = Access::view(storage_.data(), &window_);

    EXPECT_THROW((void)view[3], std::out_of_range);
}

TEST_F(SeriesTest, IndexBeyondMaxLookbackThrowsOutOfRange) {
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    const auto view = Access::view(storage_.data(), &window_);

    // 物理数组里还有 reserve 区，但超出可读深度必须抛——不读未填的余量
    EXPECT_THROW((void)view[7], std::out_of_range);
}

TEST_F(SeriesTest, OutOfRangeMessageCarriesNSizeMaxLookback) {
    bind_window(/*size=*/4, /*max_lookback=*/4, /*buffer_bars=*/8);
    const auto view = Access::view(storage_.data(), &window_);

    try {
        (void)view[9];
        FAIL() << "expected std::out_of_range";
    } catch (const std::out_of_range& error) {
        const std::string message = error.what();
        // 三个诊断量缺一不可：用户要能据此判断该调大哪个配置项
        EXPECT_NE(message.find("n=9"), std::string::npos) << message;
        EXPECT_NE(message.find("size=4"), std::string::npos) << message;
        EXPECT_NE(message.find("max_lookback=4"), std::string::npos) << message;
    }
}

TEST_F(SeriesTest, OutOfRangeHitsDebugAssertBeforeThrowing) {
    bind_window(/*size=*/2, /*max_lookback=*/2, /*buffer_bars=*/8);
    const auto view = Access::view(storage_.data(), &window_);
    assert_hits() = 0;

    EXPECT_THROW((void)view[2], std::out_of_range);
    EXPECT_EQ(assert_hits(), 1) << "Debug 断言必须先于抛出（组件文档 §4.2）";

    EXPECT_EQ(view[1], 0.0);  // 合法读不触发断言（[1] 映射到物理槽位 end-2）
    EXPECT_EQ(assert_hits(), 1);
}

TEST_F(SeriesTest, EmptyWindowThrowsOnAnyRead) {
    // on_init 期窗口未填：任何读都抛，不返回初值、不返回 NaN
    bind_window(/*size=*/0, /*max_lookback=*/32, /*buffer_bars=*/96);
    const auto view = Access::view(storage_.data(), &window_);

    EXPECT_EQ(view.size(), 0);
    EXPECT_THROW((void)view[0], std::out_of_range);
}

TEST_F(SeriesTest, DefaultConstructedViewIsInertNotCrashing) {
    // B 侧常见的默认构造形态：Series 可默认构造（随后由 ctx.make_series 赋值）。
    // 未绑定状态下 size/max_lookback 为 0、span() 为空、读元素响亮失败。
    Series<double> series;
    const SeriesView<double>& view = series;
    EXPECT_EQ(view.size(), 0);
    EXPECT_EQ(view.max_lookback(), 0);
    EXPECT_TRUE(view.span().empty());
    EXPECT_THROW((void)view[0], std::out_of_range);
}

// ---------------- AC1：span() 时间正序、长度 = size、零检查 ----------------

TEST_F(SeriesTest, SpanIsChronologicalAndCoversWindow) {
    bind_window(/*size=*/4, /*max_lookback=*/4, /*buffer_bars=*/8);
    window_.end = 6;  // 窗口 [2, 6)
    const auto view = Access::view(storage_.data(), &window_);

    const std::span<const double> window = view.span();
    ASSERT_EQ(window.size(), static_cast<std::size_t>(view.size()));
    // 时间正序（最旧→最新），与 [n] 的往回口径互为镜像
    EXPECT_EQ(window.front(), view[3]);
    EXPECT_EQ(window.back(), view[0]);
    EXPECT_EQ(std::vector<double>(window.begin(), window.end()),
              (std::vector<double>{2.0, 3.0, 4.0, 5.0}));
}

TEST_F(SeriesTest, SpanFeedsStlAlgorithms) {
    bind_window(/*size=*/5, /*max_lookback=*/5, /*buffer_bars=*/8);
    const auto view = Access::view(storage_.data(), &window_);

    // 卖点之一：无逐元素分支即可直接喂 STL / 向量化
    EXPECT_DOUBLE_EQ(std::accumulate(view.span().begin(), view.span().end(), 0.0), 10.0);
}

TEST_F(SeriesTest, SpanOfEmptyWindowIsEmpty) {
    bind_window(/*size=*/0, /*max_lookback=*/32, /*buffer_bars=*/96);
    const auto view = Access::view(storage_.data(), &window_);

    EXPECT_TRUE(view.span().empty());
}

TEST_F(SeriesTest, SpanOfFullWindowIsCompleteAndUnchecked) {
    // span() 是零检查快路径：整窗可读，长度严格等于 size
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/3);
    const auto view = Access::view(storage_.data(), &window_);

    const auto window = view.span();
    ASSERT_EQ(window.size(), 3u);
    EXPECT_EQ(window[0], 0.0);
    EXPECT_EQ(window[2], 2.0);
}

// ---------------- AC3：Series 只增加写 [0] ----------------

TEST_F(SeriesTest, SeriesWritesCurrentBarOnly) {
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    auto series = Access::series(storage_.data(), &window_);

    series[0] = 42.0;
    EXPECT_EQ(storage_[2], 42.0);  // 物理槽位 = end - 1
    EXPECT_EQ(static_cast<double>(series[0]), 42.0);
}

TEST_F(SeriesTest, CarryAssignmentCopiesValueAndLeavesHistoryIntact) {
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    auto series = Access::series(storage_.data(), &window_);

    // X[0] = X[1] 必须是「把 [1] 的值写进 [0]」，不是代理对象之间的指针拷贝
    series[0] = series[1];
    EXPECT_EQ(storage_[2], 1.0) << "carry 未写入当前 bar（代理被逐成员拷贝了）";
    EXPECT_EQ(storage_[1], 1.0) << "历史位被改写";
    EXPECT_EQ(storage_[0], 0.0) << "历史位被改写";
}

TEST_F(SeriesTest, WriteToHistorySlotStartsAtWriteProxyTarget) {
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    auto series = Access::series(storage_.data(), &window_);

    series[0] = 77.0;
    EXPECT_EQ(static_cast<double>(series[0]), 77.0);
    EXPECT_EQ(static_cast<double>(series[1]), 1.0);  // [0] 的写入不影响 [1]
}

TEST_F(SeriesTest, WriteToHistoryIndexThrowsOutOfRange) {
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    auto series = Access::series(storage_.data(), &window_);

    // 写历史位在索引校验处就响亮失败（与读越界同一口径）
    EXPECT_THROW((void)(series[1] = 1234.0), std::out_of_range);
    EXPECT_EQ(storage_[1], 1.0) << "越界写不得落到物理数组";
}

TEST_F(SeriesTest, UnboundSeriesRefRejectsAssignment) {
    // 写入代理的防御性兜底：未绑定的代理不能写
    SeriesRef<double> ref;
    EXPECT_THROW((void)(ref = 1.0), std::out_of_range);
}

TEST_F(SeriesTest, Int64SeriesHasSameSemantics) {
    std::vector<std::int64_t> storage(8, 0);
    storage[0] = 10;
    storage[1] = 20;
    storage[2] = 30;
    WindowState window{.end = 3, .size = 3, .max_lookback = 3, .buffer_bars = 8};
    auto series = Access::series(storage.data(), &window);
    const auto view = Access::view(storage.data(), &window);

    EXPECT_EQ(view[0], 30);
    EXPECT_EQ(view[2], 10);
    series[0] = 99;
    EXPECT_EQ(storage[2], 99);
    EXPECT_THROW((void)view[-1], std::out_of_range);
    EXPECT_THROW((void)view[3], std::out_of_range);
}

// ---------------- AC4：静态断言锁定的公开面 ----------------

TEST(SeriesLayoutTest, SeriesAddsNoDataMembersOverView) {
    EXPECT_EQ(sizeof(Series<double>), sizeof(SeriesView<double>));
    EXPECT_EQ(sizeof(Series<std::int64_t>), sizeof(SeriesView<std::int64_t>));
    EXPECT_EQ(sizeof(SeriesView<double>), 2u * sizeof(void*));
    EXPECT_EQ(sizeof(WindowState), 4u * sizeof(std::int32_t));
}

TEST(SeriesLayoutTest, SeriesIsTrivialStandardLayoutWithoutVirtuals) {
    EXPECT_TRUE(std::is_trivially_copyable_v<Series<double>>);
    EXPECT_TRUE(std::is_trivially_copyable_v<Series<std::int64_t>>);
    EXPECT_TRUE(std::is_standard_layout_v<Series<double>>);
    EXPECT_TRUE(std::is_standard_layout_v<Series<std::int64_t>>);
    EXPECT_FALSE(std::has_virtual_destructor_v<Series<double>>);
    EXPECT_FALSE(std::is_polymorphic_v<Series<double>>);
    EXPECT_FALSE(std::has_virtual_destructor_v<SeriesView<double>>);
    EXPECT_FALSE(std::is_polymorphic_v<SeriesView<double>>);
}

TEST(SeriesLayoutTest, ReadOnlyViewCannotBeUpgradedToWritableSeries) {
    EXPECT_FALSE((std::is_constructible_v<Series<double>, const SeriesView<double>&>));
    EXPECT_FALSE((std::is_convertible_v<SeriesView<double>, Series<double>>));
    EXPECT_TRUE(std::is_default_constructible_v<Series<double>>);  // B 侧可默认构造后赋值
}

TEST(SeriesLayoutTest, NoVectorLikeWriteInterfaceExists) {
    // 「不提供 push_back/erase/resize/reserve/at」是公开面承诺，靠类型上没有这些成员守
    EXPECT_FALSE(has_push_back<Series<double>>::value);
    EXPECT_FALSE(has_push_back<SeriesView<double>>::value);
    EXPECT_FALSE(has_resize<Series<double>>::value);
    EXPECT_FALSE(has_reserve<Series<double>>::value);
    EXPECT_FALSE(has_at<Series<double>>::value);
    EXPECT_FALSE(has_erase<Series<double>>::value);
}

}  // namespace
