#include <dztrader/platform/sysmon.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace dztrader::platform {
namespace {

using nlohmann::json;

/// 自描述错误前缀：`path | reason`（英文、小写开头、不写句号）。
std::string err(std::string_view path, std::string_view reason) {
    return std::string(path) + " | " + std::string(reason);
}

bool is_number(const json& j) { return j.is_number(); }

/// 非负整数：nlohmann 对正数可能给 signed（字段声明 int64_t）或 unsigned（uint64_t），两者都合法。
bool is_non_negative_integer(const json& j) {
    if (!j.is_number_integer()) {
        return false;
    }
    if (j.is_number_unsigned()) {
        return true;
    }
    return j.get<int64_t>() >= 0;
}

/// 数值是否在闭区间内（浮点专用）。
bool in_range(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

/// 整数是否在闭区间内；非整数类型返回 false。
template <typename T>
bool int_in_range(const json& j, T lo, T hi) {
    if (!j.is_number_integer()) {
        return false;
    }
    const auto v = j.get<int64_t>();
    return v >= static_cast<int64_t>(lo) && v <= static_cast<int64_t>(hi);
}

/// 递归检查 JSON 中是否存在显式 null（general §8：除契约显式允许外一律非法）。
bool contains_null(const json& j) {
    if (j.is_null()) {
        return true;
    }
    if (j.is_object()) {
        for (const auto& [k, v] : j.items()) {
            (void)k;
            if (contains_null(v)) {
                return true;
            }
        }
    } else if (j.is_array()) {
        for (const auto& v : j) {
            if (contains_null(v)) {
                return true;
            }
        }
    }
    return false;
}

/// 校验可选速率字段：缺失/null 合法；数值必须 >= 0。
std::optional<std::string> validate_optional_rate(const json& parent,
                                                  std::string_view parent_path,
                                                  const char* key) {
    if (!parent.contains(key) || parent[key].is_null()) {
        return std::nullopt;
    }
    if (!is_number(parent[key])) {
        return err(std::string(parent_path) + "." + key, "must be a number or null");
    }
    if (parent[key].get<double>() < 0.0) {
        return err(std::string(parent_path) + "." + key, "must be >= 0");
    }
    return std::nullopt;
}

/// 校验可选比值字段：缺失/null 合法；数值必须位于 [0, 1]。
std::optional<std::string> validate_optional_ratio(const json& parent,
                                                   std::string_view parent_path,
                                                   const char* key) {
    if (!parent.contains(key) || parent[key].is_null()) {
        return std::nullopt;
    }
    if (!is_number(parent[key])) {
        return err(std::string(parent_path) + "." + key, "must be a number or null");
    }
    if (!in_range(parent[key].get<double>(), 0.0, 1.0)) {
        return err(std::string(parent_path) + "." + key, "must be within [0, 1]");
    }
    return std::nullopt;
}

std::optional<std::string> require_non_empty_string(const json& j,
                                                    std::string_view path,
                                                    const char* key) {
    if (!j.contains(key) || !j[key].is_string() || j[key].get<std::string>().empty()) {
        return err(std::string(path) + "." + key, "must be a non-empty string");
    }
    return std::nullopt;
}

std::optional<std::string> require_uint(const json& j,
                                        std::string_view path,
                                        const char* key,
                                        bool allow_zero = true) {
    if (!j.contains(key) || !is_non_negative_integer(j[key])) {
        return err(std::string(path) + "." + key, "must be a non-negative integer");
    }
    if (!allow_zero && j[key].get<uint64_t>() == 0) {
        return err(std::string(path) + "." + key, "must be > 0");
    }
    return std::nullopt;
}

std::optional<std::string> validate_availability(const json& avail) {
    if (!avail.is_object()) {
        return err("availability", "must be an object");
    }
    if (auto e = require_non_empty_string(avail, "availability", "platform_description")) {
        return e;
    }
    if (!avail.contains("metrics") || !avail["metrics"].is_object()) {
        return err("availability.metrics", "must be an object");
    }
    // 契约 sysmon §2.5：固定键集合必须每帧全覆盖（消费方据此渲染，缺键视为坏帧）。
    for (const char* required : SYSMON_METRIC_KEYS) {
        if (!avail["metrics"].contains(required)) {
            return err(std::string("availability.metrics.") + required, "is required");
        }
    }
    for (const auto& [key, entry] : avail["metrics"].items()) {
        const std::string path = "availability.metrics." + key;
        if (!entry.is_object()) {
            return err(path, "must be an object");
        }
        if (!entry.contains("available") || !entry["available"].is_boolean()) {
            return err(path + ".available", "must be a boolean");
        }
        const bool available = entry["available"].get<bool>();
        if (available) {
            if (entry.contains("reason")) {
                return err(path + ".reason", "must be omitted when available=true");
            }
        } else {
            if (!entry.contains("reason") || !entry["reason"].is_string() ||
                entry["reason"].get<std::string>().empty()) {
                return err(path + ".reason", "must be a non-empty string when available=false");
            }
        }
    }
    if (avail.contains("semantic_hints")) {
        if (!avail["semantic_hints"].is_object()) {
            return err("availability.semantic_hints", "must be an object");
        }
        for (const auto& [key, hint] : avail["semantic_hints"].items()) {
            if (!hint.is_string() || hint.get<std::string>().empty()) {
                return err("availability.semantic_hints." + key, "must be a non-empty string");
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> validate_snapshot_system(const json& system) {
    if (!system.is_object()) {
        return err("system", "must be an object");
    }
    if (auto e = require_non_empty_string(system, "system", "hostname")) {
        return e;
    }
    if (system.contains("cpu_model") && !system["cpu_model"].is_string()) {
        return err("system.cpu_model", "must be a string");
    }
    if (!system.contains("logical_cores") || !is_non_negative_integer(system["logical_cores"]) ||
        system["logical_cores"].get<uint64_t>() == 0) {
        return err("system.logical_cores", "must be a positive integer");
    }
    if (auto e = require_uint(system, "system", "boot_time_unix_sec")) {
        return e;
    }
    if (auto e = require_uint(system, "system", "uptime_sec")) {
        return e;
    }
    return std::nullopt;
}

std::optional<std::string> validate_snapshot_cpu(const json& cpu) {
    if (!cpu.is_object()) {
        return err("cpu", "must be an object");
    }
    if (!cpu.contains("total") || !cpu["total"].is_object()) {
        return err("cpu.total", "must be an object");
    }
    if (!cpu["total"].contains("usage_ratio")) {
        return err("cpu.total.usage_ratio", "is required (null when unavailable)");
    }
    if (auto e = validate_optional_ratio(cpu["total"], "cpu.total", "usage_ratio")) {
        return e;
    }
    for (const char* key :
         {"core_frequencies", "load_average", "context_switches_per_sec", "interrupts_per_sec"}) {
        if (!cpu.contains(key)) {
            return err(std::string("cpu.") + key, "is required (null when unavailable)");
        }
    }
    if (!cpu.contains("cores") || !cpu["cores"].is_array()) {
        return err("cpu.cores", "must be an array");
    }
    for (std::size_t i = 0; i < cpu["cores"].size(); ++i) {
        const auto& core = cpu["cores"][i];
        const std::string path = "cpu.cores[" + std::to_string(i) + "]";
        if (!core.is_object()) {
            return err(path, "must be an object");
        }
        if (!core.contains("index") || !is_non_negative_integer(core["index"])) {
            return err(path + ".index", "must be a non-negative integer");
        }
        if (!core.contains("usage_ratio")) {
            return err(path + ".usage_ratio", "is required (null when unavailable)");
        }
        if (auto e = validate_optional_ratio(core, path, "usage_ratio")) {
            return e;
        }
        if (!core.contains("saturated") || !core["saturated"].is_boolean()) {
            return err(path + ".saturated", "must be a boolean");
        }
    }
    if (!cpu.contains("saturated_cores") || !cpu["saturated_cores"].is_array()) {
        return err("cpu.saturated_cores", "must be an array");
    }
    for (const auto& core_index : cpu["saturated_cores"]) {
        if (!is_non_negative_integer(core_index)) {
            return err("cpu.saturated_cores", "elements must be non-negative integers");
        }
    }
    if (cpu.contains("core_frequencies") && !cpu["core_frequencies"].is_null()) {
        const auto& frequencies = cpu["core_frequencies"];
        if (!frequencies.is_array()) {
            return err("cpu.core_frequencies", "must be an array or null");
        }
        for (std::size_t i = 0; i < frequencies.size(); ++i) {
            const auto& item = frequencies[i];
            const std::string path = "cpu.core_frequencies[" + std::to_string(i) + "]";
            if (!item.is_object()) {
                return err(path, "must be an object");
            }
            if (auto e = require_uint(item, path, "index")) {
                return e;
            }
            if (auto e = require_uint(item, path, "mhz", /*allow_zero=*/false)) {
                return e;
            }
        }
    }
    if (cpu.contains("load_average") && !cpu["load_average"].is_null()) {
        const auto& load = cpu["load_average"];
        if (!load.is_object()) {
            return err("cpu.load_average", "must be an object or null");
        }
        for (const char* key : {"one", "five", "fifteen"}) {
            if (!load.contains(key)) {
                return err(std::string("cpu.load_average.") + key,
                           "is required (null when unavailable)");
            }
            if (!load[key].is_null() && !load[key].is_number()) {
                return err(std::string("cpu.load_average.") + key, "must be a number or null");
            }
        }
        if (!load.contains("semantic") || !load["semantic"].is_string() ||
            load["semantic"].get<std::string>().empty()) {
            return err("cpu.load_average.semantic", "must be a non-empty string");
        }
    }
    if (auto e = validate_optional_rate(cpu, "cpu", "context_switches_per_sec")) {
        return e;
    }
    if (auto e = validate_optional_rate(cpu, "cpu", "interrupts_per_sec")) {
        return e;
    }
    return std::nullopt;
}

std::optional<std::string> validate_snapshot_temperature(const json& temperature) {
    if (temperature.is_null()) {
        return std::nullopt;
    }
    if (!temperature.is_array()) {
        return err("temperature", "must be an array or null");
    }
    for (std::size_t i = 0; i < temperature.size(); ++i) {
        const auto& item = temperature[i];
        const std::string path = "temperature[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            return err(path, "must be an object");
        }
        if (auto e = require_non_empty_string(item, path, "sensor")) {
            return e;
        }
        if (!item.contains("celsius") || !is_number(item["celsius"])) {
            return err(path + ".celsius", "must be a number");
        }
    }
    return std::nullopt;
}

std::optional<std::string> validate_snapshot_memory(const json& memory) {
    if (memory.is_null()) {
        return std::nullopt;
    }
    if (!memory.is_object()) {
        return err("memory", "must be an object or null");
    }
    for (const char* key : {"total_bytes", "available_bytes", "used_bytes"}) {
        if (auto e = require_uint(memory, "memory", key)) {
            return e;
        }
    }
    const uint64_t total = memory["total_bytes"].get<uint64_t>();
    const uint64_t available = memory["available_bytes"].get<uint64_t>();
    const uint64_t used = memory["used_bytes"].get<uint64_t>();
    if (available > total || used > total) {
        return err("memory", "used/available bytes must not exceed total bytes");
    }
    if (used != total - available) {
        return err("memory", "used_bytes must equal total_bytes - available_bytes");
    }
    if (auto e = validate_optional_ratio(memory, "memory", "used_ratio")) {
        return e;
    }
    // cached_bytes / buffers_bytes（契约 sysmon §2.4）：缺失或 null 合法（本平台不提供）；
    // 出现时须为非负整数且不大于 total_bytes。
    for (const char* key : {"cached_bytes", "buffers_bytes"}) {
        if (!memory.contains(key) || memory[key].is_null()) {
            continue;
        }
        if (auto e = require_uint(memory, "memory", key)) {
            return e;
        }
        if (memory[key].get<uint64_t>() > total) {
            return err(std::string("memory.") + key, "must not exceed total_bytes");
        }
    }
    return std::nullopt;
}

std::optional<std::string> validate_snapshot_swap(const json& swap) {
    if (swap.is_null()) {
        return std::nullopt;
    }
    if (!swap.is_object()) {
        return err("swap", "must be an object or null");
    }
    if (auto e = require_uint(swap, "swap", "total_bytes")) {
        return e;
    }
    if (auto e = require_uint(swap, "swap", "used_bytes")) {
        return e;
    }
    if (swap["used_bytes"].get<uint64_t>() > swap["total_bytes"].get<uint64_t>()) {
        return err("swap.used_bytes", "must not exceed total_bytes");
    }
    return validate_optional_ratio(swap, "swap", "used_ratio");
}

std::optional<std::string> validate_snapshot_volumes(const json& volumes) {
    if (volumes.is_null()) {
        return std::nullopt;
    }
    if (!volumes.is_array()) {
        return err("volumes", "must be an array or null");
    }
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        const auto& volume = volumes[i];
        const std::string path = "volumes[" + std::to_string(i) + "]";
        if (!volume.is_object()) {
            return err(path, "must be an object");
        }
        if (auto e = require_non_empty_string(volume, path, "mount")) {
            return e;
        }
        if (auto e = require_uint(volume, path, "total_bytes")) {
            return e;
        }
        if (auto e = require_uint(volume, path, "used_bytes")) {
            return e;
        }
        if (volume["used_bytes"].get<uint64_t>() > volume["total_bytes"].get<uint64_t>()) {
            return err(path + ".used_bytes", "must not exceed total_bytes");
        }
        if (auto e = validate_optional_ratio(volume, path, "used_ratio")) {
            return e;
        }
    }
    return std::nullopt;
}

std::optional<std::string> validate_snapshot_disk_io(const json& disk_io) {
    if (disk_io.is_null()) {
        return std::nullopt;
    }
    if (!disk_io.is_array()) {
        return err("disk_io", "must be an array or null");
    }
    for (std::size_t i = 0; i < disk_io.size(); ++i) {
        const auto& item = disk_io[i];
        const std::string path = "disk_io[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            return err(path, "must be an object");
        }
        if (auto e = require_non_empty_string(item, path, "device")) {
            return e;
        }
        for (const char* key : {"read_bytes_per_sec", "write_bytes_per_sec", "read_iops",
                                "write_iops", "latency_ms", "queue_depth", "utilization"}) {
            if (auto e = validate_optional_rate(item, path, key)) {
                return e;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> validate_snapshot_network(const json& network) {
    if (network.is_null()) {
        return std::nullopt;
    }
    if (!network.is_array()) {
        return err("network", "must be an array or null");
    }
    for (std::size_t i = 0; i < network.size(); ++i) {
        const auto& item = network[i];
        const std::string path = "network[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            return err(path, "must be an object");
        }
        if (auto e = require_non_empty_string(item, path, "interface")) {
            return e;
        }
        for (const char* key : {"rx_bytes_total", "tx_bytes_total"}) {
            if (auto e = require_uint(item, path, key)) {
                return e;
            }
        }
        for (const char* key : {"rx_bytes_per_sec", "tx_bytes_per_sec", "rx_errors_per_sec",
                                "tx_errors_per_sec", "rx_drops_per_sec", "tx_drops_per_sec"}) {
            if (auto e = validate_optional_rate(item, path, key)) {
                return e;
            }
        }
        if (item.contains("is_virtual") && !item["is_virtual"].is_boolean()) {
            return err(path + ".is_virtual", "must be a boolean");
        }
    }
    return std::nullopt;
}

std::optional<std::string> validate_snapshot_shm_dir(const json& shm_dir) {
    if (shm_dir.is_null()) {
        return std::nullopt;
    }
    if (!shm_dir.is_object()) {
        return err("shm_dir", "must be an object or null");
    }
    if (auto e = require_non_empty_string(shm_dir, "shm_dir", "path")) {
        return e;
    }
    if (auto e = require_uint(shm_dir, "shm_dir", "total_bytes")) {
        return e;
    }
    if (auto e = require_uint(shm_dir, "shm_dir", "page_files")) {
        return e;
    }
    if (shm_dir.contains("oldest_file_age_sec") && !shm_dir["oldest_file_age_sec"].is_null()) {
        if (!shm_dir["oldest_file_age_sec"].is_number_integer() ||
            shm_dir["oldest_file_age_sec"].get<int64_t>() < 0) {
            return err("shm_dir.oldest_file_age_sec", "must be a non-negative integer or null");
        }
    }
    return std::nullopt;
}

// ===== 配置校验辅助 =====

std::optional<std::string> validate_config_sampling(const json& sampling) {
    if (!sampling.is_object()) {
        return err("sampling", "must be an object");
    }
    if (!sampling.contains("measure_interval_ms") ||
        !int_in_range(sampling["measure_interval_ms"], 100, 60000)) {
        return err("sampling.measure_interval_ms", "must be an integer within [100, 60000]");
    }
    if (!sampling.contains("record_interval_sec") ||
        !int_in_range(sampling["record_interval_sec"], 1, 86400)) {
        return err("sampling.record_interval_sec", "must be an integer within [1, 86400]");
    }
    if (!sampling.contains("phase_offset_sec") ||
        !int_in_range(sampling["phase_offset_sec"], 0, 86400)) {
        return err("sampling.phase_offset_sec", "must be an integer within [0, 86400]");
    }
    if (!sampling.contains("groups") || !sampling["groups"].is_object()) {
        return err("sampling.groups", "must be an object");
    }
    for (const auto& [group, enabled] : sampling["groups"].items()) {
        (void)group;
        if (!enabled.is_boolean()) {
            return err("sampling.groups." + group, "must be a boolean");
        }
    }
    return std::nullopt;
}

std::optional<std::string> validate_config_disk(const json& disk) {
    if (!disk.is_object()) {
        return err("disk", "must be an object");
    }
    for (const char* key : {"watch_paths", "devices"}) {
        if (!disk.contains(key) || !disk[key].is_array()) {
            return err(std::string("disk.") + key, "must be an array");
        }
        for (const auto& item : disk[key]) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                return err(std::string("disk.") + key, "elements must be non-empty strings");
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> validate_config_network(const json& network) {
    if (!network.is_object()) {
        return err("network", "must be an object");
    }
    if (!network.contains("hide_virtual") || !network["hide_virtual"].is_boolean()) {
        return err("network.hide_virtual", "must be a boolean");
    }
    return std::nullopt;
}

std::optional<std::string> validate_config_history(const json& history) {
    if (!history.is_object()) {
        return err("history", "must be an object");
    }
    if (!history.contains("enabled") || !history["enabled"].is_boolean()) {
        return err("history.enabled", "must be a boolean");
    }
    if (!history.contains("raw_retention_hours") ||
        !int_in_range(history["raw_retention_hours"], 1, 720)) {
        return err("history.raw_retention_hours", "must be an integer within [1, 720]");
    }
    if (!history.contains("max_retention_days") ||
        !int_in_range(history["max_retention_days"], 1, 3650)) {
        return err("history.max_retention_days", "must be an integer within [1, 3650]");
    }
    if (!history.contains("max_bytes") || !is_non_negative_integer(history["max_bytes"])) {
        return err("history.max_bytes", "must be a non-negative integer");
    }
    return std::nullopt;
}

std::optional<std::string> validate_config_realtime(const json& realtime) {
    if (!realtime.is_object()) {
        return err("realtime", "must be an object");
    }
    if (!realtime.contains("push_interval_sec") ||
        !int_in_range(realtime["push_interval_sec"], 1, 3600)) {
        return err("realtime.push_interval_sec", "must be an integer within [1, 3600]");
    }
    return std::nullopt;
}

std::optional<std::string> validate_config_alerts(const json& alerts) {
    if (!alerts.is_object()) {
        return err("alerts", "must be an object");
    }
    for (const char* key :
         {"cpu_sustain_used_pct", "memory_used_pct", "swap_used_pct", "volume_free_pct"}) {
        if (!alerts.contains(key) || !alerts[key].is_number() ||
            !in_range(alerts[key].get<double>(), 0.0, 100.0)) {
            return err(std::string("alerts.") + key, "must be a number within [0, 100]");
        }
    }
    if (!alerts.contains("cpu_sustain_samples") ||
        !int_in_range(alerts["cpu_sustain_samples"], 1, 3600)) {
        return err("alerts.cpu_sustain_samples", "must be an integer within [1, 3600]");
    }
    if (!alerts.contains("volume_free_bytes") ||
        !is_non_negative_integer(alerts["volume_free_bytes"])) {
        return err("alerts.volume_free_bytes", "must be a non-negative integer");
    }
    if (!alerts.contains("cooldown_sec") || !int_in_range(alerts["cooldown_sec"], 0, 86400)) {
        return err("alerts.cooldown_sec", "must be an integer within [0, 86400]");
    }
    return std::nullopt;
}

}  // namespace

// ===== SysmonSnapshot =====

std::optional<std::string> SysmonSnapshot::validate(const nlohmann::json& payload) {
    if (!payload.is_object()) {
        return err("snapshot", "must be an object");
    }
    if (!payload.contains("source") || !payload["source"].is_string() ||
        payload["source"].get<std::string>() != "dzsysmon") {
        return err("snapshot.source", "must be the fixed sender dzsysmon");
    }
    if (!payload.contains("timestamp") || !payload["timestamp"].is_number_integer() ||
        payload["timestamp"].get<int64_t>() < 0) {
        return err("snapshot.timestamp", "must be a non-negative integer");
    }
    if (!payload.contains("sampling") || !payload["sampling"].is_object()) {
        return err("snapshot.sampling", "must be an object");
    }
    {
        const auto& sampling = payload["sampling"];
        if (!sampling.contains("measure_interval_ms") ||
            !int_in_range(sampling["measure_interval_ms"], 1, 60000)) {
            return err("snapshot.sampling.measure_interval_ms",
                       "must be an integer within [1, 60000]");
        }
        if (auto e = require_uint(sampling, "snapshot.sampling", "last_sample_unix_ms")) {
            return e;
        }
        if (auto e = require_uint(sampling, "snapshot.sampling", "missed_ticks")) {
            return e;
        }
    }
    if (!payload.contains("platform") || !payload["platform"].is_object()) {
        return err("snapshot.platform", "must be an object");
    }
    {
        const auto& platform = payload["platform"];
        if (auto e = require_non_empty_string(platform, "snapshot.platform", "family")) {
            return e;
        }
        if (auto e = require_non_empty_string(platform, "snapshot.platform", "version")) {
            return e;
        }
        if (!platform.contains("kernel") || !platform["kernel"].is_string()) {
            return err("snapshot.platform.kernel", "must be a string");
        }
        if (!platform.contains("arch") || !platform["arch"].is_string()) {
            return err("snapshot.platform.arch", "must be a string");
        }
        if (auto e = require_uint(platform, "snapshot.platform", "build")) {
            return e;
        }
    }
    if (!payload.contains("system")) {
        return err("snapshot.system", "is required");
    }
    if (auto e = validate_snapshot_system(payload["system"])) {
        return e;
    }
    if (!payload.contains("cpu")) {
        return err("snapshot.cpu", "is required");
    }
    if (auto e = validate_snapshot_cpu(payload["cpu"])) {
        return e;
    }
    const auto check_optional = [&](const char* key) -> std::optional<std::string> {
        if (payload.contains(key)) {
            return std::nullopt;
        }
        return err(std::string("snapshot.") + key, "is required (null when unavailable)");
    };
    for (const char* key :
         {"temperature", "memory", "swap", "volumes", "disk_io", "network", "shm_dir"}) {
        if (auto e = check_optional(key)) {
            return e;
        }
    }
    if (auto e = validate_snapshot_temperature(payload["temperature"])) {
        return e;
    }
    if (auto e = validate_snapshot_memory(payload["memory"])) {
        return e;
    }
    if (auto e = validate_snapshot_swap(payload["swap"])) {
        return e;
    }
    if (auto e = validate_snapshot_volumes(payload["volumes"])) {
        return e;
    }
    if (auto e = validate_snapshot_disk_io(payload["disk_io"])) {
        return e;
    }
    if (auto e = validate_snapshot_network(payload["network"])) {
        return e;
    }
    if (auto e = validate_snapshot_shm_dir(payload["shm_dir"])) {
        return e;
    }
    if (!payload.contains("availability")) {
        return err("snapshot.availability", "is required");
    }
    if (auto e = validate_availability(payload["availability"])) {
        return e;
    }
    // 契约 sysmon §2.5：Windows 平台描述必须含 build 号（Win10/11 都报 NT 10.0）。
    if (payload["platform"].value("family", "") == "windows") {
        const auto build = payload["platform"].value("build", 0);
        const auto description = payload["availability"].value("platform_description", "");
        if (description.find(std::to_string(build)) == std::string::npos) {
            return err("availability.platform_description",
                       "must include the windows build number");
        }
    }
    return std::nullopt;
}

// ===== SysmonConfig =====

std::map<std::string, bool> SysmonSamplingConfig::default_groups() {
    // B 组（温度 / 每核频率）默认值按平台定（契约 sysmon §3.1）：Linux hwmon 与 cpufreq
    // 普遍可用，默认开启；Windows 无可靠来源，默认关闭（用户可显式开启，采集器仍报
    // 不可用，契约 sysmon §2.3/§2.4）。
#if defined(_WIN32)
    constexpr bool kTemperatureDefault = false;
    constexpr bool kCpuFrequencyDefault = false;
#elif defined(__linux__)
    constexpr bool kTemperatureDefault = true;
    constexpr bool kCpuFrequencyDefault = true;
#else
#error "unsupported platform: sysmon B-group defaults require _WIN32 or __linux__"
#endif
    return {
        {"cpu", true},
        {"memory", true},
        {"swap", true},
        {"volumes", true},
        {"disk_io", true},
        {"network", true},
        {SYSMON_GROUP_TEMPERATURE, kTemperatureDefault},
        {SYSMON_GROUP_CPU_FREQUENCY, kCpuFrequencyDefault},
    };
}

SysmonConfig SysmonConfig::default_config() {
    SysmonConfig cfg;
    cfg.sampling.groups = SysmonSamplingConfig::default_groups();
    return cfg;
}

std::optional<std::string> SysmonConfig::validate(const nlohmann::json& cfg) {
    if (!cfg.is_object()) {
        return err("config", "must be an object");
    }
    if (contains_null(cfg)) {
        return err("config", "must not contain null");
    }
    for (const char* section : SYSMON_CONFIG_SECTIONS) {
        if (!cfg.contains(section)) {
            return err(std::string("config.") + section, "is required");
        }
    }
    if (auto e = validate_config_sampling(cfg["sampling"])) {
        return e;
    }
    if (auto e = validate_config_disk(cfg["disk"])) {
        return e;
    }
    if (auto e = validate_config_network(cfg["network"])) {
        return e;
    }
    if (auto e = validate_config_history(cfg["history"])) {
        return e;
    }
    if (auto e = validate_config_realtime(cfg["realtime"])) {
        return e;
    }
    return validate_config_alerts(cfg["alerts"]);
}

nlohmann::json SysmonConfig::apply_patch(const nlohmann::json& current,
                                         const nlohmann::json& patch) {
    if (!patch.is_object()) {
        throw std::runtime_error("sysmon config patch must be an object");
    }
    if (contains_null(patch)) {
        throw std::runtime_error("sysmon config patch must not contain null");
    }
    nlohmann::json merged = current;
    merged.merge_patch(patch);
    if (auto e = validate(merged)) {
        throw std::runtime_error("sysmon config invalid | " + *e);
    }
    return merged;
}

}  // namespace dztrader::platform
