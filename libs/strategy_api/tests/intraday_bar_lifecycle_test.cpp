#include "bar_test_data.h"

#include <dztrader/bar.h>

#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

using namespace dztrader::bar;
using namespace dztrader::strategy_api::test;

namespace {

/// @brief 一次 bar 事件的通知类型
enum class EventKind { Open, Update, Close };

/// @brief sink 收到的一次 bar 事件（携带该时刻的 bar 快照）
struct BarEvent {
    EventKind kind;
    Bar bar;
};

/// @brief 记录完整事件序列的手写 fake sink
class RecordingSink {
public:
    void on_bar_open(const Bar& bar) { events.push_back({EventKind::Open, bar}); }
    void on_bar_update(const Bar& bar) { events.push_back({EventKind::Update, bar}); }
    void on_bar_close(const Bar& bar) { events.push_back({EventKind::Close, bar}); }

    std::vector<BarEvent> events;
};

/// @brief 只实现 update 回调的 sink（缺失的 open/close 应为空操作）
class UpdateOnlySink {
public:
    void on_bar_update(const Bar& bar) {
        ++update_count;
        last_update = bar;
    }

    int update_count = 0;
    Bar last_update{};
};

/// @brief 不实现任何回调的 sink（缺失回调全部为空操作）
struct EmptySink {};

/// @brief 断言两根 bar 的全部公开字段一致
void expect_bar_equal(const Bar& actual, const Bar& expected) {
    EXPECT_EQ(actual.trading_day, expected.trading_day);
    EXPECT_EQ(actual.begin_sec, expected.begin_sec);
    EXPECT_EQ(actual.last_tick_sec, expected.last_tick_sec);
    EXPECT_EQ(actual.period_sec, expected.period_sec);
    EXPECT_DOUBLE_EQ(actual.open, expected.open);
    EXPECT_DOUBLE_EQ(actual.high, expected.high);
    EXPECT_DOUBLE_EQ(actual.low, expected.low);
    EXPECT_DOUBLE_EQ(actual.close, expected.close);
    EXPECT_EQ(actual.volume, expected.volume);
    EXPECT_DOUBLE_EQ(actual.turnover, expected.turnover);
    EXPECT_EQ(actual.open_interest, expected.open_interest);
    EXPECT_EQ(actual.cum_volume, expected.cum_volume);
    EXPECT_DOUBLE_EQ(actual.cum_turnover, expected.cum_turnover);
    EXPECT_EQ(actual.is_final, expected.is_final);
}

}  // namespace

class IntradayBarLifecycleTest : public ::testing::Test {
protected:
    TradingSessionTable table_ = day_session_table();
    RecordingSink sink_;
    IntradayBarBuilder<RecordingSink> builder_{table_, 60, sink_};
};

TEST_F(IntradayBarLifecycleTest, FirstTickEmitsOpenThenUpdate) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));

    ASSERT_EQ(sink_.events.size(), 2U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Update);
    for (const BarEvent& event : sink_.events) {
        EXPECT_FALSE(event.bar.is_final);
        EXPECT_EQ(event.bar.trading_day, TRADING_DAY);
        EXPECT_EQ(event.bar.begin_sec, TSEC_09_00);
        EXPECT_EQ(event.bar.period_sec, 60);
        EXPECT_EQ(event.bar.last_tick_sec, TSEC_09_00);
        EXPECT_DOUBLE_EQ(event.bar.open, 100.5);
        EXPECT_DOUBLE_EQ(event.bar.high, 100.5);
        EXPECT_DOUBLE_EQ(event.bar.low, 100.5);
        EXPECT_DOUBLE_EQ(event.bar.close, 100.5);
        EXPECT_EQ(event.bar.volume, 10);
        EXPECT_DOUBLE_EQ(event.bar.turnover, 1005.0);
        EXPECT_EQ(event.bar.open_interest, 7);
    }
}

TEST_F(IntradayBarLifecycleTest, SameBucketTicksEmitUpdateOnly) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));
    builder_.on_tick(make_tick(9 * 3600 + 30, 103.0, 15, 1545.0, 9));
    builder_.on_tick(make_tick(9 * 3600 + 59, 99.0, 18, 1782.0, 8));

    ASSERT_EQ(sink_.events.size(), 4U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[2].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[3].kind, EventKind::Update);
    for (std::size_t i = 1; i < sink_.events.size(); ++i) {
        EXPECT_FALSE(sink_.events[i].bar.is_final);
        EXPECT_EQ(sink_.events[i].bar.begin_sec, TSEC_09_00);
    }
    const Bar& last = sink_.events[3].bar;
    EXPECT_DOUBLE_EQ(last.high, 103.0);
    EXPECT_DOUBLE_EQ(last.low, 99.0);
    EXPECT_DOUBLE_EQ(last.close, 99.0);
    EXPECT_EQ(last.last_tick_sec, TSEC_09_00 + 59);
    EXPECT_EQ(last.volume, 18);
}

TEST_F(IntradayBarLifecycleTest, BucketSwitchEmitsCloseOpenUpdateForOneMinute) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));
    builder_.on_tick(make_tick(9 * 3600 + 10, 103.0, 14, 1442.0, 9));
    builder_.on_tick(make_tick(9 * 3600 + 60, 101.0, 20, 2020.0, 8));

    ASSERT_EQ(sink_.events.size(), 6U);
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[2].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[3].kind, EventKind::Close);
    EXPECT_EQ(sink_.events[4].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[5].kind, EventKind::Update);

    // 旧 bar 以 is_final=true 关闭，携带它自己的最终数据
    const Bar& closed = sink_.events[3].bar;
    EXPECT_TRUE(closed.is_final);
    EXPECT_EQ(closed.begin_sec, TSEC_09_00);
    EXPECT_EQ(closed.last_tick_sec, TSEC_09_00 + 10);
    EXPECT_DOUBLE_EQ(closed.open, 100.5);
    EXPECT_DOUBLE_EQ(closed.high, 103.0);
    EXPECT_DOUBLE_EQ(closed.low, 100.5);
    EXPECT_DOUBLE_EQ(closed.close, 103.0);
    EXPECT_EQ(closed.volume, 14);
    EXPECT_DOUBLE_EQ(closed.turnover, 1442.0);
    EXPECT_EQ(closed.open_interest, 9);

    // 新 bar 先以 is_final=false 打开，再收到本笔 tick 的 update；量额基准接上一笔
    for (std::size_t i = 4; i < sink_.events.size(); ++i) {
        const Bar& opened = sink_.events[i].bar;
        EXPECT_FALSE(opened.is_final);
        EXPECT_EQ(opened.begin_sec, TSEC_09_00 + 60);
        EXPECT_EQ(opened.last_tick_sec, TSEC_09_00 + 60);
        EXPECT_DOUBLE_EQ(opened.open, 101.0);
        EXPECT_DOUBLE_EQ(opened.high, 101.0);
        EXPECT_DOUBLE_EQ(opened.low, 101.0);
        EXPECT_DOUBLE_EQ(opened.close, 101.0);
        EXPECT_EQ(opened.volume, 6);
        EXPECT_DOUBLE_EQ(opened.turnover, 578.0);
        EXPECT_EQ(opened.cum_volume, 20);
        EXPECT_DOUBLE_EQ(opened.cum_turnover, 2020.0);
        EXPECT_EQ(opened.open_interest, 8);
    }

    ASSERT_TRUE(builder_.has_current_bar());
    EXPECT_FALSE(builder_.current_bar().is_final);
    EXPECT_EQ(builder_.current_bar().begin_sec, TSEC_09_00 + 60);
}

TEST_F(IntradayBarLifecycleTest, SkippedBucketsProduceNoEmptyBars) {
    builder_.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));
    builder_.on_tick(make_tick(9 * 3600 + 300, 101.0, 20, 2020.0, 8));  // 跳跃 5 个桶

    ASSERT_EQ(sink_.events.size(), 5U);  // open + update + close + open + update
    EXPECT_EQ(sink_.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[1].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[2].kind, EventKind::Close);
    EXPECT_EQ(sink_.events[3].kind, EventKind::Open);
    EXPECT_EQ(sink_.events[4].kind, EventKind::Update);
    EXPECT_EQ(sink_.events[2].bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(sink_.events[2].bar.last_tick_sec, TSEC_09_00);
    EXPECT_EQ(sink_.events[3].bar.begin_sec, TSEC_09_00 + 300);
    EXPECT_EQ(sink_.events[4].bar.begin_sec, TSEC_09_00 + 300);
    for (const BarEvent& event : sink_.events) {
        EXPECT_FALSE(event.bar.begin_sec > TSEC_09_00 && event.bar.begin_sec < TSEC_09_00 + 300)
            << "empty bucket must not produce a bar, begin_sec=" << event.bar.begin_sec;
    }
}

TEST(IntradayBarLifecyclePeriodTest, BucketSwitchEmitsCloseOpenUpdateForFiveMinutes) {
    const TradingSessionTable table = day_session_table();
    RecordingSink sink;
    IntradayBarBuilder<RecordingSink> builder(table, 300, sink);

    builder.on_tick(make_tick(9 * 3600, 100.0, 10, 1000.0, 7));
    builder.on_tick(make_tick(9 * 3600 + 299, 102.0, 16, 1632.0, 9));  // 09:04:59 仍在 09:00 桶
    builder.on_tick(make_tick(9 * 3600 + 300, 101.0, 20, 2020.0, 8));  // 内部边界 tick 归新桶

    ASSERT_EQ(sink.events.size(), 6U);
    EXPECT_EQ(sink.events[0].kind, EventKind::Open);
    EXPECT_EQ(sink.events[1].kind, EventKind::Update);
    EXPECT_EQ(sink.events[2].kind, EventKind::Update);
    EXPECT_EQ(sink.events[3].kind, EventKind::Close);
    EXPECT_EQ(sink.events[4].kind, EventKind::Open);
    EXPECT_EQ(sink.events[5].kind, EventKind::Update);
    EXPECT_TRUE(sink.events[3].bar.is_final);
    EXPECT_FALSE(sink.events[4].bar.is_final);
    EXPECT_FALSE(sink.events[5].bar.is_final);
    EXPECT_EQ(sink.events[3].bar.begin_sec, TSEC_09_00);
    EXPECT_EQ(sink.events[3].bar.last_tick_sec, TSEC_09_00 + 299);
    EXPECT_EQ(sink.events[4].bar.begin_sec, TSEC_09_00 + 300);
    EXPECT_EQ(sink.events[4].bar.period_sec, 300);
    EXPECT_EQ(sink.events[4].bar.last_tick_sec, TSEC_09_00 + 300);
    EXPECT_EQ(sink.events[5].bar.begin_sec, TSEC_09_00 + 300);
    // 新 bar 以本笔 tick 播种 O=H=L=C，量额基准接上一笔（16/1632 → 20/2020）
    EXPECT_DOUBLE_EQ(sink.events[4].bar.open, 101.0);
    EXPECT_DOUBLE_EQ(sink.events[4].bar.high, 101.0);
    EXPECT_DOUBLE_EQ(sink.events[4].bar.low, 101.0);
    EXPECT_DOUBLE_EQ(sink.events[4].bar.close, 101.0);
    EXPECT_EQ(sink.events[4].bar.volume, 4);
    EXPECT_DOUBLE_EQ(sink.events[4].bar.turnover, 388.0);
}

TEST(IntradayBarLifecycleSinkTest, SinkWithMissingCallbacksStillCompilesAndUpdates) {
    const TradingSessionTable table = day_session_table();
    UpdateOnlySink sink;
    IntradayBarBuilder<UpdateOnlySink> builder(table, 60, sink);

    builder.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));
    builder.on_tick(make_tick(9 * 3600 + 10, 101.0, 12, 1212.0, 8));
    builder.on_tick(
        make_tick(9 * 3600 + 60, 102.0, 20, 2040.0, 9));  // 跨桶：缺失的 open/close 为空操作

    EXPECT_EQ(sink.update_count, 3);
    EXPECT_EQ(sink.last_update.begin_sec, TSEC_09_00 + 60);
    EXPECT_EQ(sink.last_update.last_tick_sec, TSEC_09_00 + 60);
    EXPECT_DOUBLE_EQ(sink.last_update.close, 102.0);
    EXPECT_EQ(sink.last_update.volume, 8);  // 新 bar 量额基准接上一笔（20 - 12）
    EXPECT_FALSE(sink.last_update.is_final);
}

TEST(IntradayBarLifecycleSinkTest, SinkWithNoCallbacksStillCompiles) {
    const TradingSessionTable table = day_session_table();
    EmptySink sink;
    IntradayBarBuilder<EmptySink> builder(table, 60, sink);

    builder.on_tick(make_tick(9 * 3600, 100.5, 10, 1005.0, 7));
    builder.on_tick(make_tick(9 * 3600 + 60, 101.0, 20, 2020.0, 8));  // 跨桶：缺失回调为空操作

    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().begin_sec, TSEC_09_00 + 60);
    EXPECT_EQ(builder.current_bar().volume, 10);  // 20 - 10
    EXPECT_FALSE(builder.current_bar().is_final);
}

TEST(IntradayBarLifecycleSinkTest, NullBarSinkBehavesLikeSinkWithoutCallbacks) {
    const TradingSessionTable table = day_session_table();
    NullBarSink null_sink;  // NullBarSink 可实例化，且是默认 Sink 类型
    IntradayBarBuilder<> default_builder(table, 60, null_sink);
    EmptySink empty_sink;
    IntradayBarBuilder<EmptySink> empty_builder(table, 60, empty_sink);

    const DzTick ticks[] = {
        make_tick(9 * 3600, 100.5, 10, 1005.0, 7),
        make_tick(9 * 3600 + 10, 103.0, 14, 1442.0, 9),
        make_tick(9 * 3600 + 60, 101.0, 20, 2020.0, 8),
        make_tick(9 * 3600 + 120, 99.0, 23, 2277.0, 6),
    };
    for (const DzTick& tick : ticks) {
        default_builder.on_tick(tick);
        empty_builder.on_tick(tick);
        ASSERT_TRUE(default_builder.has_current_bar());
        ASSERT_TRUE(empty_builder.has_current_bar());
        expect_bar_equal(default_builder.current_bar(), empty_builder.current_bar());
    }

    // 默认 Sink 路径的数据也要有独立断言（避免只做两个 builder 的同义比较）
    const Bar& bar = default_builder.current_bar();
    EXPECT_EQ(bar.begin_sec, TSEC_09_00 + 120);
    EXPECT_EQ(bar.last_tick_sec, TSEC_09_00 + 120);
    EXPECT_EQ(bar.trading_day, TRADING_DAY);
    EXPECT_DOUBLE_EQ(bar.open, 99.0);
    EXPECT_DOUBLE_EQ(bar.high, 99.0);
    EXPECT_DOUBLE_EQ(bar.low, 99.0);
    EXPECT_DOUBLE_EQ(bar.close, 99.0);
    EXPECT_EQ(bar.volume, 3);  // 23 - 20
    EXPECT_EQ(bar.cum_volume, 23);
    EXPECT_FALSE(bar.is_final);
}
