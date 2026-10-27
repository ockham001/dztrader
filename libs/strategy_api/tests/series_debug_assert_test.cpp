/**
 * @file series_debug_assert_test.cpp
 * @brief 越界抛出前的 Debug 断言（issue #155 AC2「Debug 抛前有 assert」，组件文档 §4.2）
 *
 * 单独成 target 的理由：其他 series 用例定义 `DZTRADER_SERIES_NO_DEBUG_ASSERT` 关掉断言
 * （断言先于异常，会 abort、吞掉 `EXPECT_THROW`），于是 AC2 的断言条款在那些 target 里
 * 无法验证。本 target **不定义**该宏：Debug 下断言生效，用 `EXPECT_DEATH` 断言进程确实
 * 在越界时中止；Release（`NDEBUG`）下断言编译掉，只验证「仍然抛异常」。
 *
 * 覆盖四个断言点（`check_index` 的越界与负索引共用同一处断言、`validate_writable` 的写历史、
 * `validate_current` 的过期代理、`validate_bound` 的未绑定代理），不靠同一个入口复述。
 */
#include <dztrader/series.h>
#include <dztrader/series_internal.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

using dztrader::Series;
using dztrader::SeriesView;
using dztrader::WindowState;
using dztrader::series_internal::Access;

/// @brief 已填 2 根、容量 8 的窗口（值 = 物理下标）
struct BoundWindow {
    explicit BoundWindow(std::int32_t size = 2)
        : storage(8, 0.0) {
        for (std::int32_t i = 0; i < 8; ++i) {
            storage[static_cast<std::size_t>(i)] = static_cast<double>(i);
        }
        state = WindowState{
            .end = size, .size = size, .max_lookback = 4, .buffer_bars = 8, .advance_seq = 0};
    }
    [[nodiscard]] SeriesView<double> view() { return Access::view(storage.data(), &state); }
    [[nodiscard]] Series<double> series() { return Access::series(storage.data(), &state); }

    std::vector<double> storage;
    WindowState state{};
};

// Debug：断言先于异常，进程 abort（退出码非 0）。Release：断言编译掉，改为抛异常。
#if defined(NDEBUG)
constexpr bool kAssertArmed = false;
#else
constexpr bool kAssertArmed = true;
#endif

}  // namespace

TEST(SeriesDebugAssertTest, ReadOutOfRangePath) {
    BoundWindow fixture;
    auto view = fixture.view();
    if (kAssertArmed) {
        EXPECT_DEATH((void)view[9], "Assertion|assert|false");
    } else {
        EXPECT_THROW((void)view[9], std::out_of_range);
    }
}

TEST(SeriesDebugAssertTest, NegativeIndexPath) {
    BoundWindow fixture;
    auto view = fixture.view();
    if (kAssertArmed) {
        EXPECT_DEATH((void)view[-1], "Assertion|assert|false");
    } else {
        EXPECT_THROW((void)view[-1], std::out_of_range);
    }
}

TEST(SeriesDebugAssertTest, WriteHistoryPath) {
    // 写历史在 check_index 处断言（到达 SeriesRef 之前）
    BoundWindow fixture;
    auto series = fixture.series();
    if (kAssertArmed) {
        EXPECT_DEATH((void)(series[1] = 1.0), "Assertion|assert|false");
    } else {
        EXPECT_THROW((void)(series[1] = 1.0), std::out_of_range);
    }
}

TEST(SeriesDebugAssertTest, StaleProxyPath) {
    // 过期代理的写入（validate_current）也断言
    BoundWindow fixture;
    auto series = fixture.series();
    auto saved = series[0];
    fixture.state.end = 3;  // 推进后代理过期
    ++fixture.state.advance_seq;
    if (kAssertArmed) {
        EXPECT_DEATH((void)(saved = 1.0), "Assertion|assert|false");
    } else {
        EXPECT_THROW((void)(saved = 1.0), std::out_of_range);
    }
}

TEST(SeriesDebugAssertTest, UnboundProxyPath) {
    dztrader::SeriesRef<double> unbound;
    if (kAssertArmed) {
        EXPECT_DEATH((void)static_cast<double>(unbound), "Assertion|assert|false");
    } else {
        EXPECT_THROW((void)static_cast<double>(unbound), std::out_of_range);
    }
}
