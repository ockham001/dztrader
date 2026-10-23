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
                                    const CpuMetrics& metrics,
                                    const TemperatureMetrics& temperature = {},
                                    const MemorySwapMetrics& memory = {},
                                    const DiskMetrics& disk = {}) {
    return build_snapshot(kSourceName, info, metrics, temperature, memory, disk, kMeasureIntervalMs,
                          kLastSampleMs, kMissedTicks, kNowMs);
}

/// 本票（#73）落地后的典型温度读数。
TemperatureMetrics make_temperature_metrics() {
    TemperatureMetrics temperature;
    temperature.collected = true;
    temperature.sensors = std::vector<platform::SysmonTemperatureInfo>{
        {"coretemp:Package id 0", 45.5}, {"coretemp:Core 0", 44.0}};
    return temperature;
}

/// 本票（#73）落地后的典型每核频率读数。
CpuMetrics make_metrics_with_frequencies() {
    CpuMetrics metrics = make_full_metrics();
    metrics.frequency_collected = true;
    metrics.core_frequencies =
        std::vector<platform::SysmonCoreFrequency>{{0, 4200}, {1, 3600}, {2, 3600}};
    return metrics;
}

/// 本票（#74）落地后的典型内存/交换读数（Linux：cached/buffers 均可得，交换有值）。
MemorySwapMetrics make_memory_metrics() {
    MemorySwapMetrics metrics;
    metrics.collected = true;
    metrics.memory = platform::RawMemoryInfo{.total_bytes = 32ULL * 1024 * 1024 * 1024,
                                             .available_bytes = 16ULL * 1024 * 1024 * 1024,
                                             .cached_bytes = 8ULL * 1024 * 1024 * 1024,
                                             .buffers_bytes = 256ULL * 1024 * 1024};
    metrics.swap_collected = true;
    metrics.swap_supported = true;
    metrics.swap = platform::RawSwapInfo{.total_bytes = 8ULL * 1024 * 1024 * 1024,
                                         .free_bytes = 6ULL * 1024 * 1024 * 1024};
    return metrics;
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
    // 真实 load average 不需要口径提示；disk_io 的延迟/队列深度提示恒随快照携带（#76）。
    EXPECT_FALSE(snapshot.availability.semantic_hints.contains("cpu.load_average"));
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
    EXPECT_FALSE(snapshot.availability.semantic_hints.contains("cpu.load_average"));
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

TEST(SnapshotBuilderTest, UncollectedDomainsAreNull) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    // #74 未传入内存/交换测量结果（等效能力从未确认）→ 域为 null + collect_failed。
    EXPECT_EQ(snapshot.memory, std::nullopt);
    EXPECT_EQ(snapshot.swap, std::nullopt);
    EXPECT_EQ(snapshot.volumes, std::nullopt);
    EXPECT_EQ(snapshot.watch_paths, std::nullopt);
    EXPECT_EQ(snapshot.disk_io, std::nullopt);
    EXPECT_EQ(snapshot.network, std::nullopt);
    EXPECT_EQ(snapshot.shm_dir, std::nullopt);
    EXPECT_EQ(snapshot.temperature, std::nullopt);
    EXPECT_EQ(snapshot.cpu.core_frequencies, std::nullopt);
}

TEST(SnapshotBuilderTest, AvailabilityCoversAllThirteenKeys) {
    const auto snapshot = build_with(make_linux_info(), make_metrics_with_frequencies(),
                                     make_temperature_metrics(), make_memory_metrics());
    const std::vector<std::string> expected = {
        platform::SYSMON_METRIC_SYSTEM,
        platform::SYSMON_METRIC_CPU_TOTAL,
        platform::SYSMON_METRIC_CPU_CORES,
        platform::SYSMON_METRIC_CPU_LOAD_AVERAGE,
        platform::SYSMON_METRIC_CPU_CONTEXT_SWITCHES,
        platform::SYSMON_METRIC_CPU_FREQUENCY,
        platform::SYSMON_METRIC_MEMORY,
        platform::SYSMON_METRIC_SWAP,
        platform::SYSMON_METRIC_VOLUMES,
        platform::SYSMON_METRIC_DISK_IO,
        platform::SYSMON_METRIC_NETWORK,
        platform::SYSMON_METRIC_SHM_DIR,
        platform::SYSMON_METRIC_TEMPERATURE,
    };
    EXPECT_EQ(snapshot.availability.metrics.size(), expected.size());
    EXPECT_EQ(expected.size(), platform::SYSMON_METRIC_KEYS.size());
    for (const auto& key : expected) {
        EXPECT_TRUE(snapshot.availability.metrics.contains(key)) << key;
    }

    // 已落地：system/cpu.*/memory/swap/temperature 在输入就绪时可用。
    for (const char* key :
         {platform::SYSMON_METRIC_SYSTEM, platform::SYSMON_METRIC_CPU_TOTAL,
          platform::SYSMON_METRIC_CPU_CORES, platform::SYSMON_METRIC_CPU_LOAD_AVERAGE,
          platform::SYSMON_METRIC_CPU_CONTEXT_SWITCHES, platform::SYSMON_METRIC_CPU_FREQUENCY,
          platform::SYSMON_METRIC_MEMORY, platform::SYSMON_METRIC_SWAP,
          platform::SYSMON_METRIC_TEMPERATURE}) {
        ASSERT_TRUE(snapshot.availability.metrics.contains(key));
        const auto& entry = snapshot.availability.metrics.at(key);
        EXPECT_TRUE(entry.available) << key;
        EXPECT_EQ(entry.reason, std::nullopt) << key;
    }
    // #75 起 volumes/shm_dir 已实现：本用例未传盘测结果（能力从未确认）→ collect_failed。
    for (const char* key : {platform::SYSMON_METRIC_VOLUMES, platform::SYSMON_METRIC_SHM_DIR,
                            platform::SYSMON_METRIC_DISK_IO}) {
        const auto& entry = snapshot.availability.metrics.at(key);
        EXPECT_FALSE(entry.available) << key;
        ASSERT_TRUE(entry.reason.has_value()) << key;
        EXPECT_EQ(entry.reason.value(), "collect_failed") << key;
    }
    // 网络仍未实现：available=false + reason="not_implemented"
    {
        const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_NETWORK);
        EXPECT_FALSE(entry.available);
        ASSERT_TRUE(entry.reason.has_value());
        EXPECT_EQ(entry.reason.value(), "not_implemented");
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
    const auto snapshot =
        build_with(make_linux_info(), make_full_metrics(), {}, make_memory_metrics());
    const nlohmann::json payload = snapshot;
    EXPECT_EQ(platform::SysmonSnapshot::validate(payload), std::nullopt);

    EXPECT_EQ(payload["timestamp"], kNowMs);
    EXPECT_DOUBLE_EQ(payload["cpu"]["total"]["usage_ratio"].get<double>(), 0.187);
    EXPECT_EQ(payload["cpu"]["cores"].size(), 3U);
    EXPECT_DOUBLE_EQ(payload["cpu"]["cores"][2]["usage_ratio"].get<double>(), 0.95);
    EXPECT_EQ(payload["cpu"]["saturated_cores"][0].get<uint32_t>(), 2U);
    EXPECT_EQ(payload["cpu"]["load_average"]["semantic"].get<std::string>(), "load_average");
    EXPECT_DOUBLE_EQ(payload["cpu"]["context_switches_per_sec"].get<double>(), 1234.0);
    EXPECT_TRUE(payload["cpu"]["core_frequencies"].is_null());
    EXPECT_TRUE(payload["temperature"].is_null());
    ASSERT_FALSE(payload["memory"].is_null());
    EXPECT_EQ(payload["memory"]["total_bytes"].get<uint64_t>(), 32ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(payload["memory"]["available_bytes"].get<uint64_t>(), 16ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(payload["memory"]["used_bytes"].get<uint64_t>(), 16ULL * 1024 * 1024 * 1024);
    EXPECT_DOUBLE_EQ(payload["memory"]["used_ratio"].get<double>(), 0.5);
    EXPECT_FALSE(payload["memory"].contains("free_bytes"));
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

TEST(SnapshotBuilderTest, TemperatureMappedWhenCollected) {
    const auto snapshot =
        build_with(make_linux_info(), make_full_metrics(), make_temperature_metrics());
    ASSERT_TRUE(snapshot.temperature.has_value());
    ASSERT_EQ(snapshot.temperature->size(), 2U);
    EXPECT_EQ(snapshot.temperature->at(0).sensor, "coretemp:Package id 0");
    EXPECT_DOUBLE_EQ(snapshot.temperature->at(0).celsius, 45.5);
    EXPECT_EQ(snapshot.temperature->at(1).sensor, "coretemp:Core 0");
    EXPECT_DOUBLE_EQ(snapshot.temperature->at(1).celsius, 44.0);

    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_TEMPERATURE);
    EXPECT_TRUE(entry.available);
    EXPECT_EQ(entry.reason, std::nullopt);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, TemperatureCollectedButNoReadingKeepsAvailable) {
    // 能力粘性（契约 sysmon §2.5、§2.7）：本帧未读到温度时数值 null，可用性仍为 true，
    // 消费方据此保留区块并显示「暂无数据」而不是「不可用」。
    TemperatureMetrics temperature;
    temperature.collected = true;
    temperature.sensors = std::nullopt;
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), temperature);

    EXPECT_EQ(snapshot.temperature, std::nullopt);
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_TEMPERATURE).available);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, TemperatureUnavailableOnLinuxIsNoSensor) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    EXPECT_EQ(snapshot.temperature, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_TEMPERATURE);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "no_sensor");
}

TEST(SnapshotBuilderTest, TemperatureUnavailableOnWindowsIsUnsupported) {
    auto info = make_linux_info();
    info.os_family = "windows";
    info.os_version = "Windows 10 Pro";
    info.kernel_version = "10.0.19045";
    info.os_build = 19045;
    const auto snapshot = build_with(info, make_full_metrics());

    // 契约 sysmon §8：Windows 不提供温度（恒 null），可用性项不可用。
    EXPECT_EQ(snapshot.temperature, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_TEMPERATURE);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "unsupported");
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, TemperatureDisabledByConfigWinsOverCollected) {
    auto temperature = make_temperature_metrics();
    temperature.disabled = true;
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), temperature);

    // 配置关闭优先：数值清空、可用性 disabled_by_config（契约 sysmon §2.5、§2.7）。
    EXPECT_EQ(snapshot.temperature, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_TEMPERATURE);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "disabled_by_config");
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, CoreFrequenciesMappedWhenCollected) {
    const auto snapshot =
        build_with(make_linux_info(), make_metrics_with_frequencies(), make_temperature_metrics());
    ASSERT_TRUE(snapshot.cpu.core_frequencies.has_value());
    ASSERT_EQ(snapshot.cpu.core_frequencies->size(), 3U);
    EXPECT_EQ(snapshot.cpu.core_frequencies->at(0).index, 0U);
    EXPECT_EQ(snapshot.cpu.core_frequencies->at(0).mhz, 4200U);
    EXPECT_EQ(snapshot.cpu.core_frequencies->at(1).index, 1U);
    EXPECT_EQ(snapshot.cpu.core_frequencies->at(1).mhz, 3600U);

    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_FREQUENCY);
    EXPECT_TRUE(entry.available);
    EXPECT_EQ(entry.reason, std::nullopt);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, CoreFrequenciesCollectedButNoReadingKeepsAvailable) {
    auto metrics = make_full_metrics();
    metrics.frequency_collected = true;
    metrics.core_frequencies = std::nullopt;
    const auto snapshot = build_with(make_linux_info(), metrics);

    EXPECT_EQ(snapshot.cpu.core_frequencies, std::nullopt);
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_FREQUENCY).available);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, CoreFrequenciesUnavailableIsUnsupported) {
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    EXPECT_EQ(snapshot.cpu.core_frequencies, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_FREQUENCY);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "unsupported");
}

TEST(SnapshotBuilderTest, CoreFrequenciesDisabledByConfigWinsOverCollected) {
    auto metrics = make_metrics_with_frequencies();
    metrics.frequency_disabled = true;
    const auto snapshot = build_with(make_linux_info(), metrics);

    EXPECT_EQ(snapshot.cpu.core_frequencies, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_CPU_FREQUENCY);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "disabled_by_config");
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, SerializedSensorDataPassesPlatformValidation) {
    const auto snapshot =
        build_with(make_linux_info(), make_metrics_with_frequencies(), make_temperature_metrics());
    const nlohmann::json payload = snapshot;
    EXPECT_EQ(platform::SysmonSnapshot::validate(payload), std::nullopt);
    ASSERT_EQ(payload["cpu"]["core_frequencies"].size(), 3U);
    EXPECT_EQ(payload["cpu"]["core_frequencies"][0]["index"].get<uint32_t>(), 0U);
    EXPECT_EQ(payload["cpu"]["core_frequencies"][0]["mhz"].get<uint32_t>(), 4200U);
    ASSERT_EQ(payload["temperature"].size(), 2U);
    EXPECT_EQ(payload["temperature"][0]["sensor"].get<std::string>(), "coretemp:Package id 0");
    EXPECT_DOUBLE_EQ(payload["temperature"][0]["celsius"].get<double>(), 45.5);
}

// ===== #74 内存与交换 =====

TEST(SnapshotBuilderTest, MemoryMappedAndValidated) {
    const auto snapshot =
        build_with(make_linux_info(), make_full_metrics(), {}, make_memory_metrics());
    ASSERT_TRUE(snapshot.memory.has_value());
    EXPECT_EQ(snapshot.memory->total_bytes, 32ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(snapshot.memory->available_bytes, 16ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(snapshot.memory->used_bytes, 16ULL * 1024 * 1024 * 1024);
    ASSERT_TRUE(snapshot.memory->used_ratio.has_value());
    EXPECT_DOUBLE_EQ(*snapshot.memory->used_ratio, 0.5);
    ASSERT_TRUE(snapshot.memory->cached_bytes.has_value());
    EXPECT_EQ(*snapshot.memory->cached_bytes, 8ULL * 1024 * 1024 * 1024);
    ASSERT_TRUE(snapshot.memory->buffers_bytes.has_value());
    EXPECT_EQ(*snapshot.memory->buffers_bytes, 256ULL * 1024 * 1024);

    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_MEMORY);
    EXPECT_TRUE(entry.available);
    EXPECT_EQ(entry.reason, std::nullopt);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, MemoryClampsAnomalousValues) {
    // 采集层已归一；快照构造再做一层防御：available/cached/buffers 不得超过 total
    // （否则 used = total - available 会下溢、快照校验会失败）。
    MemorySwapMetrics memory;
    memory.collected = true;
    memory.memory = platform::RawMemoryInfo{.total_bytes = 1024ULL,
                                            .available_bytes = 4096ULL,
                                            .cached_bytes = 4096ULL,
                                            .buffers_bytes = 2048ULL};
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, memory);

    ASSERT_TRUE(snapshot.memory.has_value());
    EXPECT_EQ(snapshot.memory->available_bytes, 1024ULL);  // 钳制到 total
    EXPECT_EQ(snapshot.memory->used_bytes, 0ULL);
    ASSERT_TRUE(snapshot.memory->used_ratio.has_value());
    EXPECT_DOUBLE_EQ(*snapshot.memory->used_ratio, 0.0);
    EXPECT_EQ(snapshot.memory->cached_bytes.value_or(0), 1024ULL);
    EXPECT_EQ(snapshot.memory->buffers_bytes.value_or(0), 1024ULL);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, MemoryCollectedButNoReadingKeepsAvailable) {
    // 能力粘性：本帧读取失败只清数值，可用性保持 true（消费方显示「暂无数据」）。
    MemorySwapMetrics memory;
    memory.collected = true;
    memory.memory = std::nullopt;
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, memory);

    EXPECT_EQ(snapshot.memory, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_MEMORY);
    EXPECT_TRUE(entry.available);
    EXPECT_EQ(entry.reason, std::nullopt);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, MemoryDisabledByConfigWinsOverCollected) {
    auto memory = make_memory_metrics();
    memory.disabled = true;
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, memory);

    EXPECT_EQ(snapshot.memory, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_MEMORY);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "disabled_by_config");
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, SwapMappedWithRatio) {
    const auto snapshot =
        build_with(make_linux_info(), make_full_metrics(), {}, make_memory_metrics());
    ASSERT_TRUE(snapshot.swap.has_value());
    EXPECT_EQ(snapshot.swap->total_bytes, 8ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(snapshot.swap->used_bytes, 2ULL * 1024 * 1024 * 1024);
    ASSERT_TRUE(snapshot.swap->used_ratio.has_value());
    EXPECT_DOUBLE_EQ(*snapshot.swap->used_ratio, 0.25);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_SWAP);
    EXPECT_TRUE(entry.available);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, SwapWithoutSwapSpaceIsUnsupported) {
    // 无交换分区/无页面文件：payload 输出 total=0 的对象，可用性 unavailable+unsupported
    // （契约 sysmon §2.4、§2.5）；前端据此显示「无交换分区」而不是 0 或旧数据。
    MemorySwapMetrics memory;
    memory.collected = true;
    memory.memory = platform::RawMemoryInfo{.total_bytes = 1024ULL,
                                            .available_bytes = 512ULL,
                                            .cached_bytes = std::nullopt,
                                            .buffers_bytes = std::nullopt};
    memory.swap_collected = true;
    memory.swap_supported = false;
    memory.swap = platform::RawSwapInfo{};
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, memory);

    ASSERT_TRUE(snapshot.swap.has_value());
    EXPECT_EQ(snapshot.swap->total_bytes, 0ULL);
    EXPECT_EQ(snapshot.swap->used_bytes, 0ULL);
    EXPECT_EQ(snapshot.swap->used_ratio, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_SWAP);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "unsupported");
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, SwapClampsFreeAboveTotal) {
    MemorySwapMetrics memory;
    memory.swap_collected = true;
    memory.swap_supported = true;
    memory.swap = platform::RawSwapInfo{.total_bytes = 1024ULL, .free_bytes = 4096ULL};
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, memory);

    ASSERT_TRUE(snapshot.swap.has_value());
    EXPECT_EQ(snapshot.swap->used_bytes, 0ULL);
    ASSERT_TRUE(snapshot.swap->used_ratio.has_value());
    EXPECT_DOUBLE_EQ(*snapshot.swap->used_ratio, 0.0);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, SwapCollectedButNoReadingKeepsAvailable) {
    MemorySwapMetrics memory;
    memory.swap_collected = true;
    memory.swap_supported = true;
    memory.swap = std::nullopt;
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, memory);

    EXPECT_EQ(snapshot.swap, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_SWAP);
    EXPECT_TRUE(entry.available);
    EXPECT_EQ(entry.reason, std::nullopt);
}

TEST(SnapshotBuilderTest, SwapDisabledByConfigWinsOverCollected) {
    auto memory = make_memory_metrics();
    memory.swap_disabled = true;
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, memory);

    EXPECT_EQ(snapshot.swap, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_SWAP);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "disabled_by_config");
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

// 假采集器（实现 SystemCollector 接缝）确定性驱动速率计算与快照编码：
// 同一组输入在任何机器/时间都产出同一结果（契约 sysmon 采购接缝测试要求）。
class FakeCollector final : public platform::SystemCollector {
public:
    platform::RawSystemInfo info;
    std::optional<platform::RawCpuTimes> times;
    std::optional<platform::RawLoadAverage> load;
    std::optional<std::vector<platform::RawTemperature>> temperatures;
    std::optional<std::vector<platform::RawCoreFrequency>> core_frequencies;
    std::optional<platform::RawMemoryInfo> memory;
    std::optional<platform::RawSwapInfo> swap;
    std::optional<std::vector<platform::RawVolumeInfo>> volumes;
    std::vector<platform::RawWatchPathInfo> watch_paths;
    std::optional<platform::RawShmDirInfo> shm_dir;
    std::optional<std::vector<platform::RawDiskIoInfo>> disk_io;

    platform::RawSystemInfo collect_system_info() override { return info; }
    std::optional<platform::RawCpuTimes> collect_cpu_times() override { return times; }
    std::optional<platform::RawLoadAverage> collect_load_average() override { return load; }
    std::optional<std::vector<platform::RawTemperature>> collect_temperatures() override {
        return temperatures;
    }
    std::optional<std::vector<platform::RawCoreFrequency>> collect_core_frequencies() override {
        return core_frequencies;
    }
    std::optional<platform::RawMemoryInfo> collect_memory() override { return memory; }
    std::optional<platform::RawSwapInfo> collect_swap() override { return swap; }
    std::optional<std::vector<platform::RawVolumeInfo>> collect_volumes() override {
        return volumes;
    }
    std::vector<platform::RawWatchPathInfo> collect_watch_paths(
        const std::vector<std::string>& paths) override {
        (void)paths;
        return watch_paths;
    }
    std::optional<platform::RawShmDirInfo> collect_shm_dir(
        const std::string& shm_dir_path) override {
        (void)shm_dir_path;
        return shm_dir;
    }
    std::optional<std::vector<platform::RawDiskIoInfo>> collect_disk_io() override {
        return disk_io;
    }
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

// ===== #75：卷、关注路径与共享内存目录 =====

/// #75 落地后的典型盘测数据：两个可读卷（其一被关注）+ 一个读不到容量的卷。
DiskMetrics make_disk_metrics() {
    DiskMetrics disk;
    disk.volumes_collected = true;
    disk.volumes = std::vector<platform::RawVolumeInfo>{
        {.mount = "/",
         .total_bytes = 100ULL * 1024 * 1024 * 1024,
         .free_bytes = 40ULL * 1024 * 1024 * 1024,
         .reason = ""},
        {.mount = "/data",
         .total_bytes = 1000ULL * 1024 * 1024 * 1024,
         .free_bytes = 750ULL * 1024 * 1024 * 1024,
         .reason = ""},
        {.mount = "/backup",
         .total_bytes = std::nullopt,
         .free_bytes = std::nullopt,
         .reason = "no_permission"},
    };
    disk.watch_paths = std::vector<platform::RawWatchPathInfo>{
        {.path = "/srv/dztrader/shm", .mount = "/data", .reason = ""},
        {.path = "/srv/dztrader/db", .mount = "/data", .reason = ""},
    };
    disk.shm_dir_collected = true;
    disk.shm_dir = platform::RawShmDirInfo{.path = "/srv/dztrader/shm",
                                           .total_bytes = 754'974'720,
                                           .page_files = 12,
                                           .oldest_file_age_sec = 480};
    return disk;
}

TEST(SnapshotBuilderTest, DiskDomainsAreEncodedWhenCollected) {
    const auto snapshot =
        build_with(make_linux_info(), make_full_metrics(), {}, {}, make_disk_metrics());

    ASSERT_TRUE(snapshot.volumes.has_value());
    ASSERT_EQ(snapshot.volumes->size(), 3U);
    // used = total - free，比率由之计算（契约 sysmon §2.4）。
    const auto& root = (*snapshot.volumes)[0];
    EXPECT_EQ(root.mount, "/");
    EXPECT_FALSE(root.watched);
    EXPECT_TRUE(root.available);
    ASSERT_TRUE(root.total_bytes.has_value());
    ASSERT_TRUE(root.used_bytes.has_value());
    EXPECT_EQ(*root.used_bytes, 60ULL * 1024 * 1024 * 1024);
    ASSERT_TRUE(root.used_ratio.has_value());
    EXPECT_DOUBLE_EQ(*root.used_ratio, 0.6);

    // 关注路径落在的卷被标记 watched（契约 sysmon §2.4）。
    EXPECT_TRUE((*snapshot.volumes)[1].watched);

    // 读不到容量的卷：available=false + reason，容量字段保持 null（不得用 0 冒充）。
    const auto& backup = (*snapshot.volumes)[2];
    EXPECT_EQ(backup.mount, "/backup");
    EXPECT_FALSE(backup.available);
    EXPECT_FALSE(backup.total_bytes.has_value());
    EXPECT_FALSE(backup.used_bytes.has_value());
    EXPECT_FALSE(backup.used_ratio.has_value());
    ASSERT_TRUE(backup.reason.has_value());
    EXPECT_EQ(*backup.reason, "no_permission");

    ASSERT_TRUE(snapshot.watch_paths.has_value());
    ASSERT_EQ(snapshot.watch_paths->size(), 2U);
    ASSERT_TRUE((*snapshot.watch_paths)[0].mount.has_value());
    EXPECT_EQ(*(*snapshot.watch_paths)[0].mount, "/data");
    EXPECT_TRUE((*snapshot.watch_paths)[0].available);

    ASSERT_TRUE(snapshot.shm_dir.has_value());
    EXPECT_EQ(snapshot.shm_dir->path, "/srv/dztrader/shm");
    EXPECT_EQ(snapshot.shm_dir->total_bytes, 754'974'720U);
    EXPECT_EQ(snapshot.shm_dir->page_files, 12U);
    ASSERT_TRUE(snapshot.shm_dir->oldest_file_age_sec.has_value());
    EXPECT_EQ(*snapshot.shm_dir->oldest_file_age_sec, 480);

    // 可用性与契约校验：整帧必须通过 SysmonSnapshot::validate。
    EXPECT_TRUE(snapshot.availability.metrics.at("volumes").available);
    EXPECT_TRUE(snapshot.availability.metrics.at("shm_dir").available);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, UnreadableVolumeStaysInArray) {
    // 单个卷不可读不改写整体可用性：卷仍在数组里，整体仍 available（契约 sysmon §2.5）。
    const auto snapshot =
        build_with(make_linux_info(), make_full_metrics(), {}, {}, make_disk_metrics());
    EXPECT_TRUE(snapshot.availability.metrics.at("volumes").available);
    ASSERT_TRUE(snapshot.volumes.has_value());
    EXPECT_EQ(snapshot.volumes->size(), 3U);
}

TEST(SnapshotBuilderTest, FailedWatchPathResolutionIsReportedNotGuessed) {
    DiskMetrics disk = make_disk_metrics();
    disk.watch_paths = std::vector<platform::RawWatchPathInfo>{
        {.path = "/srv/dztrader/missing", .mount = std::nullopt, .reason = "path_not_found"},
    };
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, {}, disk);

    ASSERT_TRUE(snapshot.watch_paths.has_value());
    ASSERT_EQ(snapshot.watch_paths->size(), 1U);
    const auto& info = (*snapshot.watch_paths)[0];
    EXPECT_FALSE(info.available);
    EXPECT_FALSE(info.mount.has_value());  // 不得猜卷、不得回退到 "/"
    ASSERT_TRUE(info.reason.has_value());
    EXPECT_EQ(*info.reason, "path_not_found");
    // 落卷全失败时没有任何卷被标记 watched。
    ASSERT_TRUE(snapshot.volumes.has_value());
    for (const auto& volume : *snapshot.volumes) {
        EXPECT_FALSE(volume.watched) << volume.mount;
    }
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, WatchPathsAreNotGatedByVolumesGroup) {
    // 告警需要落卷信息（#80）：关闭 volumes 组后 volumes 为 null，但 watch_paths 仍输出
    // （契约 sysmon §2.5）。
    DiskMetrics disk = make_disk_metrics();
    disk.volumes_disabled = true;
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, {}, disk);

    EXPECT_EQ(snapshot.volumes, std::nullopt);
    ASSERT_TRUE(snapshot.watch_paths.has_value());
    EXPECT_EQ(snapshot.watch_paths->size(), 2U);
    const auto& entry = snapshot.availability.metrics.at("volumes");
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(*entry.reason, "disabled_by_config");
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, ShmDirUnavailableIsNullNotZeroObject) {
    DiskMetrics disk = make_disk_metrics();
    disk.shm_dir = std::nullopt;  // 本帧读不到（能力仍标记为已确认，粘性）
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, {}, disk);

    // 读不到不是"目录为空"：域为 null，可用性保持 available（粘性，契约 sysmon §2.5）。
    EXPECT_EQ(snapshot.shm_dir, std::nullopt);
    EXPECT_TRUE(snapshot.availability.metrics.at("shm_dir").available);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, DiskDomainsUnavailableBeforeFirstCollection) {
    // 能力从未确认（本测试不传 disk 指标）：域为 null + collect_failed。
    const auto snapshot = build_with(make_linux_info(), make_full_metrics());
    EXPECT_EQ(snapshot.volumes, std::nullopt);
    EXPECT_EQ(snapshot.watch_paths, std::nullopt);
    EXPECT_EQ(snapshot.shm_dir, std::nullopt);
    for (const char* key : {"volumes", "shm_dir"}) {
        const auto& entry = snapshot.availability.metrics.at(key);
        EXPECT_FALSE(entry.available) << key;
        ASSERT_TRUE(entry.reason.has_value()) << key;
        EXPECT_EQ(*entry.reason, "collect_failed") << key;
    }
}

// ===== #76：每物理盘 I/O（契约 sysmon §2.4、§2.5、§2.7）=====

/// #76 落地后的典型盘测数据：两块物理盘、首帧之外各派生字段均有值。
DiskMetrics make_disk_io_metrics() {
    DiskMetrics disk;
    disk.disk_io_collected = true;
    disk.disk_io = std::vector<platform::SysmonDiskIoInfo>{
        platform::SysmonDiskIoInfo{.device = "sda",
                                   .read_bytes_per_sec = 1'000'000.0,
                                   .write_bytes_per_sec = 2'000'000.0,
                                   .read_iops = 15.0,
                                   .write_iops = 5.0,
                                   .latency_ms = 1.25,
                                   .queue_depth = 3.0,
                                   .utilization = 0.5},
        platform::SysmonDiskIoInfo{.device = "nvme0n1",
                                   .read_bytes_per_sec = 0.0,
                                   .write_bytes_per_sec = 0.0,
                                   .read_iops = 0.0,
                                   .write_iops = 0.0,
                                   .latency_ms = std::nullopt,
                                   .queue_depth = 0.0,
                                   .utilization = 0.0},
    };
    return disk;
}

TEST(SnapshotBuilderTest, DiskIoEncodedWhenCollected) {
    const auto snapshot =
        build_with(make_linux_info(), make_full_metrics(), {}, {}, make_disk_io_metrics());

    ASSERT_TRUE(snapshot.disk_io.has_value());
    ASSERT_EQ(snapshot.disk_io->size(), 2U);
    const auto& first = (*snapshot.disk_io)[0];
    EXPECT_EQ(first.device, "sda");
    EXPECT_DOUBLE_EQ(first.read_bytes_per_sec.value(), 1'000'000.0);
    EXPECT_DOUBLE_EQ(first.utilization.value(), 0.5);
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_DISK_IO).available);
    // 口径提示随快照携带（契约 sysmon §2.5）：延迟是每请求平均，队列深度是瞬时值。
    const auto hint = snapshot.availability.semantic_hints.at("disk_io");
    EXPECT_FALSE(hint.empty());
    EXPECT_NE(hint.find("latency"), std::string::npos);
    EXPECT_NE(hint.find("queue depth"), std::string::npos);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, DiskIoFirstFrameValuesStayNullNotZero) {
    // 首次采样：设备条目存在但派生字段全为 null（契约 sysmon §2.7：不得用 0 冒充）。
    DiskMetrics disk;
    disk.disk_io_collected = true;
    platform::SysmonDiskIoInfo first_frame;
    first_frame.device = "sda";
    disk.disk_io = std::vector<platform::SysmonDiskIoInfo>{first_frame};
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, {}, disk);

    ASSERT_TRUE(snapshot.disk_io.has_value());
    ASSERT_EQ(snapshot.disk_io->size(), 1U);
    EXPECT_FALSE((*snapshot.disk_io)[0].read_bytes_per_sec.has_value());
    EXPECT_FALSE((*snapshot.disk_io)[0].latency_ms.has_value());
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_DISK_IO).available);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, DiskIoCollectedButNoReadingKeepsAvailable) {
    // 能力已确认（粘性）但本帧没采到：域为 null，可用性仍 available（契约 §2.5）。
    DiskMetrics disk;
    disk.disk_io_collected = true;
    disk.disk_io = std::nullopt;
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, {}, disk);

    EXPECT_EQ(snapshot.disk_io, std::nullopt);
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_DISK_IO).available);
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

TEST(SnapshotBuilderTest, DiskIoEmptyArrayWhenNoPhysicalDisk) {
    // 采集成功但本机没有物理盘：空数组（不是 null，也不是不可用）。
    DiskMetrics disk;
    disk.disk_io_collected = true;
    disk.disk_io = std::vector<platform::SysmonDiskIoInfo>{};
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, {}, disk);

    ASSERT_TRUE(snapshot.disk_io.has_value());
    EXPECT_TRUE(snapshot.disk_io->empty());
    EXPECT_TRUE(snapshot.availability.metrics.at(platform::SYSMON_METRIC_DISK_IO).available);
}

TEST(SnapshotBuilderTest, DiskIoUnavailableReasonsDependOnPlatform) {
    // Linux：/proc 不可读 → collect_failed；Windows：计数器需特权/未开启 → no_permission
    // （契约 sysmon §2.4/§2.5：不可用是常态而非异常，读不到与读到 0 必须区分）。
    const auto linux_snapshot = build_with(make_linux_info(), make_full_metrics());
    {
        const auto& entry = linux_snapshot.availability.metrics.at(platform::SYSMON_METRIC_DISK_IO);
        EXPECT_FALSE(entry.available);
        ASSERT_TRUE(entry.reason.has_value());
        EXPECT_EQ(entry.reason.value(), "collect_failed");
        EXPECT_EQ(linux_snapshot.disk_io, std::nullopt);
    }

    auto windows_info = make_linux_info();
    windows_info.os_family = "windows";
    windows_info.os_version = "Windows 10 Pro";
    windows_info.os_build = 19045;
    const auto windows_snapshot = build_with(windows_info, make_full_metrics());
    {
        const auto& entry =
            windows_snapshot.availability.metrics.at(platform::SYSMON_METRIC_DISK_IO);
        EXPECT_FALSE(entry.available);
        ASSERT_TRUE(entry.reason.has_value());
        EXPECT_EQ(entry.reason.value(), "no_permission");
        EXPECT_EQ(windows_snapshot.disk_io, std::nullopt);
    }
}

TEST(SnapshotBuilderTest, DiskIoDisabledByConfigWinsOverCollected) {
    DiskMetrics disk = make_disk_io_metrics();
    disk.disk_io_disabled = true;
    const auto snapshot = build_with(make_linux_info(), make_full_metrics(), {}, {}, disk);

    EXPECT_EQ(snapshot.disk_io, std::nullopt);
    const auto& entry = snapshot.availability.metrics.at(platform::SYSMON_METRIC_DISK_IO);
    EXPECT_FALSE(entry.available);
    ASSERT_TRUE(entry.reason.has_value());
    EXPECT_EQ(entry.reason.value(), "disabled_by_config");
    EXPECT_EQ(platform::SysmonSnapshot::validate(nlohmann::json(snapshot)), std::nullopt);
}

}  // namespace
}  // namespace dztrader::sysmon
