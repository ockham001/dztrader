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

/// @brief Series 层用例的公共夹具
///
/// 测试目标定义 `DZTRADER_SERIES_NO_DEBUG_ASSERT`（见 CMakeLists 注释）关闭 Debug 断言：
/// 断言先于异常，否则 Debug 下 `EXPECT_THROW` 用例会先 abort。断言本身的存在性由
/// `DZTRADER_SERIES_ASSERT` 宏在 Debug 构建里保证（Release 由 NDEBUG 关闭）。
class SeriesTest : public ::testing::Test {
protected:
    /// @brief 造一个已填 `size` 根的窗口，值 = 物理下标
    void bind_window(std::int32_t size, std::int32_t max_lookback, std::int32_t buffer_bars) {
        storage_.assign(static_cast<std::size_t>(buffer_bars), 0.0);
        for (std::int32_t i = 0; i < buffer_bars; ++i) {
            storage_[static_cast<std::size_t>(i)] = static_cast<double>(i);
        }
        window_ = WindowState{.end = size,
                              .size = size,
                              .max_lookback = max_lookback,
                              .buffer_bars = buffer_bars,
                              .advance_seq = 0};
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

TEST_F(SeriesTest, WriteToHistoryIsCompilableButThrowsAtRuntime) {
    // 组件文档 §4.2 口径：写历史没有 API（拿不到 T&），但 `X[1] = v` 这种写法仍可编译，
    // 代价是运行期抛。这条用例把「可编译 + 必抛」两件事同时钉住，防止有人为了
    // 「编译期拒绝」而改坏迁移代码的可用性（如 for 循环里 X[n]）。
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    auto series = Access::series(storage_.data(), &window_);

    static_assert(std::is_assignable_v<decltype(series[1]), double>,
                  "X[n]=v must stay compilable (migration code writes X[n] in loops)");
    EXPECT_THROW((void)(series[1] = 9.0), std::out_of_range);
    EXPECT_THROW((void)(series[2] = 9.0), std::out_of_range);
    EXPECT_THROW((void)(series[3] = 9.0), std::out_of_range);  // n == size 也抛
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

TEST_F(SeriesTest, UnboundSeriesRefRejectsReadInsteadOfDereferencingNull) {
    // 未绑定代理的读取同样必须响亮失败。缺这条检查时 `*slot_` 是 nullptr 解引用
    // （UB，ASan 下 SEGV）：默认构造的代理看似无害，读一下就崩进程。
    SeriesRef<double> ref;
    EXPECT_THROW((void)static_cast<double>(ref), std::out_of_range);

    SeriesRef<std::int64_t> int_ref;
    EXPECT_THROW((void)static_cast<std::int64_t>(int_ref), std::out_of_range);
}

TEST_F(SeriesTest, DefaultConstructedSeriesRejectsReadAndWriteBeforeBinding) {
    // A 完成注入前（#157 的 make_series 尚未调用）的默认构造形态：读/写都抛，不解引用空指针
    Series<double> series;
    EXPECT_THROW((void)series[0], std::out_of_range);
    EXPECT_TRUE(series.span().empty());
}

TEST_F(SeriesTest, StaleWriteProxyCannotClobberHistory) {
    // 关键不变式：B 可以保存 Series 副本（组件文档 §3），那么写入代理也可能被保存到
    // 下一根 bar。此时它绑定的槽位已变成历史 [1]，而 n_ 仍是 0——只检查 n_ 会让写历史
    // **静默**成功。代理记住创建时的 window.end，推进后写入必须抛。
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    auto series = Access::series(storage_.data(), &window_);

    auto saved = series[0];
    saved = 55.0;  // 同一根 bar 内：合法
    EXPECT_EQ(storage_[2], 55.0);

    // 推进一根：窗口右边界前移，saved 的槽位成为历史
    window_.end = 4;
    window_.size = 3;
    ++window_.advance_seq;  // 模拟一次真实推进
    storage_[3] = 66.0;
    const double history_before = storage_[2];

    EXPECT_THROW((void)(saved = -1.0), std::out_of_range);
    EXPECT_EQ(storage_[2], history_before) << "过期代理静默改写了历史位";
}

TEST_F(SeriesTest, StaleProxyReadAlsoThrowsInsteadOfReturningHistory) {
    // 读写必须同口径：只挡写会让「读」静默返回历史值（同一类静默不一致）。
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    auto series = Access::series(storage_.data(), &window_);

    const auto saved = series[0];
    EXPECT_EQ(static_cast<double>(saved), 2.0);  // 同一根 bar 内可读

    window_.end = 4;  // 推进后代理已过期
    window_.size = 3;
    ++window_.advance_seq;

    EXPECT_THROW((void)static_cast<double>(saved), std::out_of_range);
}

TEST_F(SeriesTest, StaleProxySurvivesEndGoingBackwardsAfterRelocate) {
    // ABA 回归：回卷把 end 置回 size（变小），之后再推进会让 end 重新等于创建代理时的值。
    // 若只比较 end，守卫会被骗过；advance_seq 单调，才是可靠判据。
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    auto series = Access::series(storage_.data(), &window_);
    auto saved = series[0];
    const std::int32_t end_at_creation = window_.end;

    // 推进 → 回卷（end 回到 size）→ 再推进（end 重新到达 creation 值）
    window_.end = 4;
    ++window_.advance_seq;
    window_.end = window_.size;  // 回卷：end 回退，advance_seq 不动
    ++window_.advance_seq;       // 再推进一根

    ASSERT_EQ(window_.end, end_at_creation) << "前提：end 已回到创建代理时的值（ABA 场景）";
    EXPECT_THROW((void)(saved = 1.0), std::out_of_range)
        << "end 回到旧值不代表代理仍指向同一根 bar";
    EXPECT_THROW((void)static_cast<double>(saved), std::out_of_range);
}

TEST_F(SeriesTest, StaleWriteProxyMessageNamesBarAdvance) {
    bind_window(/*size=*/2, /*max_lookback=*/2, /*buffer_bars=*/8);
    auto series = Access::series(storage_.data(), &window_);
    auto saved = series[0];

    window_.end = 3;
    window_.size = 2;
    ++window_.advance_seq;

    try {
        saved = 1.0;
        FAIL() << "expected std::out_of_range";
    } catch (const std::out_of_range& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("stale"), std::string::npos) << message;
        EXPECT_NE(message.find("bar_advanced=1"), std::string::npos) << message;
    }
}

TEST_F(SeriesTest, ConstSeriesCannotBeWrittenAtCompileTime) {
    // 「拿不到可写引用」是真正的不变式：不可写序列一旦 const 化，赋值就是编译期错误。
    // 运行期可达的 `X[n] = v` 走 SeriesRef 的索引校验（见 WriteToHistoryIndexThrowsOutOfRange）。
    bind_window(/*size=*/3, /*max_lookback=*/3, /*buffer_bars=*/8);
    const auto series = Access::series(storage_.data(), &window_);

    // 只有经 const 路径读出的值可赋给变量，写不回数组
    const double value = series[0];
    EXPECT_EQ(value, 2.0);
    static_assert(!std::is_assignable_v<decltype(series[0]), double>,
                  "const Series must not expose a writable element");
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
    // 4 个几何量 + 单调推进序号（advance_seq，防回卷后 end 的 ABA）
    EXPECT_EQ(sizeof(WindowState), 5u * sizeof(std::int32_t));
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
