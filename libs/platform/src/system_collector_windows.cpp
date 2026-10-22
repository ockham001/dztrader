/**
 * @file system_collector_windows.cpp
 * @brief Windows 系统指标采集实现（契约 sysmon）。
 *
 * 仅当编译目标为 Windows 时编译；其他平台产生空翻译单元（Linux 实现见
 * system_collector_linux.cpp，工厂函数每平台只定义一次）。
 *
 * 版本探测必须走不受 manifest 影响的 ntdll!RtlGetVersion（契约 sysmon §2.2 与
 * platform_requirements.h 明文禁止 GetVersionEx / VerifyVersionInfo / IsWindows*）。
 * 每核时间、上下文切换与中断走 ntdll!NtQuerySystemInformation 的 NT 原生信息类；
 * 处理器队列长度代理 load average 走 PDH（Performance Data Helper）。
 *
 * 规格与算法参考 psutil（BSD-3-Clause）：仅借鉴 OS API 选择、边界处理语义与测试不变量，
 * 不翻译其代码、不引入依赖；归属声明见仓库根 THIRD_PARTY_NOTICES.md。NT 结构体布局是
 * Windows 内核 ABI 事实（Windows Internals / phnt），本文件按事实重新声明。
 *
 * 采集失败一律降级：系统信息返回空串/0，CPU 计数器返回 std::nullopt，温度与每核频率
 * 本版本恒返回 std::nullopt，不抛异常（契约 sysmon §2.7：采集失败跳过本次计量由上层记缺口）。
 */

#if defined(_WIN32)

// 项目仅支持 Windows x86_64（AGENTS 跨平台硬门禁：非支持组合必须 #error，禁止静默回退）。
#if !defined(_M_X64) && !defined(__x86_64__)
#error "dzsysmon: only windows x64 is supported"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>  // PDH_CSTATUS_VALID_DATA / PDH_CSTATUS_NEW_DATA（部分 SDK 的 pdh.h 不引入）
#include <winreg.h>

#include <dztrader/platform/system_collector.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dztrader::platform {
namespace {

/// FILETIME 纪元（1601-01-01）到 Unix 纪元（1970-01-01）的 100ns 数。
constexpr ULONGLONG kFiletimeUnixEpochOffset = 116444736000000000ULL;
/// FILETIME 每秒的 100ns 数。
constexpr ULONGLONG kFiletimeTicksPerSecond = 10000000ULL;

/// RtlGetVersion 函数签名（NTSTATUS = LONG；成功返回 0 = STATUS_SUCCESS）。
using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

/// 动态解析 ntdll!RtlGetVersion；解析失败返回 nullptr（不静默使用被垫片的 API）。
RtlGetVersionFn load_rtl_get_version() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
}

/// 宽字符（UTF-16）转 UTF-8；空串或转换失败返回空串。
std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int input_length = static_cast<int>(text.size());
    const int utf8_length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), input_length, nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.data(), input_length, utf8.data(), utf8_length,
                            nullptr, nullptr) <= 0) {
        return {};
    }
    return utf8;
}

/// 去掉字符串两端的 ASCII 空白（注册表字符串可能带首尾空格）。
std::string trim_ascii(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    text.erase(end + 1);
    text.erase(0, begin);
    return text;
}

/// 读取注册表字符串值（REG_SZ）；键/值缺失或读取失败返回空串。
std::wstring read_registry_string(HKEY root, const wchar_t* sub_key, const wchar_t* value_name) {
    DWORD type = 0;
    DWORD byte_size = 0;
    LSTATUS status =
        RegGetValueW(root, sub_key, value_name, RRF_RT_REG_SZ, &type, nullptr, &byte_size);
    if (status != ERROR_SUCCESS || byte_size == 0) {
        return {};
    }
    std::wstring buffer(byte_size / sizeof(wchar_t), L'\0');
    status =
        RegGetValueW(root, sub_key, value_name, RRF_RT_REG_SZ, &type, buffer.data(), &byte_size);
    if (status != ERROR_SUCCESS) {
        return {};
    }
    buffer.resize(byte_size / sizeof(wchar_t));
    // REG_SZ 数据可能以终止符结尾，去掉尾部 NUL 与首尾空白。
    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return buffer;
}

/// 编译期确定的本进程架构（上方 #error 已锁定 Windows x64，无回退分支）。
constexpr const char* compiled_arch() { return "x86_64"; }

/// 人类可读 OS 版本：优先注册表 ProductName（如 "Windows 10 Pro"），缺失时回退
/// "Windows 10"/"Windows 11"；Windows 11 的 ProductName 仍可能报 "Windows 10 ..."，
/// 因此按 RtlGetVersion 的 build 号修正产品名前缀（build >= 22000 = Windows 11）。
std::string query_os_version(uint32_t build) {
    constexpr uint32_t kWindows11Build = 22000;
    std::string label = build >= kWindows11Build ? "Windows 11" : "Windows 10";
    const auto product_name = read_registry_string(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    std::string name = trim_ascii(wide_to_utf8(product_name));
    if (name.empty()) {
        return label;
    }
    // "Windows 10" 与 "Windows 11" 前缀等长（10 个字符）。
    constexpr std::string_view kProductPrefix = "Windows 10";
    if (name.starts_with("Windows 10") || name.starts_with("Windows 11")) {
        name = label + name.substr(kProductPrefix.size());
    }
    return name;
}

/// 主机名（DNS 主机名）；查询失败返回空串。
std::string query_hostname() {
    std::wstring buffer(256, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (GetComputerNameExW(ComputerNameDnsHostname, buffer.data(), &size) == 0) {
        // 缓冲不足时 size 为所需容量（含终止符），扩容后重试一次；其余失败直接降级。
        if (GetLastError() != ERROR_MORE_DATA || size == 0) {
            return {};
        }
        buffer.resize(size);
        size = static_cast<DWORD>(buffer.size());
        if (GetComputerNameExW(ComputerNameDnsHostname, buffer.data(), &size) == 0) {
            return {};
        }
    }
    buffer.resize(size);  // 成功时 size 为写入字符数（不含终止符）
    return wide_to_utf8(buffer);
}

/// CPU 型号（注册表 CentralProcessor\0\ProcessorNameString）；取不到返回空串。
std::string query_cpu_model() {
    const auto model = read_registry_string(HKEY_LOCAL_MACHINE,
                                            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                            L"ProcessorNameString");
    return trim_ascii(wide_to_utf8(model));
}

/// 逻辑核数：优先 GetActiveProcessorCount（支持 >64 核的处理器组），失败回退
/// GetSystemInfo；均失败返回 0。
uint32_t query_logical_cores() {
    const DWORD active = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (active != 0) {
        return static_cast<uint32_t>(active);
    }
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    return static_cast<uint32_t>(system_info.dwNumberOfProcessors);
}

/// FILETIME（自 1601 年起 100ns）转 uint64。
uint64_t filetime_to_u64(const FILETIME& file_time) {
    ULARGE_INTEGER value;
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return static_cast<uint64_t>(value.QuadPart);
}

/// LARGE_INTEGER 转 uint64（NT 原生 API 的 100ns 计数）。
uint64_t large_integer_to_u64(const LARGE_INTEGER& value) {
    return static_cast<uint64_t>(value.QuadPart);
}

/// 启动时间（Unix 秒）：当前墙钟（FILETIME）减去 GetTickCount64 的运行时长。
int64_t query_boot_time_unix_sec() {
    FILETIME now_file_time{};
    GetSystemTimeAsFileTime(&now_file_time);
    const uint64_t now_100ns = filetime_to_u64(now_file_time);
    if (now_100ns <= kFiletimeUnixEpochOffset) {
        return 0;
    }
    const uint64_t now_unix_sec = (now_100ns - kFiletimeUnixEpochOffset) / kFiletimeTicksPerSecond;
    const uint64_t uptime_sec = static_cast<uint64_t>(GetTickCount64()) / 1000ULL;
    if (uptime_sec >= now_unix_sec) {
        return 0;  // 异常防御：启动时间不得为负
    }
    return static_cast<int64_t>(now_unix_sec - uptime_sec);
}

/// 采集一次系统信息（失败字段留空串/0，不抛异常）。
RawSystemInfo collect_system_info_windows() {
    RawSystemInfo info;
    info.os_family = "windows";
    info.arch = compiled_arch();

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = static_cast<DWORD>(sizeof(version));
    const auto rtl_get_version = load_rtl_get_version();
    if (rtl_get_version != nullptr && rtl_get_version(&version) == 0) {
        info.kernel_version = std::to_string(version.dwMajorVersion) + "." +
                              std::to_string(version.dwMinorVersion) + "." +
                              std::to_string(version.dwBuildNumber);
        info.os_build = static_cast<uint32_t>(version.dwBuildNumber);
        info.os_version = query_os_version(info.os_build);
    }

    info.hostname = query_hostname();
    info.cpu_model = query_cpu_model();
    info.logical_cores = query_logical_cores();
    info.boot_time_unix_sec = query_boot_time_unix_sec();
    return info;
}

// ===== NT 原生 API：每核时间 / 上下文切换（契约 sysmon §2.3、§2.7）=====
//
// 结构布局是 NT 内核 ABI 事实（Windows Internals / phnt / psutil ntextapi.h），
// 本文件按事实重新声明，不复制第三方实现代码。

/// NtQuerySystemInformation 信息类别（契约 sysmon）。
constexpr ULONG kSystemPerformanceInformation = 2;
constexpr ULONG kSystemProcessorPerformanceInformation = 8;
/// STATUS_INFO_LENGTH_MISMATCH：缓冲区不足时内核返回并回填所需长度。
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004L);

using NtQuerySystemInformationFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);

/// 动态解析 ntdll!NtQuerySystemInformation；解析失败返回 nullptr。
NtQuerySystemInformationFn load_nt_query_system_information() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
}

/// SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION：每核累计时间（100ns；x64 大小 48 字节）。
/// `kernel_time` 已包含 `idle_time`，故 total = kernel + user（与 GetSystemTimes 同口径）。
struct NtProcessorPerformanceInformation {
    LARGE_INTEGER idle_time;
    LARGE_INTEGER kernel_time;
    LARGE_INTEGER user_time;
    LARGE_INTEGER dpc_time;
    LARGE_INTEGER interrupt_time;
    ULONG interrupt_count;
};
static_assert(sizeof(NtProcessorPerformanceInformation) == 48,
              "SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION layout mismatch");

/// SYSTEM_PERFORMANCE_INFORMATION：内核全局累计量（x64 大小 0x138，ContextSwitches 在 0x128）。
/// 只声明到 SystemCalls；这是 Windows 内核 ABI 的固定布局（Windows Internals / phnt）。
struct NtSystemPerformanceInformation {
    LARGE_INTEGER idle_process_time;
    LARGE_INTEGER io_read_transfer_count;
    LARGE_INTEGER io_write_transfer_count;
    LARGE_INTEGER io_other_transfer_count;
    ULONG io_read_operation_count;
    ULONG io_write_operation_count;
    ULONG io_other_operation_count;
    ULONG available_pages;
    ULONG committed_pages;
    ULONG commit_limit;
    ULONG peak_commitment;
    ULONG page_fault_count;
    ULONG copy_on_write_count;
    ULONG transition_count;
    ULONG cache_transition_count;
    ULONG demand_zero_count;
    ULONG page_read_count;
    ULONG page_read_io_count;
    ULONG cache_read_count;
    ULONG cache_io_count;
    ULONG dirty_pages_write_count;
    ULONG dirty_write_io_count;
    ULONG mapped_pages_write_count;
    ULONG mapped_write_io_count;
    ULONG paged_pool_pages;
    ULONG non_paged_pool_pages;
    ULONG paged_pool_allocs;
    ULONG paged_pool_frees;
    ULONG non_paged_pool_allocs;
    ULONG non_paged_pool_frees;
    ULONG free_system_ptes;
    ULONG resident_system_code_page;
    ULONG total_system_driver_pages;
    ULONG total_system_code_pages;
    ULONG non_paged_pool_lookaside_hits;
    ULONG paged_pool_lookaside_hits;
    ULONG available_paged_pool_pages;
    ULONG resident_system_cache_page;
    ULONG resident_paged_pool_page;
    ULONG resident_system_driver_page;
    ULONG cc_fast_read_no_wait;
    ULONG cc_fast_read_wait;
    ULONG cc_fast_read_resource_miss;
    ULONG cc_fast_read_not_possible;
    ULONG cc_fast_mdl_read_no_wait;
    ULONG cc_fast_mdl_read_wait;
    ULONG cc_fast_mdl_read_resource_miss;
    ULONG cc_fast_mdl_read_not_possible;
    ULONG cc_map_data_no_wait;
    ULONG cc_map_data_wait;
    ULONG cc_map_data_no_wait_miss;
    ULONG cc_map_data_wait_miss;
    ULONG cc_pin_mapped_data_count;
    ULONG cc_pin_read_no_wait;
    ULONG cc_pin_read_wait;
    ULONG cc_pin_read_no_wait_miss;
    ULONG cc_pin_read_wait_miss;
    ULONG cc_copy_read_no_wait;
    ULONG cc_copy_read_wait;
    ULONG cc_copy_read_no_wait_miss;
    ULONG cc_copy_read_wait_miss;
    ULONG cc_mdl_read_no_wait;
    ULONG cc_mdl_read_wait;
    ULONG cc_mdl_read_no_wait_miss;
    ULONG cc_mdl_read_wait_miss;
    ULONG cc_read_ahead_ios;
    ULONG cc_lazy_write_ios;
    ULONG cc_lazy_write_pages;
    ULONG cc_data_flushes;
    ULONG cc_data_pages;
    ULONG context_switches;
    ULONG first_level_tb_fills;
    ULONG second_level_tb_fills;
    ULONG system_calls;
};
static_assert(sizeof(NtSystemPerformanceInformation) == static_cast<std::size_t>(0x138),
              "SYSTEM_PERFORMANCE_INFORMATION layout mismatch");
static_assert(offsetof(NtSystemPerformanceInformation, context_switches) ==
                  static_cast<std::size_t>(0x128),
              "SYSTEM_PERFORMANCE_INFORMATION context switches offset mismatch");

/// 每核采集结果：每核累计时间 + 全系统累计中断数（中断数由每核 InterruptCount 求和）。
struct PerCoreSample {
    std::vector<RawCpuCoreTimes> cores;
    uint64_t interrupts = 0;
};

/// 查询每核累计时间与累计中断数。
///
/// >64 逻辑核的机器上内核只返回调用线程所在处理器组的条目；此时条目数少于逻辑核数，
/// 调用方不得把它当作全系统每核序列（契约 sysmon §2.3：可用时序列数 == 逻辑核数）。
std::optional<PerCoreSample> query_per_core_times(uint32_t logical_cores) {
    if (logical_cores == 0) {
        return std::nullopt;
    }
    const auto nt_query = load_nt_query_system_information();
    if (nt_query == nullptr) {
        return std::nullopt;
    }
    constexpr auto kEntrySize = sizeof(NtProcessorPerformanceInformation);
    std::size_t capacity = static_cast<std::size_t>(logical_cores);
    std::vector<std::byte> buffer(capacity * kEntrySize);
    ULONG returned_length = 0;
    LONG status = nt_query(kSystemProcessorPerformanceInformation, buffer.data(),
                           static_cast<ULONG>(buffer.size()), &returned_length);
    if (status == kStatusInfoLengthMismatch && returned_length > buffer.size()) {
        buffer.resize(returned_length);
        status = nt_query(kSystemProcessorPerformanceInformation, buffer.data(),
                          static_cast<ULONG>(buffer.size()), &returned_length);
    }
    if (status < 0 || returned_length == 0) {
        return std::nullopt;
    }
    const std::size_t entries = static_cast<std::size_t>(returned_length) / kEntrySize;
    if (entries == 0) {
        return std::nullopt;
    }
    PerCoreSample sample;
    sample.cores.reserve(entries);
    for (std::size_t i = 0; i < entries; ++i) {
        NtProcessorPerformanceInformation entry{};
        std::memcpy(&entry, buffer.data() + i * kEntrySize, sizeof(entry));
        RawCpuCoreTimes core;
        core.index = static_cast<uint32_t>(i);
        core.idle_ticks = large_integer_to_u64(entry.idle_time);
        // kernel 已包含 idle，故 total = kernel + user（与 GetSystemTimes 同口径）。
        core.total_ticks =
            large_integer_to_u64(entry.kernel_time) + large_integer_to_u64(entry.user_time);
        sample.cores.push_back(core);
        sample.interrupts += static_cast<uint64_t>(entry.interrupt_count);
    }
    return sample;
}

/// 查询全系统累计上下文切换次数；失败返回 std::nullopt。
std::optional<uint64_t> query_context_switches() {
    const auto nt_query = load_nt_query_system_information();
    if (nt_query == nullptr) {
        return std::nullopt;
    }
    NtSystemPerformanceInformation info{};
    ULONG returned_length = 0;
    const LONG status = nt_query(kSystemPerformanceInformation, &info,
                                 static_cast<ULONG>(sizeof(info)), &returned_length);
    if (status < 0) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(info.context_switches);
}

/// 采集一次 CPU 累计计数器（全局 + 每核 + 上下文切换/中断）。
///
/// 全局时间走 GetSystemTimes；每核时间与中断走 NtQuerySystemInformation
/// (SystemProcessorPerformanceInformation)；上下文切换走同 API 的
/// (SystemPerformanceInformation)。任一来源失败只降级对应字段，不影响其它字段。
std::optional<RawCpuTimes> query_cpu_times() {
    FILETIME idle_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (GetSystemTimes(&idle_time, &kernel_time, &user_time) == 0) {
        return std::nullopt;
    }
    RawCpuTimes times;
    // kernel 已包含 idle，故 total = kernel + user；idle 单独返回（契约 sysmon §2.3）。
    times.total_ticks = filetime_to_u64(kernel_time) + filetime_to_u64(user_time);
    times.idle_ticks = filetime_to_u64(idle_time);

    const uint32_t logical_cores = query_logical_cores();
    if (auto per_core = query_per_core_times(logical_cores)) {
        // 契约要求每核序列数与逻辑核数一致；处理器组限制导致不一致时每核/中断不可用
        // （不得用组内数据冒充全系统每核序列）。
        if (per_core->cores.size() == static_cast<std::size_t>(logical_cores)) {
            times.cores = std::move(per_core->cores);
            times.interrupts = per_core->interrupts;
        }
    }
    times.context_switches = query_context_switches();
    return times;
}

// ===== 系统负载：处理器队列长度代理（契约 sysmon §2.3）=====

/// Windows 无原生 load average。按参考实现的规格，以 PDH 的处理器队列长度
/// （`\System\Processor Queue Length`）为瞬时负载，并像 Unix 一样做指数加权移动平均，
/// 得到 1/5/15 分钟代理值，语义标识 `processor_queue`。
///
/// 与 Unix load average **不是同一个量**：口径差异由快照的
/// `availability.semantic_hints["cpu.load_average"]` 承载，前端不得同轴比较。
class WindowsLoadAverageSampler {
public:
    WindowsLoadAverageSampler() = default;
    WindowsLoadAverageSampler(const WindowsLoadAverageSampler&) = delete;
    WindowsLoadAverageSampler& operator=(const WindowsLoadAverageSampler&) = delete;
    WindowsLoadAverageSampler(WindowsLoadAverageSampler&&) = delete;
    WindowsLoadAverageSampler& operator=(WindowsLoadAverageSampler&&) = delete;

    ~WindowsLoadAverageSampler() { close(); }

    /// 采样一次：读取当前处理器队列长度并推进指数加权平均。
    /// @return 采样成功返回代理负载；本次读不到返回 std::nullopt（不沿用旧值，契约 sysmon §2.7）。
    std::optional<RawLoadAverage> sample() {
        const auto queue_length = read_queue_length();
        const auto now = std::chrono::steady_clock::now();
        if (!queue_length.has_value()) {
            // 本帧读不到：返回不可用，由上层置 null（不得沿用旧加权值冒充本帧，契约 sysmon §2.7）。
            return std::nullopt;
        }
        if (!initialized_) {
            one_ = *queue_length;
            five_ = *queue_length;
            fifteen_ = *queue_length;
            initialized_ = true;
        } else {
            const double elapsed_seconds =
                std::chrono::duration<double>(now - last_sample_).count();
            if (elapsed_seconds > 0.0) {
                update(one_, *queue_length, elapsed_seconds, 60.0);
                update(five_, *queue_length, elapsed_seconds, 300.0);
                update(fifteen_, *queue_length, elapsed_seconds, 900.0);
            }
        }
        last_sample_ = now;
        return make_result();
    }

private:
    /// 指数衰减推进：factor = exp(-Δt/窗口)，窗口取 60/300/900 秒（同 Unix 口径的加权形状）。
    static void update(double& average,
                       double current,
                       double elapsed_seconds,
                       double window_seconds) {
        const double factor = std::exp(-elapsed_seconds / window_seconds);
        average = average * factor + current * (1.0 - factor);
    }

    [[nodiscard]] RawLoadAverage make_result() const {
        RawLoadAverage load;
        load.one = one_;
        load.five = five_;
        load.fifteen = fifteen_;
        load.semantic = SYSMON_LOAD_SEMANTIC_PROCESSOR_QUEUE;
        return load;
    }

    /// 读取处理器队列长度；PDH 不可用或计数器无有效数据返回 std::nullopt。
    std::optional<double> read_queue_length() {
        if (!ensure_query()) {
            return std::nullopt;
        }
        if (PdhCollectQueryData(query_) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        PDH_FMT_COUNTERVALUE value{};
        if (PdhGetFormattedCounterValue(counter_, PDH_FMT_DOUBLE, nullptr, &value) !=
            ERROR_SUCCESS) {
            return std::nullopt;
        }
        if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) {
            return std::nullopt;
        }
        if (!std::isfinite(value.doubleValue) || value.doubleValue < 0.0) {
            return std::nullopt;
        }
        return value.doubleValue;
    }

    /// 惰性初始化 PDH 查询；失败后不再重试（避免每次采样重复报错开销）。
    bool ensure_query() {
        if (pdh_failed_) {
            return false;
        }
        if (query_ != nullptr) {
            return true;
        }
        PDH_HQUERY query = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            pdh_failed_ = true;
            return false;
        }
        PDH_HCOUNTER counter = nullptr;
        // 英文计数器路径 + PdhAddEnglishCounterW：本地化系统上也可用（Vista+）。
        if (PdhAddEnglishCounterW(query, L"\\System\\Processor Queue Length", 0, &counter) !=
            ERROR_SUCCESS) {
            PdhCloseQuery(query);
            pdh_failed_ = true;
            return false;
        }
        query_ = query;
        counter_ = counter;
        return true;
    }

    void close() {
        if (query_ != nullptr) {
            PdhCloseQuery(query_);
            query_ = nullptr;
            counter_ = nullptr;
        }
    }

    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
    bool pdh_failed_ = false;
    bool initialized_ = false;
    std::chrono::steady_clock::time_point last_sample_{};
    double one_ = 0.0;
    double five_ = 0.0;
    double fifteen_ = 0.0;
};

/// Windows 采集器：系统信息每次现取；负载代理持有 PDH 查询与加权状态。
class WindowsSystemCollector final : public SystemCollector {
public:
    RawSystemInfo collect_system_info() override { return collect_system_info_windows(); }

    std::optional<RawCpuTimes> collect_cpu_times() override { return query_cpu_times(); }

    std::optional<RawLoadAverage> collect_load_average() override { return load_sampler_.sample(); }

    /// Windows 无可靠的跨硬件温度 API：WMI MSAcpi_ThermalZoneTemperature 依赖固件实现，
    /// 台式机常缺失、返回值也常不可信，本版本不提供温度采集（契约 sysmon §2.4：
    /// 本平台不支持时返回 std::nullopt，上层按可用性映射标记不可用）。
    std::optional<std::vector<RawTemperature>> collect_temperatures() override {
        return std::nullopt;
    }

    /// Windows 无公开的逐核当前频率 API，本版本也不提供整体频率（若后续提供必须标注
    /// 口径）；每核频率报不可用（契约 sysmon §2.3：本平台不支持时返回 std::nullopt）。
    std::optional<std::vector<RawCoreFrequency>> collect_core_frequencies() override {
        return std::nullopt;
    }

private:
    WindowsLoadAverageSampler load_sampler_;
};

}  // namespace

std::unique_ptr<SystemCollector> make_system_collector() {
    return std::make_unique<WindowsSystemCollector>();
}

}  // namespace dztrader::platform

#endif  // defined(_WIN32)
