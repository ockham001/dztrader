#include "cpu_usage.h"
#include "snapshot_builder.h"

#include <dztrader/platform/sysmon.h>
#include <dztrader/platform/system_collector.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dztrader::sysmon {
namespace {

/// 假采集数据：Linux 原始系统信息（不依赖真实机器）。
platform::RawSystemInfo make_linux_info() {
    platform::RawSystemInfo info;
    info.os_family = "linux";
    info.os_version = "Ubuntu 24.04.1 LTS";
    info.kernel_version = "6.8.0-45-generic";
    info.arch = "x86_64";
    info.hostname = "trade-01";
    info.cpu_model = "AMD Ryzen 9 7950X 16-Core Processor";
    info.logical_cores = 32;
    info.os_build = 0;
    info.boot_time_unix_sec = 1'789'990'000;
    return info;
}

constexpr int64_t kNowMs = 1'790'000'005'000;  // 2026-09 附近占位值，仅用于断言映射
constexpr int64_t kLastSampleMs = 1'790'000'005'123;
constexpr std::string_view kSourceName = "dzsysmon";
constexpr int32_t kMeasureIntervalMs = 1000;
constexpr uint64_t kMissedTicks = 2;

platform::SysmonSnapshot build_with(const platform::RawSystemInfo& info,
                                    std::optional<double> cpu_usage) {
    return build_snapshot(kSourceName, info, cpu_usage, kMeasureIntervalMs, kLastSampleMs,
                          kMissedTicks, kNowMs);
}

TEST(SnapshotBuilderTest, MapsTimestampAndSampling) {
    const auto snapshot = build_with(make_linux_info(), 0.187);
    EXPECT_EQ(snapshot.source, kSourceName);
    EXPECT_EQ(snapshot.timestamp, kNowMs);
    EXPECT_EQ(snapshot.sampling.measure_interval_ms, kMeasureIntervalMs);
    EXPECT_EQ(snapshot.sampling.last_sample_unix_ms, kLastSampleMs);
    EXPECT_EQ(snapshot.sampling.missed_ticks, kMissedTicks);
}

TEST(SnapshotBuilderTest, MapsPlatformAndSystem) {
    const auto snapshot = build_with(make_linux_info(), 0.187);
    EXPECT_EQ(snapshot.platform.family, "linux");
    EXPECT_EQ(snapshot.platform.version, "Ubuntu 24.04.1 LTS");
    EXPECT_EQ(snapshot.platform.kernel, "6.8.0-45-generic");
    EXPECT_EQ(snapshot.platform.arch, "x86_64");
    EXPECT_EQ(snapshot.platform.build, 0U);

    EXPECT_EQ(snapshot.system.hostname, "trade-01");
    EXPECT_EQ(snapshot.system.cpu_model, "AMD Ryzen 9 7950X 16-Core Processor");
    EXPECT_EQ(snapshot.system.logical_cores, 32U);
    EXPECT_EQ(snapshot.system.boot_time_unix_sec, 1'789'990'000);
    // uptime = now_sec - boot_time = 1790000005 - 1789990000
    EXPECT_EQ(snapshot.system.uptime_sec, 10'005);
}

TEST(SnapshotBuilderTest, UptimeClampsNegativeToZero) {
    auto info = make_linux_info();
    info.boot_time_unix_sec = kNowMs / 1000 + 100;  // 启动时间在未来（时钟回拨）
    const auto snapshot = build_with(info, std::nullopt);
    EXPECT_EQ(snapshot.system.uptime_sec, 0);
}

TEST(SnapshotBuilderTest, CpuTotalCarriesUsageRatio) {
    const auto snapshot = build_with(make_linux_info(), 0.187);
    ASSERT_TRUE(snapshot.cpu.total.usage_ratio.has_value());
    EXPECT_DOUBLE_EQ(snapshot.cpu.total.usage_ratio.value(), 0.187);
}

TEST(SnapshotBuilderTest, CpuUsageNullStaysNullNotZero) {
    // 首帧：不可用必须是 null，不得用 0 冒充
    const auto snapshot = build_with(make_linux_info(), std::nullopt);
    EXPECT_EQ(snapshot.cpu.total.usage_ratio, std::nullopt);
}

TEST(SnapshotBuilderTest, UnimplementedDomainsAreNull) {
    const auto snapshot = build_with(make_linux_info(), 0.187);
    EXPECT_EQ(snapshot.memory, std::nullopt);
    EXPECT_EQ(snapshot.swap, std::nullopt);
    EXPECT_EQ(snapshot.volumes, std::nullopt);
    EXPECT_EQ(snapshot.disk_io, std::nullopt);
    EXPECT_EQ(snapshot.network, std::nullopt);
    EXPECT_EQ(snapshot.shm_dir, std::nullopt);
}

TEST(SnapshotBuilderTest, UnimplementedCpuFieldsAreEmptyOrNull) {
    const auto snapshot = build_with(make_linux_info(), 0.187);
    EXPECT_TRUE(snapshot.cpu.cores.empty());
    EXPECT_TRUE(snapshot.cpu.saturated_cores.empty());
    EXPECT_EQ(snapshot.cpu.load_average, std::nullopt);
    EXPECT_EQ(snapshot.cpu.context_switches_per_sec, std::nullopt);
    EXPECT_EQ(snapshot.cpu.interrupts_per_sec, std::nullopt);
}

TEST(SnapshotBuilderTest, AvailabilityCoversAllElevenKeys) {
    const auto snapshot = build_with(make_linux_info(), 0.187);
    const std::vector<std::string> expected = {
        platform::SYSMON_METRIC_SYSTEM,
        platform::SYSMON_METRIC_CPU_TOTAL,
        platform::SYSMON_METRIC_CPU_CORES,
        platform::SYSMON_METRIC_CPU_LOAD_AVERAGE,
        platform::SYSMON_METRIC_CPU_CONTEXT_SWITCHES,
        platform::SYSMON_METRIC_MEMORY,
        platform::SYSMON_METRIC_SWAP,
        platform::SYSMON_METRIC_VOLUMES,
        platform::SYSMON_METRIC_DISK_IO,
        platform::SYSMON_METRIC_NETWORK,
        platform::SYSMON_METRIC_SHM_DIR,
    };
    EXPECT_EQ(snapshot.availability.metrics.size(), expected.size());
    for (const auto& key : expected) {
        EXPECT_TRUE(snapshot.availability.metrics.contains(key)) << key;
    }

    // 首版可用：system 与 cpu.total（available=true 且无 reason）
    for (const char* key : {platform::SYSMON_METRIC_SYSTEM, platform::SYSMON_METRIC_CPU_TOTAL}) {
        ASSERT_TRUE(snapshot.availability.metrics.contains(key));
        const auto& entry = snapshot.availability.metrics.at(key);
        EXPECT_TRUE(entry.available) << key;
        EXPECT_EQ(entry.reason, std::nullopt) << key;
    }
    // 其余 9 键：available=false + reason="not_implemented"
    for (const auto& [key, entry] : snapshot.availability.metrics) {
        if (key == platform::SYSMON_METRIC_SYSTEM || key == platform::SYSMON_METRIC_CPU_TOTAL) {
            continue;
        }
        EXPECT_FALSE(entry.available) << key;
        ASSERT_TRUE(entry.reason.has_value()) << key;
        EXPECT_EQ(entry.reason.value(), "not_implemented") << key;
    }
}

TEST(SnapshotBuilderTest, SemanticHintsEmpty) {
    const auto snapshot = build_with(make_linux_info(), 0.187);
    EXPECT_TRUE(snapshot.availability.semantic_hints.empty());
}

TEST(SnapshotBuilderTest, LinuxPlatformDescriptionContainsKernel) {
    const auto snapshot = build_with(make_linux_info(), 0.187);
    EXPECT_EQ(snapshot.availability.platform_description,
              "Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)");
}

TEST(SnapshotBuilderTest, WindowsPlatformDescriptionContainsBuild) {
    auto info = make_linux_info();
    info.os_family = "windows";
    info.os_version = "Windows 10 Pro";
    info.kernel_version = "10.0.19045";
    info.os_build = 19045;
    const auto snapshot = build_with(info, 0.5);

    EXPECT_EQ(snapshot.platform.build, 19045U);
    EXPECT_EQ(snapshot.availability.platform_description, "Windows 10 Pro (build 19045)");
}

TEST(SnapshotBuilderTest, SerializedSnapshotPassesPlatformValidation) {
    // 编码后必须能通过类型层唯一真相源的校验（形状契约的端到端自检）。
    const auto snapshot = build_with(make_linux_info(), 0.187);
    const nlohmann::json payload = snapshot;
    EXPECT_EQ(platform::SysmonSnapshot::validate(payload), std::nullopt);

    EXPECT_EQ(payload["timestamp"], kNowMs);
    EXPECT_DOUBLE_EQ(payload["cpu"]["total"]["usage_ratio"].get<double>(), 0.187);
    EXPECT_TRUE(payload["memory"].is_null());
    EXPECT_TRUE(payload["cpu"]["cores"].empty());
    EXPECT_EQ(payload["availability"]["metrics"].size(), platform::SYSMON_METRIC_KEYS.size());
}

TEST(SnapshotBuilderTest, SerializedFirstFramePassesValidationWithNullUsage) {
    const auto snapshot = build_with(make_linux_info(), std::nullopt);
    const nlohmann::json payload = snapshot;
    EXPECT_EQ(platform::SysmonSnapshot::validate(payload), std::nullopt);
    EXPECT_TRUE(payload["cpu"]["total"]["usage_ratio"].is_null());
}

// 假采集器（实现 SystemCollector 接缝）确定性驱动速率计算与快照编码：
// 同一组输入在任何机器/时间都产出同一结果（契约 sysmon 采购接缝测试要求）。
class FakeCollector final : public platform::SystemCollector {
public:
    platform::RawSystemInfo info;
    std::optional<platform::RawCpuTimes> times;

    platform::RawSystemInfo collect_system_info() override { return info; }
    std::optional<platform::RawCpuTimes> collect_cpu_times() override { return times; }
};

TEST(SnapshotBuilderTest, FakeCollectorDrivesRateCalculationAndEncoding) {
    FakeCollector fake;
    fake.info = make_linux_info();
    fake.times = platform::RawCpuTimes{};
    fake.times->total_ticks = 1000;
    fake.times->idle_ticks = 800;

    const auto first = fake.collect_cpu_times();
    fake.times->total_ticks += 1000;
    fake.times->idle_ticks += 750;
    const auto second = fake.collect_cpu_times();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    const auto usage = compute_cpu_usage(*first, *second);
    ASSERT_TRUE(usage.has_value());
    EXPECT_DOUBLE_EQ(*usage, 0.25);

    const auto snapshot = build_with(fake.collect_system_info(), *usage);
    ASSERT_TRUE(snapshot.cpu.total.usage_ratio.has_value());
    EXPECT_DOUBLE_EQ(*snapshot.cpu.total.usage_ratio, 0.25);
    EXPECT_EQ(snapshot.platform.family, "linux");
    EXPECT_EQ(snapshot.availability.metrics.at("cpu.total").available, true);
}

}  // namespace
}  // namespace dztrader::sysmon
