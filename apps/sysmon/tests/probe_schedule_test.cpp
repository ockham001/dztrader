/**
 * @file probe_schedule_test.cpp
 * @brief 探测定时排期（#299）：HH:MM 解析、下一个本地墙钟时刻折算、steady_timer 等待。
 *
 * 测试全部用**构造出来的墙钟时刻**驱动（不 sleep 到真实整点、不依赖 CI 的当前时刻）：
 * 纯函数用「本地日历时刻 + 期望的本地时刻」断言，定时器用例只等几百毫秒（到点时刻由
 * 构造的 now 决定）。本地时区取进程默认时区，因此期望值一律用 `localtime` 反解——
 * 不断言「跨时区相同」，只断言「与本地日历语义一致」。
 */

#include "probe_schedule.h"

#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace dztrader::sysmon {
namespace {

using namespace std::chrono_literals;

/// 构造本地日历时刻（固定 2026-01-15；该日期北半球无夏令时切换，精确延时断言因此稳定）。
std::chrono::system_clock::time_point local_wall(int hour,
                                                 int minute,
                                                 int second = 0,
                                                 int millis = 0) {
    std::tm fields{};
    fields.tm_year = 2026 - 1900;
    fields.tm_mon = 0;    // 1 月
    fields.tm_mday = 15;  // 固定日期
    fields.tm_hour = hour;
    fields.tm_min = minute;
    fields.tm_sec = second;
    fields.tm_isdst = -1;
    const std::time_t timestamp = std::mktime(&fields);
    EXPECT_NE(timestamp, static_cast<std::time_t>(-1));
    return std::chrono::system_clock::from_time_t(timestamp) + std::chrono::milliseconds(millis);
}

/// 把墙钟时刻反解为本地日历字段（断言用；不参与被测实现）。
std::tm local_fields(std::chrono::system_clock::time_point wall) {
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(wall);
    std::tm fields{};
#ifdef _WIN32
    localtime_s(&fields, &timestamp);
#else
    localtime_r(&timestamp, &fields);
#endif
    return fields;
}

/// 断言墙钟时刻恰好是本地日历的某日某时某分（秒与毫秒为 0）。
void expect_local_time(std::chrono::system_clock::time_point wall,
                       int month_day,
                       int hour,
                       int minute) {
    const std::tm fields = local_fields(wall);
    EXPECT_EQ(fields.tm_mon, 0) << "month must stay January";
    EXPECT_EQ(fields.tm_mday, month_day);
    EXPECT_EQ(fields.tm_hour, hour);
    EXPECT_EQ(fields.tm_min, minute);
    EXPECT_EQ(fields.tm_sec, 0);
}

// 注：`HH:MM` 的语法校验/解析由类型层唯一实现（`platform::parse_sysmon_probe_time`），
// 其接受与拒绝边界已由 `dzplatform_sysmon_test` 的配置校验用例覆盖；本文件只测排期语义。

// ===== 下一个墙钟时刻 =====

TEST(ProbeScheduleTest, NextWallTimePicksSameDayOccurrence) {
    const auto next = next_probe_wall_time(local_wall(8, 0), {"08:30"});
    ASSERT_TRUE(next.has_value());
    expect_local_time(*next, 15, 8, 30);
}

TEST(ProbeScheduleTest, NextWallTimePicksEarliestFromUnsortedList) {
    const auto next = next_probe_wall_time(local_wall(10, 0), {"20:30", "08:30", "12:30"});
    ASSERT_TRUE(next.has_value());
    expect_local_time(*next, 15, 12, 30);
}

TEST(ProbeScheduleTest, NextWallTimeSkipsAlreadyPassedTimes) {
    // 08:30 已过：只能排次日（错过不补采，启动首探覆盖当下）。
    const auto next = next_probe_wall_time(local_wall(8, 30), {"08:30"});
    ASSERT_TRUE(next.has_value());
    expect_local_time(*next, 16, 8, 30);
}

TEST(ProbeScheduleTest, NextWallTimeIsStrictlyAfterNow) {
    // now = 08:29:59.5：距 08:30 还有 500ms（不是 0、也不是次日）。
    const auto now = local_wall(8, 29, 59, 500);
    const auto delay = next_probe_delay(now, {"08:30"});
    ASSERT_TRUE(delay.has_value());
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::milliseconds>(*delay), 500ms);
}

TEST(ProbeScheduleTest, NextWallTimeHandlesMidnightRollover) {
    const auto next = next_probe_wall_time(local_wall(23, 59, 30), {"00:00"});
    ASSERT_TRUE(next.has_value());
    expect_local_time(*next, 16, 0, 0);
}

TEST(ProbeScheduleTest, NextWallTimeIgnoresDuplicatesAndInvalidEntries) {
    const auto next =
        next_probe_wall_time(local_wall(9, 0), {"08:30", "12:30", "12:30", "bad", "25:00"});
    ASSERT_TRUE(next.has_value());
    expect_local_time(*next, 15, 12, 30);
}

TEST(ProbeScheduleTest, NextWallTimeWithoutUsableEntriesIsUnavailable) {
    EXPECT_FALSE(next_probe_wall_time(local_wall(9, 0), {}).has_value());
    EXPECT_FALSE(next_probe_wall_time(local_wall(9, 0), {"bad", "8:30"}).has_value());
    EXPECT_FALSE(next_probe_delay(local_wall(9, 0), {}).has_value());
}

TEST(ProbeScheduleTest, NextDelayCoversDayWhenAllTimesPassed) {
    // 唯一时刻已过：次日同一时刻（约 24 小时；不断言精确值以避开夏令时语义）。
    const auto delay = next_probe_delay(local_wall(9, 0), {"08:30"});
    ASSERT_TRUE(delay.has_value());
    EXPECT_GT(*delay, 20h);
    EXPECT_LT(*delay, 25h);
}

// ===== 定时器 =====

TEST(ProbeScheduleTest, FiresOnceAtNextLocalMinute) {
    boost::asio::io_context io;
    ProbeSchedule schedule(io);
    int calls = 0;
    // now = 08:29:59.700 → 300ms 后到点。
    schedule.reschedule(local_wall(8, 29, 59, 700), {"08:30"}, [&calls] { ++calls; });
    ASSERT_TRUE(schedule.armed());
    io.restart();
    io.run_for(2s);
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(schedule.armed());
}

TEST(ProbeScheduleTest, StopCancelsPendingWait) {
    boost::asio::io_context io;
    ProbeSchedule schedule(io);
    int calls = 0;
    schedule.reschedule(local_wall(8, 29, 59, 700), {"08:30"}, [&calls] { ++calls; });
    schedule.stop();
    EXPECT_FALSE(schedule.armed());
    io.restart();
    io.run_for(1s);
    EXPECT_EQ(calls, 0);
}

TEST(ProbeScheduleTest, RescheduleReplacesPendingWait) {
    boost::asio::io_context io;
    ProbeSchedule schedule(io);
    int calls = 0;
    // 先排 500ms 后（08:29:59.500 → 08:30），再重排到 100ms 后：旧等待必须被取消。
    // 观测窗 1s 覆盖**旧等待的到点时刻**——若取消失效，calls 会是 2（不是恒真的 1）。
    schedule.reschedule(local_wall(8, 29, 59, 500), {"08:30"}, [&calls] { ++calls; });
    schedule.reschedule(local_wall(8, 29, 59, 900), {"08:30"}, [&calls] { ++calls; });
    ASSERT_TRUE(schedule.armed());
    io.restart();
    io.run_for(1s);
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(schedule.armed());
}

TEST(ProbeScheduleTest, EmptyProbeTimesDoesNotArm) {
    boost::asio::io_context io;
    ProbeSchedule schedule(io);
    int calls = 0;
    schedule.reschedule(local_wall(8, 0), {}, [&calls] { ++calls; });
    EXPECT_FALSE(schedule.armed());
    io.restart();
    io.run_for(200ms);
    EXPECT_EQ(calls, 0);
}

TEST(ProbeScheduleTest, HandlerCanRescheduleItselfForNextOccurrence) {
    boost::asio::io_context io;
    ProbeSchedule schedule(io);
    int calls = 0;
    // 模拟调用方在探测完成后重算下一轮：第一次到点后从「当前时刻」再排下一分钟。
    std::function<void()> on_due = [&] {
        ++calls;
        if (calls == 1) {
            schedule.reschedule(local_wall(8, 29, 59, 900), {"08:30"}, on_due);
        }
    };
    schedule.reschedule(local_wall(8, 29, 59, 700), {"08:30"}, on_due);
    io.restart();
    io.run_for(2s);
    EXPECT_EQ(calls, 2);
    EXPECT_FALSE(schedule.armed());
}

}  // namespace
}  // namespace dztrader::sysmon
