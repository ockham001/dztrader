// 契约 sysmon §2.4/§2.7：每物理盘 I/O 派生值纯函数（compute_disk_io）。
// 断言外部可观察行为：给定两帧原始累计计数器与实际经过秒数，吞吐/IOPS/延迟/队列深度/
// 利用率正确；首次采样与计数器重置不可用而非 0；32 位回绕按跨度计算。

#include "disk_io.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace dztrader::sysmon {
namespace {

platform::RawDiskIoInfo make_disk(std::string device) {
    platform::RawDiskIoInfo disk;
    disk.device = std::move(device);
    return disk;
}

}  // namespace

TEST(DiskIoTest, FirstSampleIsUnavailableNotZero) {
    // 首次采样（无基线）：设备条目存在，但全部派生字段为 null——不得用 0 冒充。
    // 即使本帧能读到瞬时 gauge（in_flight），也整块不可用（契约 sysmon §2.7）。
    auto current = make_disk("sda");
    current.read_count = 100;
    current.write_count = 50;
    current.read_bytes = 4096;
    current.write_bytes = 2048;
    current.read_time_us = 1000;
    current.write_time_us = 500;
    current.busy_time_us = 750;
    current.in_flight = 3;

    const auto values = compute_disk_io({}, {current}, 1.0);
    ASSERT_EQ(values.size(), 1U);
    const auto& entry = values[0];
    EXPECT_EQ(entry.device, "sda");
    EXPECT_FALSE(entry.read_bytes_per_sec.has_value());
    EXPECT_FALSE(entry.write_bytes_per_sec.has_value());
    EXPECT_FALSE(entry.read_iops.has_value());
    EXPECT_FALSE(entry.write_iops.has_value());
    EXPECT_FALSE(entry.latency_ms.has_value());
    EXPECT_FALSE(entry.queue_depth.has_value());
    EXPECT_FALSE(entry.utilization.has_value());
}

TEST(DiskIoTest, ComputesRatesLatencyQueueDepthAndUtilization) {
    auto prev = make_disk("sda");
    prev.read_bytes = 1000;
    prev.write_bytes = 2000;
    prev.read_count = 10;
    prev.write_count = 20;
    prev.read_time_us = 4000;
    prev.write_time_us = 6000;
    prev.busy_time_us = 200'000;

    auto cur = prev;
    cur.read_bytes = 1000 + 2'000'000;       // 2 MB in 2s -> 1 MB/s
    cur.write_bytes = 2000 + 4'000'000;      // 4 MB in 2s -> 2 MB/s
    cur.read_count = 10 + 30;                // 15 reads/s
    cur.write_count = 20 + 10;               // 5 writes/s
    cur.read_time_us = 4000 + 30'000;        // 30 ms total for 30 reads
    cur.write_time_us = 6000 + 10'000;       // 10 ms total for 10 writes
    cur.busy_time_us = 200'000 + 1'000'000;  // 1s busy in a 2s window
    cur.in_flight = 4;

    const auto values = compute_disk_io({prev}, {cur}, 2.0);
    ASSERT_EQ(values.size(), 1U);
    const auto& entry = values[0];
    ASSERT_TRUE(entry.read_bytes_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*entry.read_bytes_per_sec, 1'000'000.0);
    ASSERT_TRUE(entry.write_bytes_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*entry.write_bytes_per_sec, 2'000'000.0);
    ASSERT_TRUE(entry.read_iops.has_value());
    EXPECT_DOUBLE_EQ(*entry.read_iops, 15.0);
    ASSERT_TRUE(entry.write_iops.has_value());
    EXPECT_DOUBLE_EQ(*entry.write_iops, 5.0);
    // 平均延迟 = Δ(读+写服务时间) / Δ(读+写完成数) = 40ms / 40 = 1ms。
    ASSERT_TRUE(entry.latency_ms.has_value());
    EXPECT_DOUBLE_EQ(*entry.latency_ms, 1.0);
    // 队列深度是采样时刻在途请求数（瞬时 gauge），不是区间平均。
    ASSERT_TRUE(entry.queue_depth.has_value());
    EXPECT_DOUBLE_EQ(*entry.queue_depth, 4.0);
    // 利用率 = 1s 忙 / 2s 窗口 = 0.5。
    ASSERT_TRUE(entry.utilization.has_value());
    EXPECT_DOUBLE_EQ(*entry.utilization, 0.5);
}

TEST(DiskIoTest, ZeroRequestsInWindowMakesLatencyUnavailable) {
    auto prev = make_disk("sda");
    prev.read_count = 10;
    prev.read_time_us = 1000;
    auto cur = prev;  // 没有完成的请求，但字节/时间因其它原因推进
    cur.read_bytes = 4096;
    cur.busy_time_us = 500'000;
    cur.in_flight = 1;
    prev.busy_time_us = 0;

    const auto values = compute_disk_io({prev}, {cur}, 1.0);
    ASSERT_EQ(values.size(), 1U);
    // 平均延迟无定义：返回不可用而不是 0（契约 sysmon §2.7 "不得用 0 冒充"）。
    EXPECT_FALSE(values[0].latency_ms.has_value());
    EXPECT_TRUE(values[0].queue_depth.has_value());
}

TEST(DiskIoTest, CounterResetIsUnavailableNotDerivedFromStaleBaseline) {
    // 64 位计数器下降（设备重建/计数器重置）：该字段本帧不可用，不得沿用旧值。
    auto prev = make_disk("sda");
    prev.read_count = 5'000'000'000ULL;  // > 2^32：不能按 32 位回绕解释
    prev.read_bytes = 9'000'000'000ULL;
    prev.busy_time_us = 5'000'000;
    auto cur = make_disk("sda");
    cur.read_count = 7;
    cur.read_bytes = 4096;
    cur.busy_time_us = 1000;
    cur.in_flight = 2;

    const auto values = compute_disk_io({prev}, {cur}, 1.0);
    ASSERT_EQ(values.size(), 1U);
    EXPECT_FALSE(values[0].read_iops.has_value());
    EXPECT_FALSE(values[0].read_bytes_per_sec.has_value());
    EXPECT_FALSE(values[0].utilization.has_value());
    // 瞬时 gauge 不依赖差值：本帧仍可输出真实在途数。
    ASSERT_TRUE(values[0].queue_depth.has_value());
    EXPECT_DOUBLE_EQ(*values[0].queue_depth, 2.0);
}

TEST(DiskIoTest, ThirtyTwoBitCounterWrapIsHandled) {
    // Windows 的 ReadCount/WriteCount 是 32 位 DWORD；Linux 的毫秒计数器是 32 位。
    // 两次值都落在跨度内且本次更小 ⇒ 按跨度回绕（契约 sysmon §2.7）。
    const uint64_t u32_max = std::numeric_limits<uint32_t>::max();

    platform::RawDiskIoInfo prev = make_disk("sda");
    prev.read_count = u32_max - 1;
    prev.write_count = 100;
    prev.read_time_us = u32_max * 1000ULL - 1000;  // 32 位毫秒计数器的微秒表示
    prev.busy_time_us = kDiskTimeCounterWrapSpanUs - 500'000;

    platform::RawDiskIoInfo cur = make_disk("sda");
    cur.read_count = 1;  // 回绕：Δ = 3
    cur.write_count = 100;
    cur.read_time_us = 2000;     // 回绕：Δ = 1000 + 2000 = 3000us
    cur.busy_time_us = 500'000;  // 回绕：Δ = 1s
    cur.in_flight = 0;

    const auto values = compute_disk_io({prev}, {cur}, 2.0);
    ASSERT_EQ(values.size(), 1U);
    const auto& entry = values[0];
    ASSERT_TRUE(entry.read_iops.has_value());
    EXPECT_DOUBLE_EQ(*entry.read_iops, 1.5);  // 3 次 / 2s
    ASSERT_TRUE(entry.latency_ms.has_value());
    // 回绕后 Δ读服务时间 = (跨度 - 上次) + 本次 = 2000 + 2000 = 4000us；Δ完成请求数 = 3。
    EXPECT_DOUBLE_EQ(*entry.latency_ms, 4.0 / 3.0);
    ASSERT_TRUE(entry.utilization.has_value());
    EXPECT_DOUBLE_EQ(*entry.utilization, 0.5);  // 1s / 2s
}

TEST(DiskIoTest, DeviceSetChangesAcrossSamples) {
    auto previous_a = make_disk("sda");
    previous_a.read_count = 100;
    previous_a.read_bytes = 1024;
    previous_a.read_time_us = 1000;
    previous_a.busy_time_us = 1000;
    previous_a.in_flight = 0;

    auto current_a = previous_a;
    current_a.read_count = 200;
    current_a.read_bytes = 2048;
    current_a.read_time_us = 2000;
    current_a.busy_time_us = 2000;

    // 本帧新出现的设备没有基线：全部派生字段不可用（不是 0）。
    auto current_b = make_disk("nvme0n1");
    current_b.read_count = 10;
    current_b.in_flight = 1;

    const auto values = compute_disk_io({previous_a}, {current_a, current_b}, 1.0);
    ASSERT_EQ(values.size(), 2U);
    EXPECT_EQ(values[0].device, "sda");
    EXPECT_TRUE(values[0].read_iops.has_value());
    EXPECT_EQ(values[1].device, "nvme0n1");
    EXPECT_FALSE(values[1].read_iops.has_value());
    EXPECT_FALSE(values[1].queue_depth.has_value());

    // 上一帧存在、本帧消失的设备不再输出。
    auto previous_c = make_disk("sdc");
    previous_c.read_count = 1;
    const auto only_a = compute_disk_io({previous_a, previous_c}, {current_a}, 1.0);
    ASSERT_EQ(only_a.size(), 1U);
    EXPECT_EQ(only_a[0].device, "sda");
}

TEST(DiskIoTest, MatchesBaselineByDeviceNotArrayPosition) {
    auto first = make_disk("sda");
    first.read_count = 10;
    auto second = make_disk("sdb");
    second.read_count = 20;
    auto cur_a = first;
    cur_a.read_count = 20;
    auto cur_b = second;
    cur_b.read_count = 40;

    // 上一帧顺序与本次相反：按设备标识匹配，不能按下标。
    const auto values = compute_disk_io({second, first}, {cur_a, cur_b}, 1.0);
    ASSERT_EQ(values.size(), 2U);
    ASSERT_TRUE(values[0].read_iops.has_value());
    EXPECT_DOUBLE_EQ(*values[0].read_iops, 10.0);
    ASSERT_TRUE(values[1].read_iops.has_value());
    EXPECT_DOUBLE_EQ(*values[1].read_iops, 20.0);
}

TEST(DiskIoTest, NonPositiveElapsedMakesAllValuesUnavailable) {
    auto prev = make_disk("sda");
    prev.read_count = 1;
    auto cur = prev;
    cur.read_count = 2;
    cur.in_flight = 1;

    for (const double elapsed : {0.0, -1.0}) {
        const auto values = compute_disk_io({prev}, {cur}, elapsed);
        ASSERT_EQ(values.size(), 1U);
        EXPECT_FALSE(values[0].read_iops.has_value());
        EXPECT_FALSE(values[0].queue_depth.has_value());
        EXPECT_FALSE(values[0].utilization.has_value());
    }
}

TEST(DiskIoTest, UtilizationClampsToUnitAndRejectsOutOfWindowBusyTime) {
    auto prev = make_disk("sda");
    prev.busy_time_us = 0;
    auto cur = prev;
    // 忙时间略超窗口（测量误差范围）：钳制到 1，不产生 >1 的比率。
    cur.busy_time_us = 1'200'000;
    auto values = compute_disk_io({prev}, {cur}, 1.0);
    ASSERT_EQ(values.size(), 1U);
    ASSERT_TRUE(values[0].utilization.has_value());
    EXPECT_DOUBLE_EQ(*values[0].utilization, 1.0);

    // 忙时间超过窗口容忍上限（如 Windows QueryTime 受系统时间跳变影响）：不可用，
    // 不得钳成 100% 冒充满载。
    cur.busy_time_us = 3'000'000;
    values = compute_disk_io({prev}, {cur}, 1.0);
    ASSERT_EQ(values.size(), 1U);
    EXPECT_FALSE(values[0].utilization.has_value());
}

TEST(DiskIoTest, MissingPlatformGaugesAreNullNotZero) {
    // 平台不提供忙时间 / 在途数时对应字段为 null（本平台不可用，不是 0）。
    auto prev = make_disk("sda");
    prev.read_count = 1;
    prev.busy_time_us = std::nullopt;
    auto cur = prev;
    cur.read_count = 2;
    cur.in_flight = std::nullopt;
    cur.busy_time_us = std::nullopt;

    const auto values = compute_disk_io({prev}, {cur}, 1.0);
    ASSERT_EQ(values.size(), 1U);
    EXPECT_TRUE(values[0].read_iops.has_value());
    EXPECT_FALSE(values[0].utilization.has_value());
    EXPECT_FALSE(values[0].queue_depth.has_value());
}

}  // namespace dztrader::sysmon
