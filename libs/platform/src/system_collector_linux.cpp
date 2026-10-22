/**
 * @file system_collector_linux.cpp
 * @brief Linux 系统指标采集实现（契约 sysmon）。
 *
 * 仅当编译目标为 Linux 时编译；其他平台产生空翻译单元（Windows 实现见
 * system_collector_windows.cpp，工厂函数每平台只定义一次）。
 *
 * 规格与算法参考 psutil（BSD-3-Clause）：仅借鉴 OS API 选择、边界处理语义与测试不变量，
 * 不翻译其代码、不引入依赖；归属声明见仓库根 THIRD_PARTY_NOTICES.md。
 *
 * 采集失败一律降级：系统信息返回空串/0，CPU 计数器/温度/每核频率返回 std::nullopt，
 * 不抛异常（契约 sysmon §2.7：采集失败跳过本次计量由上层记缺口）。
 */

#if defined(__linux__)

#include "system_collector_internal.h"
#include "system_collector_linux_internal.h"

#include <dztrader/platform/system_collector.h>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace dztrader::platform {
namespace {

/// 去掉字符串两端的 ASCII 空白（含 '\r'，容忍 CRLF 文本）。
std::string_view trim(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\r\n\v\f";
    const auto begin = text.find_first_not_of(kWhitespace);
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(kWhitespace);
    return text.substr(begin, end - begin + 1);
}

/// 解析无符号十进制整数；空串、负号、非数字、溢出均返回 std::nullopt。
std::optional<uint64_t> parse_u64(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    uint64_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || ptr != last) {
        return std::nullopt;
    }
    return value;
}

/// 解析有符号十进制整数（sysfs 温度可能为负）；空串、非数字、溢出均返回 std::nullopt。
std::optional<int64_t> parse_i64(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    int64_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || ptr != last) {
        return std::nullopt;
    }
    return value;
}

/// 逐行读取文本文件；文件不可打开时返回空列表（失败降级，不抛异常）。
std::vector<std::string> read_lines(const char* path) {
    std::vector<std::string> lines;
    std::ifstream input(path);
    if (!input) {
        return lines;
    }
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(std::move(line));
    }
    return lines;
}

/// 读取 sysfs 文本文件的首个非空行并去掉两端空白；不可读或内容为空返回空串。
std::string read_sysfs_line(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }
    std::string line;
    while (std::getline(input, line)) {
        const auto value = trim(line);
        if (!value.empty()) {
            return std::string(value);
        }
    }
    return {};
}

/// 列出目录项（error_code 重载，不抛异常）；目录不可读时返回空列表。
std::vector<std::filesystem::directory_entry> list_directory(const std::filesystem::path& path) {
    std::vector<std::filesystem::directory_entry> entries;
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        entries.push_back(*it);
        it.increment(ec);
    }
    return entries;
}

/// 读取 /etc/os-release 为 key -> value（去掉值两侧的 shell 引号）。
std::map<std::string, std::string> read_os_release() {
    std::map<std::string, std::string> values;
    for (const auto& raw : read_lines("/etc/os-release")) {
        const auto line = trim(raw);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const auto key = line.substr(0, equals);
        auto value = line.substr(equals + 1);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        values.emplace(std::string(key), std::string(value));
    }
    return values;
}

/// 取 os-release 中指定键的值；缺失时返回空串。
std::string os_release_value(const std::map<std::string, std::string>& values, const char* key) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return {};
    }
    return it->second;
}

/// 人类可读发行版版本：优先 PRETTY_NAME，缺失时回退 NAME + VERSION_ID。
std::string read_os_version() {
    const auto values = read_os_release();
    auto pretty_name = os_release_value(values, "PRETTY_NAME");
    if (!pretty_name.empty()) {
        return pretty_name;
    }
    const auto name = os_release_value(values, "NAME");
    const auto version_id = os_release_value(values, "VERSION_ID");
    if (!name.empty() && !version_id.empty()) {
        return name + " " + version_id;
    }
    return name.empty() ? version_id : name;
}

/// 读取 /proc/cpuinfo 的 CPU 型号（首个非空的 "model name" 值；取不到返回空串）。
std::string read_cpu_model() {
    for (const auto& raw : read_lines("/proc/cpuinfo")) {
        const auto line = trim(raw);
        if (!line.starts_with("model name")) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        auto value = std::string(trim(line.substr(colon + 1)));
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

/// 逻辑核数（在线 CPU）：优先 sysconf，失败回退数 /proc/cpuinfo 的 processor 行。
uint32_t read_logical_cores() {
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0) {
        return static_cast<uint32_t>(online);
    }
    uint32_t count = 0;
    for (const auto& raw : read_lines("/proc/cpuinfo")) {
        if (trim(raw).starts_with("processor")) {
            ++count;
        }
    }
    return count;
}

/// 读取 /proc/stat 的 btime（Unix 秒）；缺失或解析失败返回 std::nullopt。
std::optional<int64_t> read_boot_time_unix_sec() {
    constexpr std::string_view kKey = "btime ";
    for (const auto& raw : read_lines("/proc/stat")) {
        const auto line = trim(raw);
        if (!line.starts_with(kKey)) {
            continue;
        }
        const auto value = parse_u64(trim(line.substr(kKey.size())));
        if (!value || *value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<int64_t>(*value);
    }
    return std::nullopt;
}

/// 解析 /proc/stat 单个 CPU 行（去掉 "cpu"/"cpuN" 前缀后）的累计计数器。
///
/// 字段顺序：user nice system idle iowait irq softirq steal [guest guest_nice]。
/// total = 前 8 个字段之和（不含 guest/guest_nice：二者已包含在 user/nice 中）；
/// idle = idle + iowait（iowait 不计入忙，契约 sysmon §2.7）。字段不足 4 个或
/// 不可解析时返回 std::nullopt。
std::optional<RawCpuCoreTimes> parse_cpu_fields(std::string_view rest) {
    std::array<uint64_t, 8> fields{};
    std::size_t count = 0;
    while (count < fields.size()) {
        const auto token_begin = rest.find_first_not_of(" \t");
        if (token_begin == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(token_begin);
        const auto token_end = rest.find_first_of(" \t");
        const auto value = parse_u64(rest.substr(0, token_end));
        if (!value) {
            return std::nullopt;
        }
        fields[count] = *value;
        ++count;
        if (token_end == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(token_end);
    }
    // 至少 user/nice/system/idle 四个字段才可用。
    if (count < 4) {
        return std::nullopt;
    }
    RawCpuCoreTimes times;
    for (std::size_t i = 0; i < count; ++i) {
        times.total_ticks += fields[i];
    }
    times.idle_ticks = fields[3] + fields[4];
    return times;
}

/// 读取 /proc/stat：全局 CPU 累计计数器、每核累计计数器、上下文切换与中断累计计数。
///
/// 单次读文件保证全局与每核同源；`ctxt`/`intr` 行缺失时对应字段为 std::nullopt
/// （上层按可用性描述标记不可用）。全局 "cpu " 行缺失或不可解析时返回 std::nullopt。
std::optional<RawCpuTimes> read_cpu_times() {
    RawCpuTimes times;
    bool have_total = false;
    std::optional<uint64_t> context_switches;
    std::optional<uint64_t> interrupts;
    for (const auto& raw : read_lines("/proc/stat")) {
        const auto line = trim(raw);
        if (line.starts_with("cpu ")) {
            // 全局行 "cpu "（带空格，排除 cpuN 单核行）。
            const auto parsed = parse_cpu_fields(line.substr(4));
            if (!parsed) {
                return std::nullopt;
            }
            times.total_ticks = parsed->total_ticks;
            times.idle_ticks = parsed->idle_ticks;
            have_total = true;
            continue;
        }
        if (line.starts_with("cpu")) {
            // 单核行 "cpuN"：N 为 OS CPU 编号（热插拔后可能不连续）。
            const auto index_end = line.find_first_of(" \t");
            if (index_end == std::string_view::npos) {
                continue;
            }
            const auto parsed_index = parse_u64(line.substr(3, index_end - 3));
            if (!parsed_index || *parsed_index > std::numeric_limits<uint32_t>::max()) {
                return std::nullopt;
            }
            const auto parsed = parse_cpu_fields(line.substr(index_end));
            if (!parsed) {
                return std::nullopt;
            }
            RawCpuCoreTimes core = *parsed;
            core.index = static_cast<uint32_t>(*parsed_index);
            times.cores.push_back(core);
            continue;
        }
        if (line.starts_with("ctxt ")) {
            context_switches = parse_u64(trim(line.substr(5)));
            continue;
        }
        if (line.starts_with("intr ")) {
            // 首个数字为总中断数，其后为各 IRQ 的累计计数。
            const auto rest = trim(line.substr(5));
            const auto token_end = rest.find_first_of(" \t");
            interrupts = parse_u64(rest.substr(0, token_end));
            continue;
        }
    }
    if (!have_total) {
        return std::nullopt;
    }
    times.context_switches = context_switches;
    times.interrupts = interrupts;
    return times;
}

/// 读取系统负载（真实 loadavg）：getloadavg 返回 3 个值，失败返回 std::nullopt。
std::optional<RawLoadAverage> read_load_average() {
    std::array<double, 3> values{};
    if (::getloadavg(values.data(), static_cast<int>(values.size())) != 3) {
        return std::nullopt;
    }
    RawLoadAverage load;
    load.one = values[0];
    load.five = values[1];
    load.fifteen = values[2];
    load.semantic = SYSMON_LOAD_SEMANTIC_LOAD_AVERAGE;
    return load;
}

// ===== 内存与交换（契约 sysmon §2.4、§2.7）=====

/// `/proc/meminfo` 路径（内存与交换共用的数据源）。
constexpr const char* kMeminfoPath = "/proc/meminfo";

/// 读取整个文本文件；不可打开时返回空串（失败降级，不抛异常）。
std::string read_file_contents(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

/// 记录 `MemAvailable` 异常回退（英文、小写开头、无句号；契约 sysmon §2.7）。
void log_mem_available_fallback(system_collector_linux_internal::MemAvailableFallback fallback,
                                const system_collector_linux_internal::MeminfoValues& values,
                                const RawMemoryInfo& memory) {
    const uint64_t total_kb = values.mem_total_kb.value_or(0);
    const uint64_t available_kb = values.mem_available_kb.value_or(0);
    switch (fallback) {
        case system_collector_linux_internal::MemAvailableFallback::Missing:
            SPDLOG_WARN(
                "meminfo memavailable missing, using fallback | mem_total_kb={} "
                "available_bytes={}",
                total_kb, memory.available_bytes);
            break;
        case system_collector_linux_internal::MemAvailableFallback::Zero:
            SPDLOG_WARN(
                "meminfo memavailable zero, using fallback | mem_total_kb={} available_bytes={}",
                total_kb, memory.available_bytes);
            break;
        case system_collector_linux_internal::MemAvailableFallback::AboveTotal:
            SPDLOG_WARN(
                "meminfo memavailable above total, using fallback | mem_total_kb={} "
                "mem_available_kb={}",
                total_kb, available_kb);
            break;
        case system_collector_linux_internal::MemAvailableFallback::None:
            break;
    }
}

/// `/proc/meminfo` 内存/交换读取器（契约 sysmon §2.4、§2.7）。
///
/// 日志去重：`MemAvailable` 持续异常或读取持续失败时只在状态首次出现时 WARN 一次，
/// 避免 7x24 运行下每秒刷屏；恢复正常后重置去重标记。
class LinuxMemorySwapReader {
public:
    /// 读取内存读数；总量不可用或回退也无法计算时返回 std::nullopt。
    std::optional<RawMemoryInfo> read_memory() {
        const auto values = read_meminfo();
        const auto normalized = system_collector_linux_internal::normalize_memory(values);
        if (!normalized.memory.has_value()) {
            if (!memory_failure_logged_) {
                SPDLOG_WARN("meminfo memory unavailable | path={}", kMeminfoPath);
                memory_failure_logged_ = true;
            }
            return std::nullopt;
        }
        memory_failure_logged_ = false;
        if (normalized.fallback == system_collector_linux_internal::MemAvailableFallback::None) {
            memory_anomaly_logged_ = false;  // 恢复正常：下次异常重新记日志
        } else if (!memory_anomaly_logged_) {
            log_mem_available_fallback(normalized.fallback, values, *normalized.memory);
            memory_anomaly_logged_ = true;
        }
        return normalized.memory;
    }

    /// 读取交换读数；`std::nullopt` = 采集失败，`total_bytes = 0` = 本机无交换。
    std::optional<RawSwapInfo> read_swap() {
        const auto swap = system_collector_linux_internal::normalize_swap(read_meminfo());
        if (!swap.has_value()) {
            if (!swap_failure_logged_) {
                SPDLOG_WARN("meminfo swap unavailable | path={}", kMeminfoPath);
                swap_failure_logged_ = true;
            }
            return std::nullopt;
        }
        swap_failure_logged_ = false;
        return swap;
    }

private:
    /// 读 `/proc/meminfo` 并解析；文件不可读时返回全 nullopt。
    static system_collector_linux_internal::MeminfoValues read_meminfo() {
        const std::string content = read_file_contents(kMeminfoPath);
        return system_collector_linux_internal::parse_meminfo(content);
    }

    bool memory_anomaly_logged_ = false;  ///< MemAvailable 异常日志去重（恢复后重置）
    bool memory_failure_logged_ = false;  ///< 内存采集失败日志去重（恢复后重置）
    bool swap_failure_logged_ = false;    ///< 交换采集失败日志去重（恢复后重置）
};

// ===== 温度与每核频率（契约 sysmon §2.3、§2.4）=====

/// sysfs 温度传感器根目录（每个子目录对应一颗传感器芯片）。
constexpr const char* kHwmonRoot = "/sys/class/hwmon";
/// sysfs CPU 拓扑根目录（每个 `cpuN` 子目录对应一个逻辑核）。
constexpr const char* kCpuRoot = "/sys/devices/system/cpu";
/// 温度输入文件名：`temp<N>_input`（毫摄氏度）。
constexpr std::string_view kTemperatureInputPrefix = "temp";
constexpr std::string_view kTemperatureInputSuffix = "_input";
/// 温度标签文件名后缀：`temp<N>_label`（可选，人类可读标签）。
constexpr std::string_view kTemperatureLabelSuffix = "_label";

/// 匹配 `temp<数字>_input` 文件名：返回中间的 N；不匹配返回 std::nullopt。
std::optional<uint32_t> parse_temperature_input_index(std::string_view filename) {
    if (!filename.starts_with(kTemperatureInputPrefix) ||
        !filename.ends_with(kTemperatureInputSuffix)) {
        return std::nullopt;
    }
    const auto digits = filename.substr(
        kTemperatureInputPrefix.size(),
        filename.size() - kTemperatureInputPrefix.size() - kTemperatureInputSuffix.size());
    const auto value = parse_u64(digits);
    if (!value || *value > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*value);
}

/// 匹配 `cpu<数字>` 目录名（排除 cpufreq/cpuidle 等）：返回 OS CPU 编号；不匹配返回 nullopt。
std::optional<uint32_t> parse_cpu_index(std::string_view dirname) {
    constexpr std::string_view kPrefix = "cpu";
    if (!dirname.starts_with(kPrefix) || dirname.size() == kPrefix.size()) {
        return std::nullopt;
    }
    const auto value = parse_u64(dirname.substr(kPrefix.size()));
    if (!value || *value > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*value);
}

/// 把 `tempN_input` 路径换成同名 `tempN_label` 路径。
std::filesystem::path temperature_label_path(const std::filesystem::path& input_path) {
    auto filename = input_path.filename().string();
    filename.resize(filename.size() - kTemperatureInputSuffix.size());
    filename += kTemperatureLabelSuffix;
    return input_path.parent_path() / filename;
}

/// 扫描 /sys/class/hwmon 读取温度传感器读数（契约 sysmon §2.4）。
///
/// 每个 hwmon 目录以 `name` 文件内容为传感器前缀（缺失时回退目录名，如 "hwmon0"）；
/// `tempN_input` 为毫摄氏度，除以 1000 得摄氏温度；标识为
/// `<name>#<hwmonN>:<label>`，无 label 时为 `<name>#<hwmonN>:tempN`。hwmon 目录名唯一，
/// 因此标识在同一启动周期内稳定，且不随其它同名芯片出现/消失变化。单个传感器读取
/// 失败只跳过该条；没有任何可读传感器时返回 std::nullopt（失败降级，不抛异常）。
std::optional<std::vector<RawTemperature>> read_temperatures() {
    std::vector<RawTemperature> readings;
    std::set<std::string> used_sensors;
    auto hwmon_dirs = list_directory(kHwmonRoot);
    std::sort(hwmon_dirs.begin(), hwmon_dirs.end(),
              [](const std::filesystem::directory_entry& lhs,
                 const std::filesystem::directory_entry& rhs) {
                  return lhs.path().filename().string() < rhs.path().filename().string();
              });
    for (const auto& hwmon_dir : hwmon_dirs) {
        auto base_name = read_sysfs_line(hwmon_dir.path() / "name");
        if (base_name.empty()) {
            base_name = hwmon_dir.path().filename().string();
        }
        const auto hwmon_number = hwmon_dir.path().filename().string();
        // 同一 hwmon 目录内按 tempN 的 N 数字序输出，保证顺序稳定。
        std::vector<std::pair<uint32_t, std::filesystem::path>> inputs;
        for (const auto& entry : list_directory(hwmon_dir.path())) {
            if (const auto index =
                    parse_temperature_input_index(entry.path().filename().string())) {
                inputs.emplace_back(*index, entry.path());
            }
        }
        std::sort(inputs.begin(), inputs.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        for (const auto& [index, input_path] : inputs) {
            const auto millidegrees = parse_i64(read_sysfs_line(input_path));
            if (!millidegrees) {
                continue;
            }
            const auto label = read_sysfs_line(temperature_label_path(input_path));
            // hwmon 目录名唯一：常规硬件下标识含 `<hwmonN>` 即帧内唯一，且同帧其它
            // 传感器出现/消失不会改变本传感器标识（契约 sysmon §2.4「稳定标识」）。
            const std::string base_sensor =
                base_name + "#" + hwmon_number + ":" +
                (label.empty() ? "temp" + std::to_string(index) : label);
            // 病态驱动（同一 hwmon 下重复 label，或 label 恰好等于另一项的 tempN 形态）
            // 时追加 `-<n>` 直到帧内唯一；正常硬件不会进入该循环。
            std::string sensor = base_sensor;
            for (uint32_t suffix = 1; !used_sensors.insert(sensor).second; ++suffix) {
                sensor = base_sensor + "-" + std::to_string(suffix);
            }
            RawTemperature reading;
            reading.sensor = std::move(sensor);
            reading.celsius = static_cast<double>(*millidegrees) / 1000.0;
            readings.push_back(std::move(reading));
        }
    }
    if (readings.empty()) {
        return std::nullopt;
    }
    return readings;
}

/// 扫描 /sys/devices/system/cpu/<cpuN>/cpufreq/scaling_cur_freq 读取每核当前频率
/// （kHz → MHz，四舍五入；契约 sysmon §2.3）。
///
/// 目录名 `cpuN` 的 N 即 OS CPU 编号（与 RawCpuCoreTimes::index 同源，不保证连续）；
/// 读取失败或读数为 0 的核跳过；结果按 index 升序；全部不可读时返回 std::nullopt。
std::optional<std::vector<RawCoreFrequency>> read_core_frequencies() {
    std::vector<RawCoreFrequency> frequencies;
    for (const auto& cpu_dir : list_directory(kCpuRoot)) {
        const auto index = parse_cpu_index(cpu_dir.path().filename().string());
        if (!index) {
            continue;
        }
        const auto khz =
            parse_u64(read_sysfs_line(cpu_dir.path() / "cpufreq" / "scaling_cur_freq"));
        if (!khz || *khz == 0) {
            continue;
        }
        // 防御异常内容：加 500 做四舍五入前先排除溢出，结果超出 uint32 的条目跳过。
        if (*khz > std::numeric_limits<uint64_t>::max() - 500) {
            continue;
        }
        const uint64_t mhz = (*khz + 500) / 1000;
        if (mhz == 0 || mhz > std::numeric_limits<uint32_t>::max()) {
            continue;
        }
        RawCoreFrequency frequency;
        frequency.index = *index;
        frequency.mhz = static_cast<uint32_t>(mhz);
        frequencies.push_back(frequency);
    }
    std::sort(frequencies.begin(), frequencies.end(),
              [](const RawCoreFrequency& lhs, const RawCoreFrequency& rhs) {
                  return lhs.index < rhs.index;
              });
    if (frequencies.empty()) {
        return std::nullopt;
    }
    return frequencies;
}

// ===== 卷、关注路径与共享内存目录（契约 sysmon §2.4）=====

/// `/proc/mounts` 路径（卷枚举的数据源；容器内为宿主视图，§8 明确不支持容器语义）。
constexpr const char* kProcMountsPath = "/proc/mounts";

/// 统计目录占用的递归中间结果。
struct DirectoryUsage {
    uint64_t total_bytes = 0;
    uint64_t page_files = 0;
    std::optional<int64_t> oldest_page_age_sec;
};

/// 递归统计目录占用（契约 sysmon §2.4）：总字节数、页文件数、最旧页文件年龄。
///
/// 用 error_code 重载遍历，不抛异常；子目录不可读时跳过该子树（不使整次失败）。
/// 不需要深度上限：`recursive_directory_iterator` 默认**不跟随目录符号链接**，
/// 因此不存在环；真实 shm 目录树只有「根 / 通道目录」两层。
DirectoryUsage scan_directory_usage(const std::filesystem::path& root,
                                    std::chrono::file_clock::time_point now) {
    DirectoryUsage usage;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        std::error_code entry_ec;
        if (entry.is_regular_file(entry_ec) && !entry_ec) {
            const auto size = static_cast<uint64_t>(entry.file_size(entry_ec));
            if (!entry_ec) {
                usage.total_bytes += size;
            }
            if (system_collector_internal::is_shm_page_file_name(
                    entry.path().filename().string())) {
                ++usage.page_files;
                const auto write_time = entry.last_write_time(entry_ec);
                if (!entry_ec) {
                    const auto age =
                        std::chrono::duration_cast<std::chrono::seconds>(now - write_time);
                    const int64_t age_sec = age.count();
                    // 未来时间（时钟回拨/异常 mtime）按 0 处理，不给负年龄。
                    const int64_t clamped = age_sec > 0 ? age_sec : 0;
                    if (!usage.oldest_page_age_sec.has_value() ||
                        clamped > *usage.oldest_page_age_sec) {
                        usage.oldest_page_age_sec = clamped;
                    }
                }
            }
        }
        it.increment(ec);
    }
    return usage;
}

/// 读取并解析 `/proc/mounts`；文件不可读时返回空列表（采集失败降级）。
std::vector<system_collector_linux_internal::MountEntry> read_mounts() {
    // 补设备号并按设备去重：bind mount 会让同一块存储出现多个挂载点，
    // 卷语义是"一块底层存储"，不能列出多个容量相同的卷（契约 sysmon §2.4）。
    return system_collector_linux_internal::deduplicate_by_device(
        system_collector_linux_internal::attach_devices(
            system_collector_linux_internal::parse_mounts(read_file_contents(kProcMountsPath))));
}

/// 采集卷列表与容量（契约 sysmon §2.4）：`statvfs` 逐卷取容量，读不到时留 nullopt + reason。
std::optional<std::vector<RawVolumeInfo>> read_volumes() {
    const auto mounts = read_mounts();
    if (mounts.empty()) {
        return std::nullopt;  // 一个卷都枚举不到 = 整体采集失败
    }
    std::vector<RawVolumeInfo> volumes;
    volumes.reserve(mounts.size());
    for (const auto& mount : mounts) {
        RawVolumeInfo volume;
        volume.mount = mount.mount_point;
        struct statvfs stats{};
        if (::statvfs(mount.mount_point.c_str(), &stats) == 0) {
            const auto block_size =
                static_cast<uint64_t>(stats.f_frsize != 0 ? stats.f_frsize : stats.f_bsize);
            const uint64_t total = static_cast<uint64_t>(stats.f_blocks) * block_size;
            const uint64_t free_bytes = static_cast<uint64_t>(stats.f_bavail) * block_size;
            volume.total_bytes = total;
            volume.free_bytes = std::min(free_bytes, total);
        } else {
            // 卷存在但容量读不到：保留条目并给出原因（不得用 0 冒充，契约 sysmon §2.4）。
            volume.reason =
                (errno == EACCES || errno == EPERM) ? "no_permission" : "collect_failed";
        }
        volumes.push_back(std::move(volume));
    }
    return volumes;
}

/// 采集共享内存目录占用（契约 sysmon §2.4）。
std::optional<RawShmDirInfo> read_shm_dir(const std::string& shm_dir) {
    std::error_code ec;
    const std::filesystem::path root(shm_dir);
    if (!std::filesystem::is_directory(root, ec) || ec) {
        return std::nullopt;  // 目录不存在/不可读：不得输出全 0 的对象
    }
    // `is_directory()` 对 mode 0000 的目录仍返回 true（只需父目录可 stat），而下面的
    // 递归迭代带 skip_permission_denied，会静默产出 0 个条目 + ec=0 —— 那会把"无权限"
    // 谎报成"目录为空"的全 0 对象，正是契约 sysmon §2.4 禁止的。先探一次可读性：
    // 根目录用**不带** skip_permission_denied 的迭代器，打不开就如实报不可用。
    {
        std::error_code probe_ec;
        const std::filesystem::directory_iterator probe(root, probe_ec);
        if (probe_ec) {
            return std::nullopt;
        }
    }
    const auto usage = scan_directory_usage(root, std::chrono::file_clock::now());
    RawShmDirInfo info;
    info.path = root.lexically_normal().string();
    info.total_bytes = usage.total_bytes;
    info.page_files = usage.page_files;
    info.oldest_file_age_sec = usage.oldest_page_age_sec;
    return info;
}

/// 采集一次系统信息（失败字段留空串/0，不抛异常）。
RawSystemInfo collect_system_info_linux() {
    RawSystemInfo info;
    info.os_family = "linux";
    info.os_build = 0;

    struct utsname uname_info{};
    if (uname(&uname_info) == 0) {
        info.kernel_version = uname_info.release;
        info.arch = uname_info.machine;
    }

    info.os_version = read_os_version();

    // HOST_NAME_MAX 通常为 64；缓冲区留足并强制终止，规避截断时无终止符。
    std::array<char, 256> hostname{};
    if (gethostname(hostname.data(), hostname.size()) == 0) {
        hostname.back() = '\0';
        info.hostname = hostname.data();
    }

    info.cpu_model = read_cpu_model();
    info.logical_cores = read_logical_cores();
    info.boot_time_unix_sec = read_boot_time_unix_sec().value_or(0);
    return info;
}

/// Linux 采集器：可在单线程事件循环内重复调用，不缓存（系统信息由调用方在启动时取一次）。
class LinuxSystemCollector final : public SystemCollector {
public:
    RawSystemInfo collect_system_info() override { return collect_system_info_linux(); }

    std::optional<RawCpuTimes> collect_cpu_times() override { return read_cpu_times(); }

    std::optional<RawLoadAverage> collect_load_average() override { return read_load_average(); }

    std::optional<std::vector<RawTemperature>> collect_temperatures() override {
        return read_temperatures();
    }

    std::optional<std::vector<RawCoreFrequency>> collect_core_frequencies() override {
        return read_core_frequencies();
    }

    std::optional<RawMemoryInfo> collect_memory() override { return memory_reader_.read_memory(); }

    std::optional<RawSwapInfo> collect_swap() override { return memory_reader_.read_swap(); }

    std::optional<std::vector<RawVolumeInfo>> collect_volumes() override { return read_volumes(); }

    std::vector<RawWatchPathInfo> collect_watch_paths(
        const std::vector<std::string>& paths) override {
        const auto mounts = read_mounts();
        std::vector<std::string> mount_points;
        mount_points.reserve(mounts.size());
        for (const auto& mount : mounts) {
            mount_points.push_back(mount.mount_point);
        }
        std::vector<RawWatchPathInfo> results;
        results.reserve(paths.size());
        for (const auto& path : paths) {
            RawWatchPathInfo info;
            info.path = path;
            const auto normalized = system_collector_linux_internal::normalize_path(path);
            if (!normalized.has_value()) {
                // 规范化失败：区分"不存在"与"权限不足"，不猜卷（契约 sysmon §2.4）。
                std::error_code ec;
                const bool exists = std::filesystem::exists(path, ec);
                info.reason = (!ec && !exists) ? "path_not_found" : "collect_failed";
                results.push_back(std::move(info));
                continue;
            }
            info.path = *normalized;
            // 规范化成功不等于路径存在（weakly_canonical 保留不存在的尾部用于诊断）：
            // 路径不存在 ⇒ 如实报 path_not_found，不得因为能匹配到父卷就报成功。
            std::error_code exists_ec;
            if (!std::filesystem::exists(*normalized, exists_ec) || exists_ec) {
                info.reason = "path_not_found";
                results.push_back(std::move(info));
                continue;
            }
            // 落卷：只有确实落在采集范围内的卷上才算解析成功。落在被过滤的文件系统
            // （tmpfs/overlay/fuse 等，按设备号判定）报 unsupported，不得回退到 "/"
            // （契约 sysmon §2.4；容器内 shm 目录常正好在 tmpfs 上）。
            const auto mount = system_collector_linux_internal::resolve_volume_mount_point(
                *normalized, mount_points);
            if (!mount.has_value()) {
                info.reason = "unsupported";
                results.push_back(std::move(info));
                continue;
            }
            info.mount = *mount;
            results.push_back(std::move(info));
        }
        return results;
    }

    std::optional<RawShmDirInfo> collect_shm_dir(const std::string& shm_dir) override {
        return read_shm_dir(shm_dir);
    }

private:
    LinuxMemorySwapReader memory_reader_;
};

}  // namespace

// ===== /proc/meminfo 解析与归一（内部接缝，供平台单测确定性覆盖）=====

namespace system_collector_linux_internal {
// ===== /proc/mounts 卷枚举与落卷解析（契约 sysmon §2.4）=====

namespace {

/// `/proc/mounts` 的八进制转义还原（`\040` = 空格、`\011` = tab 等）。
/// 挂载点里常有空格（如 `\040`），不还原会让匹配与展示都对不上。
std::string unescape_mount_field(std::string_view field) {
    std::string result;
    result.reserve(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            const auto is_octal = [](char c) { return c >= '0' && c <= '7'; };
            const char d0 = field[i + 1];
            const char d1 = field[i + 2];
            const char d2 = field[i + 3];
            if (is_octal(d0) && is_octal(d1) && is_octal(d2)) {
                const auto value =
                    static_cast<char>(((d0 - '0') << 6) | ((d1 - '0') << 3) | (d2 - '0'));
                result.push_back(value);
                i += 3;
                continue;
            }
        }
        result.push_back(field[i]);
    }
    return result;
}

/// 判断路径是否以某个挂载点为前缀且落在路径分量边界上（`/home` 不匹配 `/home2`）。
bool is_mount_prefix(std::string_view path, std::string_view mount_point) {
    if (mount_point == "/") {
        return true;
    }
    if (!path.starts_with(mount_point)) {
        return false;
    }
    return path.size() == mount_point.size() || path[mount_point.size()] == '/';
}

}  // namespace

bool is_pseudo_filesystem(std::string_view fs_type) {
    // 契约 sysmon §2.4：过滤内核伪文件系统、内存文件系统与无独立容量语义的挂载。
    // 清单按**精确类型名**匹配（cgroup 与 cgroup2 各列一条）；只有 `fuse.*` 家族用前缀，
    // 因为其类型名带实现后缀（如 `fuse.sshfs`）。
    static constexpr std::array<std::string_view, 26> kPseudo = {
        "proc",        "sysfs",      "devtmpfs", "devpts",     "tmpfs",   "ramfs",     "cgroup",
        "cgroup2",     "debugfs",    "tracefs",  "configfs",   "mqueue",  "hugetlbfs", "autofs",
        "binfmt_misc", "nsfs",       "pstore",   "securityfs", "fusectl", "bpf",       "efivarfs",
        "selinuxfs",   "rpc_pipefs", "nfsd",     "overlay",    "squashfs"};
    for (const auto candidate : kPseudo) {
        if (fs_type == candidate) {
            return true;
        }
    }
    // `fuse.*` / `fuse.` 家族：用户态文件系统容量语义由实现决定，不作为卷基线。
    return fs_type.starts_with("fuse.");
}

std::vector<MountEntry> parse_mounts(std::string_view content, bool include_pseudo) {
    std::vector<MountEntry> entries;
    std::set<std::string> seen_mounts;
    std::string_view rest = content;
    while (!rest.empty()) {
        const auto newline = rest.find('\n');
        const std::string_view raw_line =
            newline == std::string_view::npos ? rest : rest.substr(0, newline);
        rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
        const auto line = trim(raw_line);
        if (line.empty()) {
            continue;
        }
        // 字段：device mount_point fs_type options [dump pass]
        std::array<std::string_view, 3> fields{};
        std::size_t field_count = 0;
        std::string_view cursor = line;
        while (field_count < fields.size()) {
            const auto begin = cursor.find_first_not_of(" \t");
            if (begin == std::string_view::npos) {
                break;
            }
            cursor.remove_prefix(begin);
            const auto end = cursor.find_first_of(" \t");
            fields[field_count] = cursor.substr(0, end);
            ++field_count;
            if (end == std::string_view::npos) {
                break;
            }
            cursor.remove_prefix(end);
        }
        if (field_count < 3) {
            continue;
        }
        MountEntry entry;
        entry.mount_point = unescape_mount_field(fields[1]);
        entry.fs_type = std::string(fields[2]);
        if (entry.mount_point.empty() || !entry.mount_point.starts_with('/')) {
            continue;
        }
        if (!include_pseudo && is_pseudo_filesystem(entry.fs_type)) {
            continue;
        }
        if (!seen_mounts.insert(entry.mount_point).second) {
            continue;  // 同一挂载点只保留首次出现（bind mount 重复列出）
        }
        entries.push_back(std::move(entry));
    }
    // 最长前缀匹配直接可用：按挂载点长度降序（同长度按字典序保证确定性）。
    std::sort(entries.begin(), entries.end(), [](const MountEntry& lhs, const MountEntry& rhs) {
        if (lhs.mount_point.size() != rhs.mount_point.size()) {
            return lhs.mount_point.size() > rhs.mount_point.size();
        }
        return lhs.mount_point < rhs.mount_point;
    });
    return entries;
}

std::vector<MountEntry> attach_devices(std::vector<MountEntry> entries) {
    for (auto& entry : entries) {
        struct stat info{};
        if (::stat(entry.mount_point.c_str(), &info) == 0) {
            entry.device = static_cast<uint64_t>(info.st_dev);
        }
    }
    return entries;
}

std::vector<MountEntry> deduplicate_by_device(std::vector<MountEntry> entries) {
    // 先按"最短挂载点优先"挑选代表，再输出：这样 `/` 会成为 sda2 的代表，
    // 而 `/home/user` 这类 bind mount 不会重复出现。
    std::sort(entries.begin(), entries.end(), [](const MountEntry& lhs, const MountEntry& rhs) {
        if (lhs.mount_point.size() != rhs.mount_point.size()) {
            return lhs.mount_point.size() < rhs.mount_point.size();
        }
        return lhs.mount_point < rhs.mount_point;
    });
    std::vector<MountEntry> result;
    std::set<uint64_t> seen_devices;
    for (auto& entry : entries) {
        if (entry.device != 0 && !seen_devices.insert(entry.device).second) {
            continue;
        }
        result.push_back(std::move(entry));
    }
    // 输出顺序回到"挂载点长度降序"（供最长前缀匹配直接使用）。
    std::sort(result.begin(), result.end(), [](const MountEntry& lhs, const MountEntry& rhs) {
        if (lhs.mount_point.size() != rhs.mount_point.size()) {
            return lhs.mount_point.size() > rhs.mount_point.size();
        }
        return lhs.mount_point < rhs.mount_point;
    });
    return result;
}

std::optional<std::string> match_mount_point(std::string_view normalized_path,
                                             const std::vector<std::string>& mount_points) {
    std::optional<std::string> best;
    for (const auto& mount_point : mount_points) {
        if (mount_point == "/") {
            continue;  // 根挂载点最后兜底，避免抢先匹配
        }
        if (is_mount_prefix(normalized_path, mount_point)) {
            if (!best.has_value() || mount_point.size() > best->size()) {
                best = mount_point;
            }
        }
    }
    if (best.has_value()) {
        return best;
    }
    if (std::ranges::find(mount_points, "/") != mount_points.end()) {
        return std::string("/");
    }
    return std::nullopt;
}

std::optional<std::string> resolve_volume_mount_point(
    const std::string& normalized_path,
    const std::vector<std::string>& volume_mount_points,
    const std::string& root_mount_point) {
    // 先在不含根的集合里找最长前缀：命中即确定落在采集范围内的某个卷上。
    std::optional<std::string> best;
    for (const auto& mount_point : volume_mount_points) {
        if (mount_point == "/") {
            continue;
        }
        if (is_mount_prefix(normalized_path, mount_point)) {
            if (!best.has_value() || mount_point.size() > best->size()) {
                best = mount_point;
            }
        }
    }
    if (best.has_value()) {
        return best;
    }
    // 没命中：只有"确实在根文件系统上"才归到根卷，否则视为不在采集范围。
    // 无条件回退到 "/" 会把 tmpfs/overlay/fuse 上的路径谎报成根卷（契约 §2.4 禁止）。
    struct stat root_info{};
    struct stat path_info{};
    if (::stat(root_mount_point.c_str(), &root_info) != 0 ||
        ::stat(normalized_path.c_str(), &path_info) != 0) {
        return std::nullopt;
    }
    if (root_info.st_dev != path_info.st_dev) {
        return std::nullopt;  // 落在被过滤的挂载点上（tmpfs/overlay/fuse 等）
    }
    return root_mount_point;
}

std::optional<std::string> normalize_path(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path input(path);
    const auto canonical = std::filesystem::weakly_canonical(input, ec);
    if (ec) {
        return std::nullopt;
    }
    return canonical.string();
}

namespace {

constexpr uint64_t kBytesPerKb = 1024;
constexpr uint64_t kMaxU64 = std::numeric_limits<uint64_t>::max();

/// 饱和加法：溢出时返回 false（异常值防御，不抛异常）。
bool add_kb(uint64_t& accumulator, uint64_t value) {
    if (accumulator > kMaxU64 - value) {
        return false;
    }
    accumulator += value;
    return true;
}

/// 可选 kB 相加：两者都缺失时返回 std::nullopt；溢出返回 std::nullopt。
std::optional<uint64_t> add_optional_kb(std::optional<uint64_t> lhs, std::optional<uint64_t> rhs) {
    if (!lhs.has_value() && !rhs.has_value()) {
        return std::nullopt;
    }
    uint64_t sum = lhs.value_or(0);
    if (rhs.has_value() && !add_kb(sum, *rhs)) {
        return std::nullopt;
    }
    return sum;
}

/// kB 转 bytes 并钳制到总量；字段缺失返回 std::nullopt。
std::optional<uint64_t> kb_to_bytes_clamped(std::optional<uint64_t> kb, uint64_t max_kb) {
    if (!kb.has_value()) {
        return std::nullopt;
    }
    return std::min(*kb, max_kb) * kBytesPerKb;
}

}  // namespace

MeminfoValues parse_meminfo(std::string_view content) {
    MeminfoValues values;
    // 同名字段以首次解析成功为准（/proc 正常不会重复；防御性内容只取第一次）。
    const auto assign = [](std::optional<uint64_t>& field, std::string_view raw_value) {
        if (field.has_value()) {
            return;
        }
        const auto value = parse_u64(raw_value);
        if (value.has_value()) {
            field = value;
        }
    };
    std::string_view rest = content;
    while (!rest.empty()) {
        const auto newline = rest.find('\n');
        const std::string_view raw_line =
            newline == std::string_view::npos ? rest : rest.substr(0, newline);
        rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
        const auto line = trim(raw_line);
        if (line.empty()) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const auto key = trim(line.substr(0, colon));
        const auto value_field = trim(line.substr(colon + 1));
        // 取第一个数字段（忽略 "kB" 等单位；无单位条目同样适用）。
        const auto value_end = value_field.find_first_of(" \t");
        const auto value_text = value_field.substr(0, value_end);
        if (key == "MemTotal") {
            assign(values.mem_total_kb, value_text);
        } else if (key == "MemFree") {
            assign(values.mem_free_kb, value_text);
        } else if (key == "MemAvailable") {
            assign(values.mem_available_kb, value_text);
        } else if (key == "Buffers") {
            assign(values.buffers_kb, value_text);
        } else if (key == "Cached") {
            assign(values.cached_kb, value_text);
        } else if (key == "SReclaimable") {
            assign(values.sreclaimable_kb, value_text);
        } else if (key == "SwapTotal") {
            assign(values.swap_total_kb, value_text);
        } else if (key == "SwapFree") {
            assign(values.swap_free_kb, value_text);
        }
    }
    return values;
}

NormalizedMemory normalize_memory(const MeminfoValues& values) {
    NormalizedMemory result;
    if (!values.mem_total_kb.has_value() || *values.mem_total_kb == 0) {
        return result;  // 总量不可用：本次内存采集失败
    }
    const uint64_t total_kb = *values.mem_total_kb;
    if (total_kb > kMaxU64 / kBytesPerKb) {
        return result;  // 异常内容：kB -> bytes 会溢出
    }

    // MemAvailable 正常时直接使用；缺失/为 0/大于总量时回退到可回收量之和。
    uint64_t available_kb = 0;
    bool use_fallback = false;
    if (!values.mem_available_kb.has_value()) {
        result.fallback = MemAvailableFallback::Missing;
        use_fallback = true;
    } else if (*values.mem_available_kb == 0) {
        result.fallback = MemAvailableFallback::Zero;
        use_fallback = true;
    } else if (*values.mem_available_kb > total_kb) {
        result.fallback = MemAvailableFallback::AboveTotal;
        use_fallback = true;
    }
    if (!use_fallback) {
        available_kb = *values.mem_available_kb;
    } else {
        if (!values.mem_free_kb.has_value() || !values.buffers_kb.has_value() ||
            !values.cached_kb.has_value()) {
            return result;  // 回退所需字段缺失：本次内存采集失败
        }
        available_kb = *values.mem_free_kb;
        if (!add_kb(available_kb, *values.buffers_kb) || !add_kb(available_kb, *values.cached_kb) ||
            (values.sreclaimable_kb.has_value() &&
             !add_kb(available_kb, *values.sreclaimable_kb))) {
            return result;  // 异常内容：回退求和溢出
        }
    }
    // 防御：可用量不得超过总量（正常值不动；异常回退值钳制）。
    available_kb = std::min(available_kb, total_kb);

    RawMemoryInfo memory;
    memory.total_bytes = total_kb * kBytesPerKb;
    memory.available_bytes = available_kb * kBytesPerKb;
    // cached = Cached + SReclaimable（可回收页缓存口径）；buffers 仅 Linux 提供。
    memory.cached_bytes =
        kb_to_bytes_clamped(add_optional_kb(values.cached_kb, values.sreclaimable_kb), total_kb);
    memory.buffers_bytes = kb_to_bytes_clamped(values.buffers_kb, total_kb);
    result.memory = memory;
    return result;
}

std::optional<RawSwapInfo> normalize_swap(const MeminfoValues& values) {
    if (!values.swap_total_kb.has_value() || !values.swap_free_kb.has_value()) {
        return std::nullopt;
    }
    const uint64_t total_kb = *values.swap_total_kb;
    if (total_kb > kMaxU64 / kBytesPerKb) {
        return std::nullopt;  // 异常内容：kB -> bytes 会溢出
    }
    RawSwapInfo swap;
    swap.total_bytes = total_kb * kBytesPerKb;
    // 防御：SwapFree 超过 SwapTotal 时钳制，保证 used = total - free 落在 [0, total]。
    swap.free_bytes = std::min(*values.swap_free_kb, total_kb) * kBytesPerKb;
    return swap;
}

}  // namespace system_collector_linux_internal

std::unique_ptr<SystemCollector> make_system_collector() {
    return std::make_unique<LinuxSystemCollector>();
}

}  // namespace dztrader::platform

#endif  // defined(__linux__)
