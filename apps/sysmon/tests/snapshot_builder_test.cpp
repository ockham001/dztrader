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
#include <utility>
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

platform::SysmonCpuCore make_core(uint32_t index,
                                  std::optional<double> usage_ratio,
                                  bool saturated) {
    platform::SysmonCpuCore core;
    core.index = index;
    core.usage_ratio = usage_ratio;
    core.saturated = saturated;
    return core;
}

/// 本票（#72）落地后的典型指标：总量/每核/饱和/负载/速率全可用。
CpuMetrics make_full_metrics() {
    CpuMetrics metrics;
    metrics.total_usage_ratio = 0.187;
    metrics.cores_collected = true;
    metrics.cores = {make_core(0, 0.5, false), make_core(1, std::nullopt, false),
                     make_core(2, 0.95, true)};
    metrics.saturated_cores = {2};
    platform::SysmonLoadAverage load;
    load.one = 0.42;
    load.five = 0.51;
    load.fifteen = 0.63;
    load.semantic = "load_average";
    metrics.load_collected = true;
    metrics.load_average = load;
    metrics.rates_collected = true;
    metrics.context_switches_per_sec = 1234.0;
    metrics.interrupts_per_sec = 567.0;
    return metrics;
}

constexpr int64_t kNowMs = 1'790'000'005'000;  // 2026-09 附近占位值，仅用于断言映射
constexpr int64_t kLastSampleMs = 1'790'000'005'123;
constexpr std::string_view kSourceName = "dzsysmon";
constexpr int32_t kMeasureIntervalMs = 1000;
constexpr uint64_t kMissedTicks = 2;

platform::SysmonSnapshot build_with(const platform::RawSystemInfo& info,
                                    const CpuMetrics& metrics) {
    return build_snapshot(kSourceName, info, metrics, kMeasureIntervalMs, kLastSampleMs,
                          kMissedTicks, kNowMs);
}

TEST(SnapshotBuilderTest, MapsTimestampAndSampling) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    EXPECT_EQ(snapshot.source, kSourceName);
    EXPECT_EQ(snapshot.timestamp, kNowMs);
    EXPECT_EQ(snapshot.sampling.measure_interval_ms, kMeasureIntervalMs);
    EXPECT_EQ(snapshot.sampling.last_sample_unix_ms, kLastSampleMs);
    EXPECT_EQ(snapshot.sampling.missed_ticks, kMissedTicks);
}

TEST(SnapshotBuilderTest, MapsPlatformAndSystem) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    EXPECT_EQ(snapshot.platform.family, "linux");
    EXPECT_EQ(snapshot.platform.version, "Ubuntu 24.04.1 LTS");
    EXPECT_EQ(snapshot.platform.kernel, "6.8.0-45-generic");
    EXPECT_EQ(snapshot.platform.arch, "x86_64");
    EXPECT_EQ(snapshot.platform.build, 0U);

    EXPECT_EQ(snapshot.system.hostname, "trade-01");
    EXPECT_EQ(snapshot.system.cpu_model, "AMD Ryzen 9 7950X 16-Core Processor");
    // uptime = now_sec - boot_time = 1790000005 - 1789990000
    EXPECT_EQ(snapshot.system.boot_time_unix_sec, 1'789'990'000);
    EXPECT_EQ(snapshot.system.uptime_sec, 10'005);
}

TEST(SnapshotBuilderTest, KeepsStartupLogicalCoresWhenCoresUnavailable) {
    CpuMetrics metrics;
    const auto snapshot = build_with(make_linux_info(), metrics);
    EXPECT_EQ(snapshot.system.logical_cores, 32U);
}

TEST(SnapshotBuilderTest, UptimeClampsNegativeToZero) {
    auto info = make_linux_info();
    info.boot_time_unix_sec = kNowMs / 1000 + 100;  // 启动时间在未来（时钟回拨）
    CpuMetrics metrics;
    const auto snapshot = build_with(info, metrics);
    EXPECT_EQ(snapshot.system.uptime_sec, 0);
}

TEST(SnapshotBuilderTest, CpuTotalCarriesUsageRatio) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    ASSERT_TRUE(snapshot.cpu.total.usage_ratio.has_value());
    EXPECT_DOUBLE_EQ(snapshot.cpu.total.usage_ratio.value(), 0.187);
}

TEST(SnapshotBuilderTest, CpuUsageNullStaysNullNotZero) {
    auto metrics = make_full_metrics();
    metrics.total_usage_ratio = std::nullopt;  // 首帧：不可用必须是 null，不得用 0 冒充
    const auto snapshot = build_with(make_linux_info(), metrics);
    EXPECT_EQ(snapshot.cpu.total.usage_ratio, std::nullopt);
}

TEST(SnapshotBuilderTest, CpuCoresAndSaturationMapped) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    ASSERT_EQ(snapshot.cpu.cores.size(), 3U);
    EXPECT_EQ(snapshot.cpu.cores[0].index, 0U);
    EXPECT_EQ(snapshot.cpu.cores[1].usage_ratio, std::nullopt);
    EXPECT_TRUE(snapshot.cpu.cores[2].saturated);
    ASSERT_EQ(snapshot.cpu.saturated_cores.size(), 1U);
    EXPECT_EQ(snapshot.cpu.saturated_cores[0], 2U);
    // 契约 sysmon §2.3：每核序列可用时长度恒等于逻辑核数（热插拔后以本次序列为准）。
    EXPECT_EQ(snapshot.system.logical_cores, 3U);
}

TEST(SnapshotBuilderTest, CoresUnavailableIsEmptyArrayWithReason) {
    auto metrics = make_full_metrics();
    metrics.cores_collected = false;
    metrics.cores = {make_core(0, 0.5, true)};
    metrics.saturated_cores = {0};
    const auto snapshot = build_with(make_linux_info(), metrics);

    EXPECT_TRUE(snapshot.cpu.cores.empty());
    EXPECT_TRUE(snapshot.cpu.saturated_cores.empty());
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_CORES);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "collect_failed");
    // 启动期逻辑核数保留（不因本次每核不可用而变成 0）。
    EXPECT_EQ(snapshot.system.logical_cores, 32U);
}

TEST(SnapshotBuilderTest, LoadCollectedButValueNullKeepsAvailable) {
    // 能力粘性（契约 sysmon §2.5）：本帧负载数值未就绪时为 null，但可用性仍为 true。
    auto metrics = make_full_metrics();
    metrics.load_average = std::nullopt;
    const auto snapshot = build_with(make_linux_info(), metrics);

    EXPECT_EQ(snapshot.cpu.load_average, std::nullopt);
    EXPECT_TRUE(
        snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_LOAD_AVERAGE).available);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, CoresCollectedWithNullValuesKeepsAvailable) {
    // 能力粘性（契约 sysmon §2.5、§2.7）：本帧未采到每核数值时保留 index、usage 全 null，
    // 可用性仍为 true（不是沿用旧值，也不是把空数组冒充"不可用"）。
    auto metrics = make_full_metrics();
    for (auto& core : metrics.cores) {
        core.usage_ratio = std::nullopt;
        core.saturated = false;
    }
    metrics.saturated_cores.clear();
    const auto snapshot = build_with(make_linux_info(), metrics);

    ASSERT_EQ(snapshot.cpu.cores.size(), 3U);
    EXPECT_EQ(snapshot.cpu.cores[0].index, 0U);
    EXPECT_EQ(snapshot.cpu.cores[0].usage_ratio, std::nullopt);
    EXPECT_FALSE(snapshot.cpu.cores[0].saturated);
    EXPECT_TRUE(snapshot.cpu.saturated_cores.empty());
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_CORES).available);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, LoadAverageMappedWithoutHintForRealLoadAverage) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    ASSERT_TRUE(snapshot.cpu.load_average.has_value());
    EXPECT_EQ(snapshot.cpu.load_average->semantic, "load_average");
    ASSERT_TRUE(snapshot.cpu.load_average->one.has_value());
    EXPECT_DOUBLE_EQ(snapshot.cpu.load_average->one.value(), 0.42);
    EXPECT_TRUE(
        snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_LOAD_AVERAGE).available);
    EXPECT_TRUE(snapshot.availability.semantic_hints.empty());
}

TEST(SnapshotBuilderTest, ProcessorQueueLoadAddsSemanticHint) {
    auto info = make_linux_info();
    info.os_family = "windows";
    info.os_version = "Windows 10 Pro";
    info.kernel_version = "10.0.19045";
    info.os_build = 19045;

    auto metrics = make_full_metrics();
    platform::SysmonLoadAverage load;
    load.one = 2.0;
    load.five = 1.5;
    load.fifteen = 1.0;
    load.semantic = "processor_queue";
    metrics.load_average = load;

    const auto snapshot = build_with(info, metrics);
    ASSERT_TRUE(snapshot.cpu.load_average.has_value());
    EXPECT_EQ(snapshot.cpu.load_average->semantic, "processor_queue");
    EXPECT_TRUE(
        snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_LOAD_AVERAGE).available);
    // 契约 sysmon §2.5：Windows 负载代理必须带语义提示（口径不同、不得同轴比较）。
    ASSERT_TRUE(snapshot.availability.semantic_hints.contains("cpu.load_average"));
    EXPECT_FALSE(snapshot.availability.semantic_hints.at("cpu.load_average").empty());

    const nlohmann::json payload = snapshot;
    EXPECT_EQ(platform::SysmonSnapshot::validate(payload), std::nullopt);
}

TEST(SnapshotBuilderTest, LoadUnavailableWhenAbsent) {
    auto metrics = make_full_metrics();
    metrics.load_collected = false;
    metrics.load_average = std::nullopt;
    const auto snapshot = build_with(make_linux_info(), metrics);

    EXPECT_EQ(snapshot.cpu.load_average, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_LOAD_AVERAGE);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "collect_failed");
    EXPECT_TRUE(snapshot.availability.semantic_hints.empty());
}

TEST(SnapshotBuilderTest, ContextSwitchAndInterruptRatesMapped) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    ASSERT_TRUE(snapshot.cpu.context_switches_per_sec.has_value());
    EXPECT_DOUBLE_EQ(snapshot.cpu.context_switches_per_sec.value(), 1234.0);
    ASSERT_TRUE(snapshot.cpu.interrupts_per_sec.has_value());
    EXPECT_DOUBLE_EQ(snapshot.cpu.interrupts_per_sec.value(), 567.0);
    EXPECT_TRUE(
        snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_CONTEXT_SWITCHES).available);
}

TEST(SnapshotBuilderTest, FirstSampleRatesNullButAvailable) {
    auto metrics = make_full_metrics();
    metrics.context_switches_per_sec = std::nullopt;
    metrics.interrupts_per_sec = std::nullopt;
    const auto snapshot = build_with(make_linux_info(), metrics);

    // 能力可用但首帧数值未就绪：null 而不是 0。
    EXPECT_EQ(snapshot.cpu.context_switches_per_sec, std::nullopt);
    EXPECT_EQ(snapshot.cpu.interrupts_per_sec, std::nullopt);
    EXPECT_TRUE(
        snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_CONTEXT_SWITCHES).available);
}

TEST(SnapshotBuilderTest, RatesUnavailableIsNullWithReason) {
    auto metrics = make_full_metrics();
    metrics.rates_collected = false;
    metrics.context_switches_per_sec = 999.0;
    metrics.interrupts_per_sec = 999.0;
    const auto snapshot = build_with(make_linux_info(), metrics);

    EXPECT_EQ(snapshot.cpu.context_switches_per_sec, std::nullopt);
    EXPECT_EQ(snapshot.cpu.interrupts_per_sec, std::nullopt);
    const auto& entry =
        snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_CONTEXT_SWITCHES);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "collect_failed");
}

TEST(SnapshotBuilderTest, UnimplementedDomainsAreNull) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    EXPECT_EQ(snapshot.memory, std::nullopt);
    EXPECT_EQ(snapshot.swap, std::nullopt);
    EXPECT_EQ(snapshot.volumes, std::nullopt);
    EXPECT_EQ(snapshot.disk_io, std::nullopt);
    EXPECT_EQ(snapshot.network, std::nullopt);
    EXPECT_EQ(snapshot.shm_dir, std::nullopt);
}

TEST(SnapshotBuilderTest, AvailabilityCoversAllElevenKeys) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
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

    // 本票已落地：system/cpu.total/cpu.cores/cpu.load_average/cpu.context_switches 可用。
    for (const char* key :
         {platform::SYSMON_METRIC_SYSTEM, platform::SYSMON_METRIC_CPU_TOTAL,
          platform::SYSMON_METRIC_CPU_CORES, platform::SYSMON_METRIC_CPU_LOAD_AVERAGE,
          platform::SYSMON_METRIC_CPU_CONTEXT_SWITCHES}) {
        ASSERT_TRUE(snapshot.availability.metrics.contains(key));
        const auto& entry = snapshot.availability.metrics.at(key);
        EXPECT_TRUE(entry.available) << key;
        EXPECT_EQ(entry.reason, std::nullopt) << key;
    }
    // 其余 6 键：available=false + reason="not_implemented"
    for (const auto& [key, entry] : snapshot.availability.metrics) {
        if (key == platform::SYSMON_METRIC_SYSTEM || key == platform::SYSMON_METRIC_CPU_TOTAL ||
            key == platform::SYSMON_METRIC_CPU_CORES ||
            key == platform::SYSMON_METRIC_CPU_LOAD_AVERAGE ||
            key == platform::SYSMON_METRIC_CPU_CONTEXT_SWITCHES) {
            continue;
        }
        EXPECT_FALSE(entry.available) << key;
        ASSERT_TRUE(entry.reason.has_value()) << key;
        EXPECT_EQ(entry.reason.value(), "not_implemented") << key;
    }
}

TEST(SnapshotBuilderTest, LinuxPlatformDescriptionContainsKernel) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    EXPECT_EQ(snapshot.availability.platform_description,
              "Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)");
}

TEST(SnapshotBuilderTest, WindowsPlatformDescriptionContainsBuild) {
    auto info = make_linux_info();
    info.os_family = "windows";
    info.os_version = "Windows 10 Pro";
    info.kernel_version = "10.0.19045";
    info.os_build = 19045;
    const auto snapshot = build_with(info, make_full_metrics());

    EXPECT_EQ(snapshot.platform.build, 19045U);
    EXPECT_EQ(snapshot.availability.platform_description, "Windows 10 Pro (build 19045)");
}

TEST(SnapshotBuilderTest, SerializedSnapshotPassesPlatformValidation) {
    // 编码后必须能通过类型层唯一真相源的校验（形状契约的端到端自检）。
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    const nlohmann::json payload = snapshot;
    EXPECT_EQ(platform::SysmonSnapshot::validate(payload), std::nullopt);

    EXPECT_EQ(payload["timestamp"], kNowMs);
    EXPECT_DOUBLE_EQ(payload["cpu"]["total"]["usage_ratio"].get<double>(), 0.187);
    EXPECT_EQ(payload["cpu"]["cores"].size(), 3U);
    EXPECT_DOUBLE_EQ(payload["cpu"]["cores"][2]["usage_ratio"].get<double>(), 0.95);
    EXPECT_EQ(payload["cpu"]["saturated_cores"][0].get<uint32_t>(), 2U);
    EXPECT_EQ(payload["cpu"]["load_average"]["semantic"].get<std::string>(), "load_average");
    EXPECT_DOUBLE_EQ(payload["cpu"]["context_switches_per_sec"].get<double>(), 1234.0);
    EXPECT_TRUE(payload["memory"].is_null());
    EXPECT_EQ(payload["availability"]["metrics"].size(), platform::SYSMON_METRIC_KEYS.size());
}

TEST(SnapshotBuilderTest, SerializedFirstFramePassesValidationWithNullUsage) {
    auto metrics = make_full_metrics();
    metrics.total_usage_ratio = std::nullopt;
    for (auto& core : metrics.cores) {
        core.usage_ratio = std::nullopt;
        core.saturated = false;
    }
    metrics.saturated_cores.clear();
    metrics.context_switches_per_sec = std::nullopt;
    metrics.interrupts_per_sec = std::nullopt;

    const auto snapshot = build_with(make_linux_info(), metrics);
    const nlohmann::json payload = snapshot;
    EXPECT_EQ(platform::SysmonSnapshot::validate(payload), std::nullopt);
    EXPECT_TRUE(payload["cpu"]["total"]["usage_ratio"].is_null());
    EXPECT_TRUE(payload["cpu"]["cores"][0]["usage_ratio"].is_null());
    EXPECT_TRUE(payload["cpu"]["context_switches_per_sec"].is_null());
}

// 假采集器（实现 SystemCollector 接缝）确定性驱动速率计算与快照编码：
// 同一组输入在任何机器/时间都产出同一结果（契约 sysmon 采购接缝测试要求）。
class FakeCollector final : public platform::SystemCollector {
public:
    platform::RawSystemInfo info;
    std::optional<platform::RawCpuTimes> times;
    std::optional<platform::RawLoadAverage> load;

    platform::RawSystemInfo collect_system_info() override { return info; }
    std::optional<platform::RawCpuTimes> collect_cpu_times() override { return times; }
    std::optional<platform::RawLoadAverage> collect_load_average() override { return load; }
};

TEST(SnapshotBuilderTest, FakeCollectorDrivesRateCalculationAndEncoding) {
    FakeCollector fake;
    fake.info = make_linux_info();
    fake.times = platform::RawCpuTimes{};
    fake.times->total_ticks = 1000;
    fake.times->idle_ticks = 800;
    fake.times->context_switches = 1000;
    fake.times->interrupts = 500;
    fake.times->cores = {{0, 600, 400}, {1, 400, 400}};

    const auto first = fake.collect_cpu_times();
    ASSERT_TRUE(first.has_value());

    fake.times->total_ticks += 1000;
    fake.times->idle_ticks += 750;
    *fake.times->context_switches += 600;
    *fake.times->interrupts += 200;
    fake.times->cores[0].total_ticks += 600;
    fake.times->cores[0].idle_ticks += 450;
    fake.times->cores[1].total_ticks += 400;
    fake.times->cores[1].idle_ticks += 300;
    const auto second = fake.collect_cpu_times();
    ASSERT_TRUE(second.has_value());

    const auto usage = compute_cpu_usage(*first, *second);
    ASSERT_TRUE(usage.has_value());
    EXPECT_DOUBLE_EQ(*usage, 0.25);

    const auto core_usages = compute_core_usages(first->cores, second->cores);
    ASSERT_EQ(core_usages.size(), 2U);
    ASSERT_TRUE(core_usages[0].has_value());
    EXPECT_DOUBLE_EQ(*core_usages[0], 0.25);
    ASSERT_TRUE(core_usages[1].has_value());
    EXPECT_DOUBLE_EQ(*core_usages[1], 0.25);

    const auto switches_rate =
        compute_rate(*first->context_switches, *second->context_switches, 2.0);
    ASSERT_TRUE(switches_rate.has_value());
    EXPECT_DOUBLE_EQ(*switches_rate, 300.0);

    CpuMetrics metrics;
    metrics.total_usage_ratio = usage;
    metrics.cores_collected = true;
    for (std::size_t i = 0; i < second->cores.size(); ++i) {
        metrics.cores.push_back(make_core(second->cores[i].index, core_usages[i], false));
    }
    metrics.rates_collected = true;
    metrics.context_switches_per_sec = switches_rate;
    metrics.interrupts_per_sec = compute_rate(*first->interrupts, *second->interrupts, 2.0);
    fake.load = platform::RawLoadAverage{0.4, 0.5, 0.6, "load_average"};
    const auto load = fake.collect_load_average();
    ASSERT_TRUE(load.has_value());
    platform::SysmonLoadAverage average;
    average.one = load->one;
    average.five = load->five;
    average.fifteen = load->fifteen;
    average.semantic = load->semantic;
    metrics.load_average = average;

    const auto snapshot = build_with(fake.collect_system_info(), metrics);
    EXPECT_EQ(snapshot.platform.family, "linux");
    EXPECT_EQ(snapshot.cpu.cores.size(), 2U);
    ASSERT_TRUE(snapshot.cpu.total.usage_ratio.has_value());
    EXPECT_DOUBLE_EQ(*snapshot.cpu.total.usage_ratio, 0.25);
    EXPECT_EQ(snapshot.availability.metrics.at("cpu.cores").available, true);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

}  // namespace
}  // namespace dztrader::sysmon
