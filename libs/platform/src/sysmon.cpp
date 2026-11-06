#include <dztrader/platform/sysmon.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
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
    std::set<std::string> sensors;
    for (std::size_t i = 0; i < temperature.size(); ++i) {
        const auto& item = temperature[i];
        const std::string path = "temperature[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            return err(path, "must be an object");
        }
        if (auto e = require_non_empty_string(item, path, "sensor")) {
            return e;
        }
        // 契约 sysmon §2.4/§7：sensor 是历史库维度键，帧内必须唯一（与 volumes/disk_io/network
        // 的标识唯一性校验同一口径）；病态驱动的重复 label 由采集层追加 `-<n>` 兜底。
        if (!sensors.insert(item["sensor"].get<std::string>()).second) {
            return err(path + ".sensor", "must be unique within the frame");
        }
        // chip / label 是 additive 可选字段（#126）：存在时 chip 非空、label 允许空串；
        // 缺失合法（旧版本发送方不带这两个字段）。发送方义务见契约 §2.4。
        if (item.contains("chip")) {
            if (auto e = require_non_empty_string(item, path, "chip")) {
                return e;
            }
        }
        if (item.contains("label") && !item["label"].is_string()) {
            return err(path + ".label", "must be a string");
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

/// 校验 `volumes[]`（契约 sysmon §2.4、§7）。
///
/// `available=false` 时三个容量字段**必须**为 null 且 `reason` 非空——这是"读不到不是 0"
/// 在协议层的强制；`available=true` 时容量字段必须为整数且不得出现 `reason`。
std::optional<std::string> validate_snapshot_volumes(const json& volumes) {
    if (volumes.is_null()) {
        return std::nullopt;
    }
    if (!volumes.is_array()) {
        return err("volumes", "must be an array or null");
    }
    std::set<std::string> mounts;
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        const auto& volume = volumes[i];
        const std::string path = "volumes[" + std::to_string(i) + "]";
        if (!volume.is_object()) {
            return err(path, "must be an object");
        }
        if (auto e = require_non_empty_string(volume, path, "mount")) {
            return e;
        }
        if (!mounts.insert(volume["mount"].get<std::string>()).second) {
            return err(path + ".mount", "must be unique within the frame");
        }
        if (!volume.contains("available") || !volume["available"].is_boolean()) {
            return err(path + ".available", "must be a boolean");
        }
        const bool available = volume["available"].get<bool>();
        const auto require_null = [&volume, &path](const char* key) -> std::optional<std::string> {
            if (!volume.contains(key) || !volume[key].is_null()) {
                return err(path + "." + key, "must be null when available=false");
            }
            return std::nullopt;
        };
        if (!available) {
            if (auto e = require_non_empty_string(volume, path, "reason")) {
                return e;
            }
            for (const char* key : {"total_bytes", "used_bytes", "used_ratio"}) {
                if (auto e = require_null(key)) {
                    return e;
                }
            }
            continue;
        }
        if (volume.contains("reason")) {
            return err(path + ".reason", "must be omitted when available=true");
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
    std::set<std::string> devices;
    for (std::size_t i = 0; i < disk_io.size(); ++i) {
        const auto& item = disk_io[i];
        const std::string path = "disk_io[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            return err(path, "must be an object");
        }
        if (auto e = require_non_empty_string(item, path, "device")) {
            return e;
        }
        if (!devices.insert(item["device"].get<std::string>()).second) {
            return err(path + ".device", "must be unique within the frame");
        }
        for (const char* key : {"read_bytes_per_sec", "write_bytes_per_sec", "read_iops",
                                "write_iops", "latency_ms", "queue_depth"}) {
            if (auto e = validate_optional_rate(item, path, key)) {
                return e;
            }
        }
        // utilization 是比率：null 或 [0,1]（契约 sysmon §2.4、§7）。
        if (auto e = validate_optional_ratio(item, path, "utilization")) {
            return e;
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
    std::set<std::string> interfaces;
    for (std::size_t i = 0; i < network.size(); ++i) {
        const auto& item = network[i];
        const std::string path = "network[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            return err(path, "must be an object");
        }
        if (auto e = require_non_empty_string(item, path, "interface")) {
            return e;
        }
        if (!interfaces.insert(item["interface"].get<std::string>()).second) {
            return err(path + ".interface", "must be unique within the frame");
        }
        // 累计计数器没有"不可用"语义：八个 *_total 必须存在且为非负整数（契约 sysmon §7）。
        for (const char* key :
             {"rx_bytes_total", "tx_bytes_total", "rx_packets_total", "tx_packets_total",
              "rx_errors_total", "tx_errors_total", "rx_drops_total", "tx_drops_total"}) {
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
        if (!item.contains("is_virtual") || !item["is_virtual"].is_boolean()) {
            return err(path + ".is_virtual", "must be a boolean");
        }
    }
    return std::nullopt;
}

/// `servers[].error` 的合法取值（契约 sysmon §2.9「失败原因」）。
const std::set<std::string>& clock_server_error_codes() {
    static const std::set<std::string> codes = {"timeout", "kod_rate", "kod_deny", "bad_echo",
                                                "unreachable"};
    return codes;
}

/// 校验 `clock` 域（契约 sysmon §2.9、§7）。
///
/// 空态（尚未探测）与失败态都是合法形状；三条硬约束：
/// 1. `offset_ms` / `bound_ms` / `source_stratum` 同生共死，且 `source` 为空串当且仅当三者全 null；
/// 2. `servers[]` 的 `ok` 二态决定其余字段可空性（成功样本必有四项测量、失败样本必无）；
/// 3. `daemon_conflict` 必须等于「running=true 的不同服务数 >= 2」。
///
/// `clock = null` 只表示该域被配置关闭（§2.9）——校验层看不到配置，因此接受 null；
/// 形状规则的其余部分与非 null 时一致。
std::optional<std::string> validate_snapshot_clock(const json& clock) {
    if (clock.is_null()) {
        return std::nullopt;
    }
    if (!clock.is_object()) {
        return err("clock", "must be an object or null");
    }
    // 四个测量/辅助字段允许 null（不可用语义见 §2.9），其余字段不可 null 且必须存在。
    for (const char* key : {"offset_ms", "bound_ms", "source_stratum", "kernel_synced"}) {
        if (!clock.contains(key)) {
            return err(std::string("clock.") + key, "is required (null when unavailable)");
        }
    }
    for (const char* key :
         {"source", "last_probe_unix_ms", "servers", "daemons", "daemon_conflict"}) {
        if (!clock.contains(key)) {
            return err(std::string("clock.") + key, "is required");
        }
    }
    const bool has_offset = !clock["offset_ms"].is_null();
    const bool has_bound = !clock["bound_ms"].is_null();
    const bool has_stratum = !clock["source_stratum"].is_null();
    if (has_offset != has_bound || has_offset != has_stratum) {
        return err("clock",
                   "offset_ms, bound_ms and source_stratum must be null or non-null together");
    }
    if (!clock["source"].is_string()) {
        return err("clock.source", "must be a string");
    }
    const bool source_empty = clock["source"].get<std::string>().empty();
    if (source_empty == has_offset) {
        return err("clock.source",
                   "must be an empty string if and only if offset_ms/bound_ms/source_stratum "
                   "are null");
    }
    if (has_offset) {
        if (!is_number(clock["offset_ms"])) {
            return err("clock.offset_ms", "must be a number or null");
        }
        if (!is_number(clock["bound_ms"])) {
            return err("clock.bound_ms", "must be a number or null");
        }
        if (clock["bound_ms"].get<double>() < 0.0) {
            return err("clock.bound_ms", "must be >= 0");
        }
        if (!int_in_range(clock["source_stratum"], 1, 15)) {
            return err("clock.source_stratum", "must be an integer within [1, 15] or null");
        }
    }
    if (!is_non_negative_integer(clock["last_probe_unix_ms"])) {
        return err("clock.last_probe_unix_ms", "must be a non-negative integer");
    }
    if (!clock["kernel_synced"].is_null() && !clock["kernel_synced"].is_boolean()) {
        return err("clock.kernel_synced", "must be a boolean or null");
    }
    if (!clock["servers"].is_array()) {
        return err("clock.servers", "must be an array");
    }
    std::set<std::string> hosts;
    for (std::size_t i = 0; i < clock["servers"].size(); ++i) {
        const auto& server = clock["servers"][i];
        const std::string path = "clock.servers[" + std::to_string(i) + "]";
        if (!server.is_object()) {
            return err(path, "must be an object");
        }
        if (auto e = require_non_empty_string(server, path, "host")) {
            return e;
        }
        if (!hosts.insert(server["host"].get<std::string>()).second) {
            return err(path + ".host", "must be unique within the frame");
        }
        if (!server.contains("ok") || !server["ok"].is_boolean()) {
            return err(path + ".ok", "must be a boolean");
        }
        for (const char* key : {"offset_ms", "bound_ms", "rtt_ms", "stratum", "error"}) {
            if (!server.contains(key)) {
                return err(path + "." + key, "is required (null when unavailable)");
            }
        }
        if (server["ok"].get<bool>()) {
            if (!server["error"].is_null()) {
                return err(path + ".error", "must be null when ok=true");
            }
            for (const char* key : {"offset_ms", "bound_ms", "rtt_ms"}) {
                if (!is_number(server[key])) {
                    return err(path + "." + key, "must be a number when ok=true");
                }
            }
            if (server["rtt_ms"].get<double>() < 0.0) {
                return err(path + ".rtt_ms", "must be >= 0");
            }
            if (!int_in_range(server["stratum"], 1, 15)) {
                return err(path + ".stratum", "must be an integer within [1, 15] when ok=true");
            }
        } else {
            // 失败样本不产出任何测量值（契约 sysmon §2.9/§7）。
            if (!server["error"].is_string() ||
                !clock_server_error_codes().contains(server["error"].get<std::string>())) {
                return err(path + ".error",
                           "must be one of timeout, kod_rate, kod_deny, bad_echo, unreachable "
                           "when ok=false");
            }
            for (const char* key : {"offset_ms", "bound_ms", "rtt_ms", "stratum"}) {
                if (!server[key].is_null()) {
                    return err(path + "." + key, "must be null when ok=false");
                }
            }
        }
    }
    if (!clock["daemons"].is_array()) {
        return err("clock.daemons", "must be an array");
    }
    std::set<std::string> daemon_names;
    std::size_t running_count = 0;
    for (std::size_t i = 0; i < clock["daemons"].size(); ++i) {
        const auto& daemon = clock["daemons"][i];
        const std::string path = "clock.daemons[" + std::to_string(i) + "]";
        if (!daemon.is_object()) {
            return err(path, "must be an object");
        }
        if (auto e = require_non_empty_string(daemon, path, "name")) {
            return e;
        }
        if (!daemon_names.insert(daemon["name"].get<std::string>()).second) {
            return err(path + ".name", "must be unique within the frame");
        }
        if (!daemon.contains("running") || !daemon["running"].is_boolean()) {
            return err(path + ".running", "must be a boolean");
        }
        if (daemon["running"].get<bool>()) {
            ++running_count;
        }
    }
    if (!clock["daemon_conflict"].is_boolean()) {
        return err("clock.daemon_conflict", "must be a boolean");
    }
    if (clock["daemon_conflict"].get<bool>() != (running_count >= 2)) {
        return err("clock.daemon_conflict",
                   "must equal whether at least two different services are running");
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
    if (!disk.contains("devices") || !disk["devices"].is_array()) {
        return err("disk.devices", "must be an array");
    }
    for (const auto& item : disk["devices"]) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            return err("disk.devices", "elements must be non-empty strings");
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

/// 阈值段校验（契约 sysmon §3.1）。键集合已随 ADR 0034 收敛：没有 `volume_free_bytes`
/// （绝对容量形态），也没有 `cooldown_sec`（改由 `log_repeat_sec` 承担重记节奏）。
std::optional<std::string> validate_config_thresholds(const json& thresholds) {
    if (!thresholds.is_object()) {
        return err("thresholds", "must be an object");
    }
    for (const char* key :
         {"cpu_sustain_used_pct", "memory_used_pct", "swap_used_pct", "volume_free_pct"}) {
        if (!thresholds.contains(key) || !thresholds[key].is_number() ||
            !in_range(thresholds[key].get<double>(), 0.0, 100.0)) {
            return err(std::string("thresholds.") + key, "must be a number within [0, 100]");
        }
    }
    if (!thresholds.contains("cpu_sustain_samples") ||
        !int_in_range(thresholds["cpu_sustain_samples"], 1, 3600)) {
        return err("thresholds.cpu_sustain_samples", "must be an integer within [1, 3600]");
    }
    if (!thresholds.contains("log_repeat_sec") ||
        !int_in_range(thresholds["log_repeat_sec"], 0, 86400)) {
        return err("thresholds.log_repeat_sec", "must be an integer within [0, 86400]");
    }
    return std::nullopt;
}

/// 单个 `probe_times` 元素是否合法 `HH:MM`（两位小时 + 两位分钟，00:00–23:59）。
///
/// **不收** `8:30` 这类单数字小时：契约 §3.1 明文要求「两位小时 + 两位分钟」。宽进会让
/// 同一个时刻出现两种写法，配置差异比较（前端 diff、运维比对）随之失真。
bool is_valid_probe_time(const std::string& text) {
    if (text.size() != 5 || text[2] != ':') {
        return false;
    }
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (!digit(text[0]) || !digit(text[1]) || !digit(text[3]) || !digit(text[4])) {
        return false;
    }
    const int hour = (text[0] - '0') * 10 + (text[1] - '0');
    const int minute = (text[3] - '0') * 10 + (text[4] - '0');
    return hour <= 23 && minute <= 59;
}

/// `clock` 段校验（契约 sysmon §3.1 `clock`；#296）。
///
/// 两条**跨字段**规则是本段校验的重点，单字段范围检查兜不住：
/// - `offset_error_ms > offset_warn_ms`：反了就会让 warning 档永远不出现（或 error 永远先命中），
///   两个档位同时存在却互斥，是静默的语义崩塌。
/// - `servers` 非空：空数组会让「尚未探测」与「无服务器可探」在快照里不可区分（§3.1 明文）。
std::optional<std::string> validate_config_clock(const json& clock) {
    if (!clock.is_object()) {
        return err("clock", "must be an object");
    }
    if (!clock.contains("enabled") || !clock["enabled"].is_boolean()) {
        return err("clock.enabled", "must be a boolean");
    }
    if (!clock.contains("servers") || !clock["servers"].is_array()) {
        return err("clock.servers", "must be an array");
    }
    if (clock["servers"].empty()) {
        return err("clock.servers", "must not be empty");
    }
    for (const auto& item : clock["servers"]) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            return err("clock.servers", "elements must be non-empty strings");
        }
    }
    if (!clock.contains("probe_times") || !clock["probe_times"].is_array()) {
        return err("clock.probe_times", "must be an array");
    }
    for (const auto& item : clock["probe_times"]) {
        if (!item.is_string() || !is_valid_probe_time(item.get<std::string>())) {
            return err("clock.probe_times", "elements must be valid HH:MM within 00:00-23:59");
        }
    }
    if (!clock.contains("probe_timeout_ms") ||
        !int_in_range(clock["probe_timeout_ms"], 100, 10000)) {
        return err("clock.probe_timeout_ms", "must be an integer within [100, 10000]");
    }
    if (!clock.contains("manual_min_interval_sec") ||
        !int_in_range(clock["manual_min_interval_sec"], 0, 3600)) {
        return err("clock.manual_min_interval_sec", "must be an integer within [0, 3600]");
    }
    if (!clock.contains("offset_warn_ms") || !int_in_range(clock["offset_warn_ms"], 1, 60000)) {
        return err("clock.offset_warn_ms", "must be an integer within [1, 60000]");
    }
    // error 档上界是 600000；下界由跨字段规则（必须严格大于 warn）决定，不单列。
    if (!clock.contains("offset_error_ms") || !int_in_range(clock["offset_error_ms"], 1, 600000)) {
        return err("clock.offset_error_ms", "must be an integer within [1, 600000]");
    }
    if (!clock.contains("bound_alarm_ms") || !int_in_range(clock["bound_alarm_ms"], 1, 60000)) {
        return err("clock.bound_alarm_ms", "must be an integer within [1, 60000]");
    }
    if (clock["offset_error_ms"].get<int64_t>() <= clock["offset_warn_ms"].get<int64_t>()) {
        return err("clock.offset_error_ms", "must be greater than offset_warn_ms");
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
        payload["source"].get<std::string>() != SYSMON_INSTANCE_NAME) {
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
         {"temperature", "memory", "swap", "volumes", "disk_io", "network", "shm_dir", "clock"}) {
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
    if (auto e = validate_snapshot_clock(payload["clock"])) {
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
        // 时钟偏差监控默认开（契约 sysmon §3.1；#296）：与 `clock.enabled` 同为两层入口，
        // 两者同时为 true 才启用（关闭取或）。
        {SYSMON_GROUP_CLOCK, true},
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
    if (auto e = validate_config_thresholds(cfg["thresholds"])) {
        return e;
    }
    return validate_config_clock(cfg["clock"]);
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
