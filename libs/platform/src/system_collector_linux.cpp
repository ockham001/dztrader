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
 * 采集失败一律降级：系统信息返回空串/0，CPU 计数器返回 std::nullopt，不抛异常
 * （契约 sysmon §2.7：采集失败跳过本次计量由上层记缺口）。
 */

#if defined(__linux__)

#include <dztrader/platform/system_collector.h>

#include <sys/utsname.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
};

}  // namespace

std::unique_ptr<SystemCollector> make_system_collector() {
    return std::make_unique<LinuxSystemCollector>();
}

}  // namespace dztrader::platform

#endif  // defined(__linux__)
