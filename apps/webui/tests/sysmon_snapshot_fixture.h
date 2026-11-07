#ifndef DZTRADER_WEBUI_TESTS_SYSMON_SNAPSHOT_FIXTURE_H_
#define DZTRADER_WEBUI_TESTS_SYSMON_SNAPSHOT_FIXTURE_H_

/**
 * @file sysmon_snapshot_fixture.h
 * @brief 契约合法的最小 SYSMON 快照夹具的**唯一实现**（#309 AC5）。
 *
 * 为什么必须只有一份（`docs/unit-test-standard.md` §5.4）：`sysmon_controller_test` 与
 * `sysmon_domain_service_test` 原先各有一份 `nlohmann::json` 内联快照，二者已实质漂移
 * ——controller 那份缺 `cpu.core_frequencies`（`validate_snapshot_cpu` 的必填键）与根级
 * `temperature` 占位，等于一份过不了 `SysmonSnapshot::validate` 的夹具；#295 审查
 * finding 5 的「两份 `availability.metrics` 13/15 vs 15/15」是同源的键数漂移。
 * `check_shared_test_headers.py` 只认 `#include` 关系，拦不住内联副本，因此形状
 * （键集合、clock 空态、availability 15 键）必须在唯一实现里固定；两处用例真正该表达
 * 的差异只有「本例关心的数值」（CPU 总量、型号、核数），用 `SysmonSnapshotSeed` 表达。
 *
 * 固定形状对齐《帧契约：系统监控》§2.5：`availability.metrics` 覆盖全部
 * `SYSMON_METRIC_KEYS`（15 键）；§2.9 `clock` 域为「尚未探测」空态
 * （`last_probe_unix_ms = 0`，测量字段全 null，`daemons` / `servers` 空数组）。
 * 快照必须通过 `platform::SysmonSnapshot::validate`——domain service 对坏帧静默丢弃，
 * 不校验的话用例只看得到「无广播」，看不出夹具已漂移。
 *
 * **header-only**：`apps/webui/CMakeLists.txt` 为 `tests/` 下每个 `*_test.cpp` 建一个
 * target，没有共享测试库；与 `apps/webui/tests/` 其它夹具头一样用 inline 实现。
 */
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace dztrader::test {

/// 两处调用点唯一合法的差异项（形状固定，数值随用例）。
struct SysmonSnapshotSeed {
    /// `cpu.total.usage_ratio`：controller 例 0.25，domain service 例 0.187
    double cpu_usage_ratio = 0.25;
    /// `system.cpu_model`
    std::string cpu_model = "test cpu";
    /// `system.logical_cores`
    std::uint64_t logical_cores = 8;
};

/// 契约合法的最小快照：`system` / `cpu.total` / `clock.sync_service` 可用，其余域 null
/// 且对应可用性键为 `not_implemented` / `no_reference`。
inline nlohmann::json sysmon_snapshot_fixture(const SysmonSnapshotSeed& seed = {}) {
    return {
        {"source", "dzsysmon"},
        {"timestamp", 1790000005000},
        {"sampling",
         {{"measure_interval_ms", 1000},
          {"last_sample_unix_ms", 1790000005123},
          {"missed_ticks", 0}}},
        {"platform",
         {{"family", "linux"},
          {"version", "Ubuntu 24.04.1 LTS"},
          {"kernel", "6.8.0-45-generic"},
          {"arch", "x86_64"},
          {"build", 0}}},
        {"system",
         {{"hostname", "trade-01"},
          {"cpu_model", seed.cpu_model},
          {"logical_cores", seed.logical_cores},
          {"boot_time_unix_sec", 1789990000},
          {"uptime_sec", 10005}}},
        {"cpu",
         {{"total", {{"usage_ratio", seed.cpu_usage_ratio}}},
          {"cores", nlohmann::json::array()},
          {"core_frequencies", nullptr},
          {"saturated_cores", nlohmann::json::array()},
          {"load_average", nullptr},
          {"context_switches_per_sec", nullptr},
          {"interrupts_per_sec", nullptr}}},
        {"temperature", nullptr},
        {"memory", nullptr},
        {"swap", nullptr},
        {"volumes", nullptr},
        {"disk_io", nullptr},
        {"network", nullptr},
        {"shm_dir", nullptr},
        {"clock",
         {{"offset_ms", nullptr},
          {"bound_ms", nullptr},
          {"source", ""},
          {"source_stratum", nullptr},
          {"last_probe_unix_ms", 0},
          {"servers", nlohmann::json::array()},
          {"daemons", nlohmann::json::array()},
          {"daemon_conflict", false},
          {"kernel_synced", nullptr}}},
        {"availability",
         {{"platform_description", "Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)"},
          {"metrics",
           {{"system", {{"available", true}}},
            {"cpu.total", {{"available", true}}},
            {"cpu.cores", {{"available", false}, {"reason", "not_implemented"}}},
            {"cpu.load_average", {{"available", false}, {"reason", "not_implemented"}}},
            {"cpu.context_switches", {{"available", false}, {"reason", "not_implemented"}}},
            {"cpu.frequency", {{"available", false}, {"reason", "not_implemented"}}},
            {"memory", {{"available", false}, {"reason", "not_implemented"}}},
            {"swap", {{"available", false}, {"reason", "not_implemented"}}},
            {"volumes", {{"available", false}, {"reason", "not_implemented"}}},
            {"disk_io", {{"available", false}, {"reason", "not_implemented"}}},
            {"network", {{"available", false}, {"reason", "not_implemented"}}},
            {"shm_dir", {{"available", false}, {"reason", "not_implemented"}}},
            {"temperature", {{"available", false}, {"reason", "not_implemented"}}},
            {"clock.offset", {{"available", false}, {"reason", "no_reference"}}},
            {"clock.sync_service", {{"available", true}}}}},
          {"semantic_hints", nlohmann::json::object()}}},
    };
}

}  // namespace dztrader::test

#endif  // DZTRADER_WEBUI_TESTS_SYSMON_SNAPSHOT_FIXTURE_H_
