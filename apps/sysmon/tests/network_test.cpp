// 契约 sysmon §2.4 network[] / §2.7：每网卡速率派生值纯函数与 hide_virtual 过滤。
// 断言外部可观察行为：给定两帧原始累计计数器与实际经过秒数，六个速率正确；首次采样、
// 新网卡与计数器重置不可用而非 0；基线按接口名匹配（热插拔不错位）；hide_virtual 的
// 过滤发生在快照构造阶段（全部被滤掉 => 空数组 + available）。

#include "network.h"
#include "snapshot_builder.h"

#include <dztrader/platform/sysmon.h>
#include <dztrader/platform/system_collector.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

platform::RawNetworkInfo make_interface(std::string name, bool is_virtual = false) {
    platform::RawNetworkInfo info;
    info.interface_name = std::move(name);
    info.is_virtual = is_virtual;
    return info;
}

platform::RawSystemInfo make_linux_info() {
    platform::RawSystemInfo info;
    info.os_family = "linux";
    info.os_version = "Ubuntu 24.04.1 LTS";
    info.kernel_version = "6.8.0-45-generic";
    info.arch = "x86_64";
    info.hostname = "trade-01";
    info.cpu_model = "AMD Ryzen 9 7950X 16-Core Processor";
    info.logical_cores = 16;
    info.boot_time_unix_sec = 1'789'990'000;
    return info;
}

/// 只驱动网络域的极小快照构造入口（其余域留空，不参与本文件断言）。
platform::SysmonSnapshot build_with_network(const NetworkMetrics& network_metrics) {
    return build_snapshot("dzsysmon", make_linux_info(), CpuMetrics{}, TemperatureMetrics{},
                          MemorySwapMetrics{}, DiskMetrics{}, network_metrics,
                          std::vector<platform::SysmonAlertInfo>{}, 1000, 0, 0, 1'800'000'000'000);
}

platform::SysmonNetworkInfo make_snapshot_entry(std::string name, bool is_virtual) {
    platform::SysmonNetworkInfo info;
    info.interface_name = std::move(name);
    info.is_virtual = is_virtual;
    return info;
}

}  // namespace

TEST(NetworkTest, FirstSampleRatesUnavailableNotZero) {
    // 首次采样（无基线）：条目存在、累计计数器按本帧填，但六个速率为 null——不得用 0 冒充。
    auto current = make_interface("eth0");
    current.rx_bytes = 1000;
    current.tx_bytes = 2000;
    current.rx_packets = 10;
    current.tx_packets = 20;
    current.rx_errors = 1;
    current.tx_errors = 2;
    current.rx_drops = 3;
    current.tx_drops = 4;

    const auto values = compute_network_rates({}, {current}, 1.0);
    ASSERT_EQ(values.size(), 1U);
    const auto& entry = values[0];
    EXPECT_EQ(entry.interface_name, "eth0");
    EXPECT_EQ(entry.rx_bytes_total, 1000U);
    EXPECT_EQ(entry.tx_bytes_total, 2000U);
    EXPECT_EQ(entry.rx_packets_total, 10U);
    EXPECT_EQ(entry.tx_packets_total, 20U);
    EXPECT_EQ(entry.rx_errors_total, 1U);
    EXPECT_EQ(entry.tx_errors_total, 2U);
    EXPECT_EQ(entry.rx_drops_total, 3U);
    EXPECT_EQ(entry.tx_drops_total, 4U);
    EXPECT_FALSE(entry.rx_bytes_per_sec.has_value());
    EXPECT_FALSE(entry.tx_bytes_per_sec.has_value());
    EXPECT_FALSE(entry.rx_errors_per_sec.has_value());
    EXPECT_FALSE(entry.tx_errors_per_sec.has_value());
    EXPECT_FALSE(entry.rx_drops_per_sec.has_value());
    EXPECT_FALSE(entry.tx_drops_per_sec.has_value());
}

TEST(NetworkTest, ComputesSixRatesFromCounterDeltas) {
    auto prev = make_interface("eth0");
    prev.rx_bytes = 1'000;
    prev.tx_bytes = 2'000;
    prev.rx_errors = 10;
    prev.tx_errors = 20;
    prev.rx_drops = 30;
    prev.tx_drops = 40;

    auto cur = prev;
    cur.rx_bytes = 1'000 + 2'000'000;  // 2 MB / 2s = 1 MB/s
    cur.tx_bytes = 2'000 + 4'000'000;  // 4 MB / 2s = 2 MB/s
    cur.rx_errors = 10 + 6;            // 3/s
    cur.tx_errors = 20 + 10;           // 5/s
    cur.rx_drops = 30 + 4;             // 2/s
    cur.tx_drops = 40 + 8;             // 4/s

    const auto values = compute_network_rates({prev}, {cur}, 2.0);
    ASSERT_EQ(values.size(), 1U);
    const auto& entry = values[0];
    ASSERT_TRUE(entry.rx_bytes_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*entry.rx_bytes_per_sec, 1'000'000.0);
    ASSERT_TRUE(entry.tx_bytes_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*entry.tx_bytes_per_sec, 2'000'000.0);
    ASSERT_TRUE(entry.rx_errors_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*entry.rx_errors_per_sec, 3.0);
    ASSERT_TRUE(entry.tx_errors_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*entry.tx_errors_per_sec, 5.0);
    ASSERT_TRUE(entry.rx_drops_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*entry.rx_drops_per_sec, 2.0);
    ASSERT_TRUE(entry.tx_drops_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*entry.tx_drops_per_sec, 4.0);
}

TEST(NetworkTest, BaselineMatchedByInterfaceNameNotByIndex) {
    // 两帧的数组顺序不同：速率必须按接口名匹配基线，不能按下标（否则速率错位）。
    auto prev_eth0 = make_interface("eth0");
    prev_eth0.rx_bytes = 1'000;
    auto prev_lo = make_interface("lo", true);
    prev_lo.rx_bytes = 500'000;

    auto cur_eth0 = prev_eth0;
    cur_eth0.rx_bytes = 1'000 + 3'000;  // 3000 B / 1s
    auto cur_lo = prev_lo;
    cur_lo.rx_bytes = 500'000 + 1'000'000;  // 1 MB / 1s

    // cur 顺序与 prev 相反。
    const auto values = compute_network_rates({prev_eth0, prev_lo}, {cur_lo, cur_eth0}, 1.0);
    ASSERT_EQ(values.size(), 2U);
    EXPECT_EQ(values[0].interface_name, "lo");
    ASSERT_TRUE(values[0].rx_bytes_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*values[0].rx_bytes_per_sec, 1'000'000.0);
    EXPECT_EQ(values[1].interface_name, "eth0");
    ASSERT_TRUE(values[1].rx_bytes_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*values[1].rx_bytes_per_sec, 3'000.0);
}

TEST(NetworkTest, NewInterfaceHasNoBaselineAndVanishedInterfaceIsDropped) {
    // lo 消失（不再输出）、wlan0 新出现（无基线 => 速率全 null）；eth0 的速率不受影响。
    auto prev_lo = make_interface("lo", true);
    prev_lo.rx_bytes = 100;
    auto prev_eth0 = make_interface("eth0");
    prev_eth0.rx_bytes = 1'000;

    auto cur_eth0 = prev_eth0;
    cur_eth0.rx_bytes = 1'000 + 2'000;  // 2000 B/s
    auto cur_wlan0 = make_interface("wlan0");
    cur_wlan0.rx_bytes = 42;

    const auto values = compute_network_rates({prev_lo, prev_eth0}, {cur_eth0, cur_wlan0}, 1.0);
    ASSERT_EQ(values.size(), 2U);
    // 输出顺序与 cur 一致，且消失的 lo 不占位（序列不错位）。
    EXPECT_EQ(values[0].interface_name, "eth0");
    ASSERT_TRUE(values[0].rx_bytes_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*values[0].rx_bytes_per_sec, 2'000.0);
    EXPECT_EQ(values[1].interface_name, "wlan0");
    EXPECT_FALSE(values[1].rx_bytes_per_sec.has_value());
    EXPECT_FALSE(values[1].tx_bytes_per_sec.has_value());
    // 累计计数器仍按本帧填（新网卡也有累计值，只是没有速率）。
    EXPECT_EQ(values[1].rx_bytes_total, 42U);
}

TEST(NetworkTest, CounterResetMakesThatFieldUnavailableOnly) {
    // 两平台网络计数器均为 64 位：下降即重置（不按 32 位回绕解释）。
    auto prev = make_interface("eth0");
    prev.rx_bytes = 9'000'000'000ULL;  // > 2^32
    prev.tx_bytes = 1'000;
    auto cur = make_interface("eth0");
    cur.rx_bytes = 7;      // 重置
    cur.tx_bytes = 1'500;  // 正常推进

    const auto values = compute_network_rates({prev}, {cur}, 1.0);
    ASSERT_EQ(values.size(), 1U);
    EXPECT_FALSE(values[0].rx_bytes_per_sec.has_value());
    ASSERT_TRUE(values[0].tx_bytes_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*values[0].tx_bytes_per_sec, 500.0);
    // 累计值照实输出重置后的新值。
    EXPECT_EQ(values[0].rx_bytes_total, 7U);
}

TEST(NetworkTest, NonPositiveOrNonFiniteWindowMakesAllRatesUnavailable) {
    auto prev = make_interface("eth0");
    prev.rx_bytes = 1'000;
    prev.tx_bytes = 1'000;
    auto cur = prev;
    cur.rx_bytes = 2'000;
    cur.tx_bytes = 2'000;

    for (const double elapsed : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity()}) {
        const auto values = compute_network_rates({prev}, {cur}, elapsed);
        ASSERT_EQ(values.size(), 1U);
        EXPECT_FALSE(values[0].rx_bytes_per_sec.has_value()) << "elapsed=" << elapsed;
        EXPECT_FALSE(values[0].tx_bytes_per_sec.has_value()) << "elapsed=" << elapsed;
        // 累计计数器与窗口无关，仍按本帧照实输出。
        EXPECT_EQ(values[0].rx_bytes_total, 2'000U) << "elapsed=" << elapsed;
    }
}

TEST(NetworkTest, IsVirtualIsPassedThroughFromRawSample) {
    auto prev_phys = make_interface("enp5s0");
    prev_phys.rx_bytes = 10;
    auto prev_virtual = make_interface("docker0", true);
    prev_virtual.rx_bytes = 20;
    auto cur_phys = prev_phys;
    cur_phys.rx_bytes = 30;
    auto cur_virtual = prev_virtual;
    cur_virtual.rx_bytes = 50;

    const auto values =
        compute_network_rates({prev_phys, prev_virtual}, {cur_phys, cur_virtual}, 1.0);
    ASSERT_EQ(values.size(), 2U);
    EXPECT_FALSE(values[0].is_virtual);
    EXPECT_TRUE(values[1].is_virtual);
}

// ===== hide_virtual：过滤在快照构造阶段（契约 sysmon §2.4、§2.5、§2.7）=====

TEST(NetworkTest, HideVirtualFiltersVirtualInterfacesInSnapshot) {
    NetworkMetrics metrics;
    metrics.network_collected = true;
    metrics.hide_virtual = true;
    metrics.network = std::vector<platform::SysmonNetworkInfo>{
        make_snapshot_entry("enp5s0", false), make_snapshot_entry("docker0", true)};

    const auto snapshot = build_with_network(metrics);
    ASSERT_TRUE(snapshot.network.has_value());
    ASSERT_EQ(snapshot.network->size(), 1U);
    EXPECT_EQ((*snapshot.network)[0].interface_name, "enp5s0");
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_NETWORK).available);
    // 发送方义务：hide_virtual=true 时不得输出 is_virtual=true 的条目（契约 sysmon §7）。
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(NetworkTest, ShowAllInterfacesKeepsVirtualOnes) {
    NetworkMetrics metrics;
    metrics.network_collected = true;
    metrics.hide_virtual = false;
    metrics.network = std::vector<platform::SysmonNetworkInfo>{
        make_snapshot_entry("enp5s0", false), make_snapshot_entry("docker0", true)};

    const auto snapshot = build_with_network(metrics);
    ASSERT_TRUE(snapshot.network.has_value());
    EXPECT_EQ(snapshot.network->size(), 2U);
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_NETWORK).available);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(NetworkTest, AllVirtualFilteredYieldsEmptyArrayButAvailable) {
    // 全部网卡被 hide_virtual 滤掉：空数组 + available=true——"全部被过滤"不是"不可用"。
    NetworkMetrics metrics;
    metrics.network_collected = true;
    metrics.hide_virtual = true;
    metrics.network = std::vector<platform::SysmonNetworkInfo>{make_snapshot_entry("docker0", true),
                                                               make_snapshot_entry("veth0", true)};

    const auto snapshot = build_with_network(metrics);
    ASSERT_TRUE(snapshot.network.has_value());
    EXPECT_TRUE(snapshot.network->empty());
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_NETWORK);
    EXPECT_TRUE(entry.available);
    EXPECT_EQ(entry.reason, std::nullopt);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(NetworkTest, HideVirtualDoesNotAffectCapabilityAvailability) {
    // 开关只影响输出条目、不影响可用性；能力可用但本帧未采到时域为 null 而 available=true。
    NetworkMetrics metrics;
    metrics.network_collected = true;
    metrics.hide_virtual = true;
    metrics.network = std::nullopt;

    const auto snapshot = build_with_network(metrics);
    EXPECT_EQ(snapshot.network, std::nullopt);
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_NETWORK).available);
}

}  // namespace dztrader::sysmon
