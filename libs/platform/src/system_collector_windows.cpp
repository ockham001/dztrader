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
// IP Helper 的用户态包含顺序：winsock2.h → ws2tcpip.h → windows.h → iphlpapi.h。
// - winsock2.h 必须先于 windows.h（否则引入旧的 winsock.h，两套套接字头冲突）；
//   WIN32_LEAN_AND_MEAN 已排除 winsock.h，与 libs/core/src/env.cpp 既有做法一致。
// - ws2tcpip.h 负责引入 ws2ipdef.h：MIB_IF_ROW2 / MIB_IF_TABLE2 / GetIfTable2 的声明
//   依赖它提供的网络接口类型（NET_LUID / NET_IFINDEX / IF_MAX_* / IF_OPER_STATUS）。
//   本 TU 原先只引 winsock2.h，于是 iphlpapi.h 间接拉入 netioapi.h 时 ws2ipdef.h 从未
//   进入包含树，netioapi.h 的接口结构段整体未生效 —— CI Windows job 报
//   `'PMIB_IF_TABLE2': undeclared identifier` / `'GetIfTable2': identifier not found`
//   （失败 TU 的 showIncludes 里确实没有 ws2ipdef.h，同 job 通过编译的其它 TU 有）。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>    // PDH_CSTATUS_VALID_DATA / PDH_CSTATUS_NEW_DATA（部分 SDK 的 pdh.h 不引入）
#include <psapi.h>     // GetPerformanceInfo：内存与页文件口径（契约 sysmon §2.4）
#include <winioctl.h>  // DISK_PERFORMANCE / IOCTL_DISK_PERFORMANCE：每物理盘 I/O（契约 sysmon §2.4）
#include <winreg.h>
// 每网卡累计计数器（契约 sysmon §2.4）：GetIfTable2 / FreeMibTable 与 MIB_IF_TABLE2 /
// MIB_IF_ROW2。**不要直接 include netioapi.h**：用户态下它期望已被 iphlpapi.h 拉起的
// 上下文（NTSTATUS 等来自 winnt.h 的类型），单独引入会在 netioapi.h 内部刷出大量
// `missing ';' before '__cdecl'` / `'NTSTATUS': redefinition`。用户态正确入口只有
// iphlpapi.h（它按需再引 netioapi.h）。链接库 iphlpapi 在 CMakeLists 声明。
#include <iphlpapi.h>

#include "system_collector_internal.h"

#include <dztrader/platform/system_collector.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
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

// ===== 内存与交换：GetPerformanceInfo（契约 sysmon §2.4、§2.7）=====
//
// PERFORMANCE_INFORMATION 的计数字段以页为单位（PageSize 字节/页）。psutil 仅作
// 规格参考（ADR 0024）：Windows 上"空闲"与"可用"是同一个量（PhysicalAvailable），
// 不得改用 Linux 的 MemFree 语义；页文件总量取"由页面文件支撑的提交上限"
// （CommitLimit - PhysicalTotal），不是原始页面文件大小。

/// 读取 PERFORMANCE_INFORMATION；失败返回 std::nullopt（采集失败降级，不抛异常）。
std::optional<PERFORMANCE_INFORMATION> query_performance_information() {
    PERFORMANCE_INFORMATION info{};
    if (GetPerformanceInfo(&info, static_cast<DWORD>(sizeof(info))) == 0) {
        return std::nullopt;
    }
    if (info.PageSize == 0) {
        return std::nullopt;
    }
    return info;
}

/// 采集内存读数：总量/可用取物理内存页数，缓存取 SystemCache（Windows 无 buffers）。
std::optional<RawMemoryInfo> query_memory_info() {
    const auto info = query_performance_information();
    if (!info.has_value() || info->PhysicalTotal == 0) {
        return std::nullopt;
    }
    const uint64_t page_size = static_cast<uint64_t>(info->PageSize);
    const uint64_t total_pages = static_cast<uint64_t>(info->PhysicalTotal);
    RawMemoryInfo memory;
    memory.total_bytes = total_pages * page_size;
    // 可用 = PhysicalAvailable（"空闲"与"可用"同义，契约 sysmon §2.4）。
    const uint64_t available_pages =
        std::min(static_cast<uint64_t>(info->PhysicalAvailable), total_pages);
    memory.available_bytes = available_pages * page_size;
    // 缓存 = SystemCache；buffers 在 Windows 不提供（std::nullopt）。
    memory.cached_bytes =
        std::min(static_cast<uint64_t>(info->SystemCache), total_pages) * page_size;
    return memory;
}

/// 采集页文件口径的交换读数（总量/已用见契约 sysmon §2.4 的 Windows 口径说明）。
std::optional<RawSwapInfo> query_swap_info() {
    const auto info = query_performance_information();
    if (!info.has_value()) {
        return std::nullopt;
    }
    const uint64_t page_size = static_cast<uint64_t>(info->PageSize);
    const uint64_t physical_pages = static_cast<uint64_t>(info->PhysicalTotal);
    const uint64_t commit_limit_pages = static_cast<uint64_t>(info->CommitLimit);
    const uint64_t commit_total_pages = static_cast<uint64_t>(info->CommitTotal);
    // 无页面文件时提交上限不超过物理内存 => total = 0（上层标记 unsupported）。
    const uint64_t total_pages =
        commit_limit_pages > physical_pages ? commit_limit_pages - physical_pages : 0;
    // 已用 = 物理内存之外已提交的量，钳制到 [0, total]（提交量可小于物理内存）。
    const uint64_t used_pages = commit_total_pages > physical_pages
                                    ? std::min(commit_total_pages - physical_pages, total_pages)
                                    : 0;
    RawSwapInfo swap;
    swap.total_bytes = total_pages * page_size;
    swap.free_bytes = (total_pages - used_pages) * page_size;
    return swap;
}

// ===== 卷、关注路径与共享内存目录（契约 sysmon §2.4）=====
//
// 卷枚举走 GetLogicalDrives + GetDriveTypeW（只要 DRIVE_FIXED 固定盘，契约 §2.4）；
// 容量走 GetDiskFreeSpaceExW。容量读不到（无权限/卷不可访问）的卷仍进数组，
// total/free 留 nullopt 并给 reason——"存在但读不到"不得用 0 冒充。
// 关注路径的落卷解析用 GetVolumePathNameW（OS 自己给出的挂载点，天然处理
// 驱动器挂载、目录挂载点与卷 GUID，不让本进程重实现一遍匹配规则）。

/// UTF-8 转宽字符；转换失败返回空串。
std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int input_length = static_cast<int>(text.size());
    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, text.data(), input_length, nullptr, 0);
    if (wide_length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text.data(), input_length, wide.data(), wide_length) <= 0) {
        return {};
    }
    return wide;
}

/// 盘符列表（`C:`、`D:` ...；不含尾部分隔符）。
std::vector<std::string> list_fixed_drives() {
    std::vector<std::string> drives;
    const DWORD mask = GetLogicalDrives();
    if (mask == 0) {
        return drives;
    }
    for (int index = 0; index < 26; ++index) {
        if ((mask & (1UL << static_cast<unsigned>(index))) == 0) {
            continue;
        }
        const char letter = static_cast<char>('A' + index);
        const std::wstring root = std::wstring(1, static_cast<wchar_t>(letter)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
            continue;  // 只覆盖固定盘：可移动/网络/光驱不是容量基线（契约 sysmon §2.4）
        }
        drives.push_back(std::string(1, letter) + ":");
    }
    return drives;
}

/// 递归统计目录占用（契约 sysmon §2.4）：总字节数、页文件数、最旧页文件年龄。
std::optional<RawShmDirInfo> query_shm_dir(const std::string& shm_dir) {
    const auto wide_dir = utf8_to_wide(shm_dir);
    if (wide_dir.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    const std::filesystem::path root(wide_dir);
    if (!std::filesystem::is_directory(root, ec) || ec) {
        return std::nullopt;  // 目录不存在/不可读：不得输出全 0 的对象
    }
    // `is_directory()` 对无读取权限的目录仍为 true，而下面的递归迭代带
    // skip_permission_denied 会静默产出 0 个条目 + ec=0 —— 那会把"无权限"谎报成
    // "目录为空"的全 0 对象（契约 sysmon §2.4 禁止）。先探一次可读性。
    {
        std::error_code probe_ec;
        const std::filesystem::directory_iterator probe(root, probe_ec);
        if (probe_ec) {
            return std::nullopt;
        }
    }
    // 不需要深度上限：递归迭代器默认不跟随目录符号链接（无环），shm 树只有两层。
    RawShmDirInfo info;
    info.path = shm_dir;
    const auto now = std::chrono::file_clock::now();
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec) && !entry_ec) {
            const auto size = static_cast<uint64_t>(it->file_size(entry_ec));
            if (!entry_ec) {
                info.total_bytes += size;
            }
            const auto narrow = wide_to_utf8(it->path().filename().wstring());
            if (system_collector_internal::is_shm_page_file_name(narrow)) {
                ++info.page_files;
                const auto write_time = it->last_write_time(entry_ec);
                if (!entry_ec) {
                    const auto age =
                        std::chrono::duration_cast<std::chrono::seconds>(now - write_time);
                    // 未来时间（时钟回拨/异常 mtime）按 0 处理，不给负年龄。
                    const int64_t age_sec = age.count() > 0 ? age.count() : 0;
                    if (!info.oldest_file_age_sec.has_value() ||
                        age_sec > *info.oldest_file_age_sec) {
                        info.oldest_file_age_sec = age_sec;
                    }
                }
            }
        }
        it.increment(ec);
    }
    return info;
}

/// 规范化路径为绝对路径（`GetFullPathNameW`：解析 `.`/`..`、统一分隔符为 `\`）。
/// @return 规范化后的 UTF-8 路径；失败返回 std::nullopt。
std::optional<std::string> absolute_path(const std::string& path) {
    const auto wide_path = utf8_to_wide(path);
    if (wide_path.empty()) {
        return std::nullopt;
    }
    const DWORD needed = GetFullPathNameW(wide_path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return std::nullopt;
    }
    std::wstring absolute(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = GetFullPathNameW(wide_path.c_str(), needed, absolute.data(), nullptr);
    if (written == 0 || written >= needed) {
        return std::nullopt;
    }
    absolute.resize(written);
    return wide_to_utf8(absolute);
}

/// 取挂载点所属的盘符根（`C:\mnt\vol2` → `C:\`；`C:` → `C:\`）。
/// UNC（`\\server\share`）与卷 GUID 路径没有盘符根，返回空串（调用方判为不在范围）。
std::string drive_root_of(const std::string& mount) {
    if (mount.size() < 2 || mount[1] != ':') {
        return {};
    }
    return mount.substr(0, 2) + "\\";
}

/// 用 GetVolumePathNameW 把路径解析到所在卷的挂载点（契约 sysmon §2.4）。
/// @return 挂载点（带尾部分隔符的盘符根或目录挂载点）；失败返回 std::nullopt。
std::optional<std::string> resolve_volume_mount(const std::string& path) {
    // GetVolumePathNameW 要求绝对路径（调用方已用 absolute_path() 规范化）。
    const auto absolute_path_string = absolute_path(path);
    if (!absolute_path_string.has_value()) {
        return std::nullopt;
    }
    const std::wstring absolute = utf8_to_wide(*absolute_path_string);
    if (absolute.empty()) {
        return std::nullopt;
    }

    // 挂载点缓冲区按 MAX_PATH 起步，长度不足时按 API 约定扩展。
    std::wstring volume_path(MAX_PATH, L'\0');
    while (true) {
        if (GetVolumePathNameW(absolute.c_str(), volume_path.data(),
                               static_cast<DWORD>(volume_path.size())) != 0) {
            break;
        }
        if (GetLastError() != ERROR_FILENAME_EXCED_RANGE) {
            return std::nullopt;
        }
        volume_path.resize(volume_path.size() * 2);
        if (volume_path.size() > 32 * 1024) {
            return std::nullopt;  // 病态长路径：放弃解析（调用方报 collect_failed）
        }
    }
    std::wstring resolved = std::wstring(volume_path.c_str());
    // 盘符统一成大写：query_volumes() 的挂载点由 'A'+index 生成（恒大写），而
    // GetVolumePathNameW 会把调用方输入的大小写原样回显（`c:\` 得 `c:\`）。
    // 契约 sysmon §2.4 要求 Windows 匹配大小写不敏感，消费方（快照构造的 watched
    // 判定、dzweb 的 §7 交叉校验）用字符串相等比较，所以这里必须归一化，
    // 否则小写盘符的关注路径永远对不上卷、整帧还会被交叉校验拒掉。
    if (resolved.size() >= 2 && resolved[1] == L':') {
        resolved[0] = static_cast<wchar_t>(std::towupper(resolved[0]));
    }
    return wide_to_utf8(resolved);
}

/// 采集卷列表与容量（契约 sysmon §2.4）；一个固定盘都没有时返回 std::nullopt。
std::optional<std::vector<RawVolumeInfo>> query_volumes() {
    const auto drives = list_fixed_drives();
    if (drives.empty()) {
        return std::nullopt;
    }
    std::vector<RawVolumeInfo> volumes;
    volumes.reserve(drives.size());
    for (const auto& drive : drives) {
        RawVolumeInfo volume;
        // 挂载点统一为带尾部分隔符的盘符根（`C:\\`），与 GetVolumePathNameW 返回口径一致。
        volume.mount = drive + "\\";
        const std::wstring wide_root = utf8_to_wide(volume.mount);
        ULARGE_INTEGER free_to_caller{};
        ULARGE_INTEGER total{};
        ULARGE_INTEGER free_total{};
        if (GetDiskFreeSpaceExW(wide_root.c_str(), &free_to_caller, &total, &free_total) != 0) {
            volume.total_bytes = total.QuadPart;
            volume.free_bytes = std::min(free_to_caller.QuadPart, total.QuadPart);
        } else {
            const DWORD error = GetLastError();
            volume.reason = (error == ERROR_ACCESS_DENIED) ? "no_permission" : "collect_failed";
        }
        volumes.push_back(std::move(volume));
    }
    return volumes;
}

// ===== 每物理盘 I/O：IOCTL_DISK_PERFORMANCE（契约 sysmon §2.4、§2.7）=====
//
// 每盘累计计数器来自 IOCTL_DISK_PERFORMANCE（输出 DISK_PERFORMANCE）。该能力依赖
// 性能计数器已使能（diskperf）与访问权限，不可用是常态而非异常：不存在的盘号、
// 不支持计数器的盘（ERROR_INVALID_FUNCTION / ERROR_NOT_SUPPORTED）、无权限或其它
// 读取失败的盘都只跳过该盘，不伪造 0。物理盘编号不保证连续，按参考实现的做法
// 遍历 `\\.\PhysicalDrive0` .. `\\.\PhysicalDrive32`，打不开即视为该编号不存在。

/// 物理盘枚举上限（编号不保证连续，取 0..32 覆盖常见配置；与参考实现一致）。
constexpr int kPhysicalDriveMaxIndex = 32;

/// IOCTL_DISK_PERFORMANCE 输出缓冲区上限：驱动可能返回比公开结构更新的更大结构，
/// 缓冲区不足时翻倍重试；上限防止病态驱动无限增长。
constexpr std::size_t kMaxDiskPerformanceBufferSize = 1024 * 1024;

/// DISK_PERFORMANCE 的 100ns 计时器转微秒（时间字段统一为微秒，契约 sysmon §2.4）。
constexpr uint64_t kDiskPerfTicksPerUs = 10;

/// 100ns 数转微秒（整除，向下取整）。
uint64_t diskperf_ticks_to_us(uint64_t ticks_100ns) { return ticks_100ns / kDiskPerfTicksPerUs; }

/// 读取单个物理盘的 DISK_PERFORMANCE；该盘不存在/不支持/读不到时返回 std::nullopt
/// （调用方跳过该盘，不记错误——Windows 计数器不可用是常态）。
std::optional<DISK_PERFORMANCE> query_disk_performance(int index) {
    const std::wstring device_path = L"\\\\.\\PhysicalDrive" + std::to_wstring(index);
    HANDLE device = CreateFileW(device_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        return std::nullopt;  // 该编号物理盘不存在（正常，跳过，不记错误）
    }
    std::vector<std::byte> buffer(sizeof(DISK_PERFORMANCE));
    DWORD returned = 0;
    bool ioctl_ok = false;
    while (true) {
        if (DeviceIoControl(device, IOCTL_DISK_PERFORMANCE, nullptr, 0, buffer.data(),
                            static_cast<DWORD>(buffer.size()), &returned, nullptr) != 0) {
            ioctl_ok = true;
            break;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
            buffer.size() >= kMaxDiskPerformanceBufferSize) {
            // ERROR_INVALID_FUNCTION / ERROR_NOT_SUPPORTED = 该盘不支持性能计数器；
            // ERROR_ACCESS_DENIED 与其它错误同理：跳过该盘，不伪造 0。
            break;
        }
        buffer.resize(std::min(buffer.size() * 2, kMaxDiskPerformanceBufferSize));
    }
    CloseHandle(device);
    if (!ioctl_ok || static_cast<std::size_t>(returned) < sizeof(DISK_PERFORMANCE)) {
        return std::nullopt;
    }
    // 缓冲区可能大于公开结构（驱动返回新版结构）：只取公开结构覆盖的前缀。
    DISK_PERFORMANCE performance{};
    std::memcpy(&performance, buffer.data(), sizeof(performance));
    // QueryTime 是每次查询都会写入的系统时间锚点；为 0 说明计数器未填充（diskperf 关闭或
    // 驱动不支持），此时结构体里其余字段的全 0 不是"读到了 0"，必须报不可用（契约 §2.4）。
    if (performance.QueryTime.QuadPart <= 0) {
        return std::nullopt;
    }
    return performance;
}

/// 采集每物理盘 I/O 累计计数器（契约 sysmon §2.4、§2.7）。
///
/// 一个盘都读不到 ⇒ `unavailable_reason = no_permission`（Windows 计数器需特权/未使能，
/// 上层直接映射为可用性原因）；至少一个成功 ⇒ 按盘号升序返回非空数组、原因为空串。
/// 单个盘读到 0 是"读到了 0"，与"读不到"分开：只有成功返回的盘才进入数组。
RawDiskIoSnapshot query_disk_io() {
    RawDiskIoSnapshot snapshot;
    for (int index = 0; index <= kPhysicalDriveMaxIndex; ++index) {
        const auto performance = query_disk_performance(index);
        if (!performance.has_value()) {
            continue;
        }
        RawDiskIoInfo disk;
        disk.device = "PhysicalDrive" + std::to_string(index);
        disk.read_count = static_cast<uint64_t>(performance->ReadCount);
        disk.write_count = static_cast<uint64_t>(performance->WriteCount);
        disk.read_bytes = large_integer_to_u64(performance->BytesRead);
        disk.write_bytes = large_integer_to_u64(performance->BytesWritten);
        disk.read_time_us = diskperf_ticks_to_us(large_integer_to_u64(performance->ReadTime));
        disk.write_time_us = diskperf_ticks_to_us(large_integer_to_u64(performance->WriteTime));
        // QueryTime 是本次查询的系统时间锚点、IdleTime 是累计空闲时间（同一 100ns 时间源）：
        // 二者差值才是累计设备忙时间（至少一个 I/O 在处理的墙钟时间）。锚点小于空闲时间
        // 属计数器重置/口径异常，本次留 std::nullopt，不输出回绕后的假值（契约 sysmon §2.7）。
        if (performance->QueryTime.QuadPart >= performance->IdleTime.QuadPart) {
            disk.busy_time_us = diskperf_ticks_to_us(static_cast<uint64_t>(
                performance->QueryTime.QuadPart - performance->IdleTime.QuadPart));
        }
        // QueueDepth 是采样时刻的在途请求数（瞬时 gauge，契约 sysmon §2.4）。
        disk.in_flight = static_cast<uint64_t>(performance->QueueDepth);
        // Windows：ReadCount/WriteCount 是 32 位 DWORD（按 2^32 跨度处理回绕）；
        // 字节与 100ns 时间计数器是 64 位（下降一律视为重置，契约 sysmon §2.7）。
        disk.count_wrap_span = DISK_IO_COUNT_WRAP_SPAN_32;
        disk.time_wrap_span = 0;
        snapshot.disks.push_back(std::move(disk));
    }
    if (snapshot.disks.empty()) {
        // Windows 上无法区分"本机没有物理盘"与"全部无权限/计数器未开启"（CreateFile 失败
        // 对两者都返回 INVALID_HANDLE_VALUE）；按契约以 no_permission（计数器需特权/未开启）
        // 表示不可用，绝不返回空数组伪装成功（契约 sysmon §2.4）。
        snapshot.unavailable_reason = "no_permission";
    }
    return snapshot;
}

// 编译期断言：IP Helper 的接口结构体确实可见。若头文件包含顺序回归（例如有人删掉
// ws2tcpip.h），编译器会先在这里给出可读的失败原因，而不是在 query_network 里刷出
// 二十多条级联的 undeclared identifier。
static_assert(sizeof(MIB_IF_ROW2) > 0,
              "MIB_IF_ROW2 must be visible: include winsock2.h, "
              "ws2tcpip.h, windows.h, netioapi.h in that order");
static_assert(sizeof(MIB_IF_TABLE2) > 0, "MIB_IF_TABLE2 must be visible: see iphlpapi/netioapi");

// ===== 每网卡累计计数器：GetIfTable2 / FreeMibTable（契约 sysmon §2.4、§2.7）=====
//
// 数据源是 IP Helper API 的 `GetIfTable2`（输出 `MIB_IF_TABLE2`，逐行 `MIB_IF_ROW2`）。
// 相比老接口 `GetIfTable`/`GetIfEntry`，它按接口 LUID 枚举、计数为 `ULONG64`，并带
// `InterfaceAndOperStatusFlags.HardwareInterface`——正是契约要求的**结构性**虚拟网卡判据
// （不看名字前缀）。`MIB_IF_ROW2.Alias` 是用户可见适配器名（`WCHAR[IF_MAX_STRING_SIZE+1]`），
// 契约规定用它作 `interface`。

/// 采集**全部**网络接口的原始累计计数器（契约 sysmon §2.4、§2.7）。
///
/// 包含虚拟网卡与回环（`is_virtual` 标明）：过滤发生在快照构造阶段，改
/// `network.hide_virtual` 不需要重新采集、也不影响速率基线。
///
/// `GetIfTable2` 失败 ⇒ 失败（`unavailable_reason=collect_failed`）；成功但没有接口
/// ⇒ 成功 + 空数组（本机无网络接口不是失败）。0 流量的接口照常进入数组
/// （“存在但为 0”与“不存在”是不同事实）。找不到别名的接口跳过（不伪造空名条目）。
RawNetworkSnapshot query_network() {
    RawNetworkSnapshot snapshot;
    PMIB_IF_TABLE2 table = nullptr;
    const DWORD status = GetIfTable2(&table);
    if (status != NO_ERROR || table == nullptr) {
        snapshot.unavailable_reason = "collect_failed";
        return snapshot;
    }
    snapshot.interfaces.reserve(table->NumEntries);
    // 帧内 interface 必须唯一（契约 sysmon §7）：`Alias` 由用户命名，理论上可以重名
    // （同一显示名绑到多个适配器），重名时**保留首次出现的条目**——与 Linux 侧
    // `parse_net_dev` 对同名接口行的处理同口径。不这么做会产出自家 validate 判非法的帧。
    std::set<std::string> seen_aliases;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        RawNetworkInfo info;
        // Alias 是 `WCHAR[IF_MAX_STRING_SIZE + 1]`；按缓冲上界封顶扫描取长度，
        // 防御异常驱动不写 NUL 终止符（不读越界）。用 sizeof 推导上界而不直接用
        // IF_MAX_STRING_SIZE 宏，避免无符号/有符号比较告警（MSVC /W4 /WX）。
        constexpr std::size_t kAliasCapacity = sizeof(row.Alias) / sizeof(row.Alias[0]);
        std::size_t alias_length = 0;
        while (alias_length < kAliasCapacity && row.Alias[alias_length] != L'\0') {
            ++alias_length;
        }
        info.interface_name = wide_to_utf8(std::wstring_view(row.Alias, alias_length));
        if (info.interface_name.empty()) {
            continue;  // 无别名的接口无法作为帧内唯一键，跳过（不伪造空名条目）
        }
        if (!seen_aliases.insert(info.interface_name).second) {
            continue;  // 别名重名：保留首次出现的适配器（帧内 interface 必须唯一）
        }
        // 八个累计计数器两平台均为 64 位（契约 sysmon §2.7）：MIB_IF_ROW2 的字段是 ULONG64，
        // 显式 static_cast 消除 /W4 下的收窄告警（MSVC /WX）。
        info.rx_bytes = static_cast<uint64_t>(row.InOctets);
        info.tx_bytes = static_cast<uint64_t>(row.OutOctets);
        // 契约要求包数含单播与组播/广播：Windows 把它们分成 Ucast 与 NUcast 两个计数器。
        info.rx_packets =
            static_cast<uint64_t>(row.InUcastPkts) + static_cast<uint64_t>(row.InNUcastPkts);
        info.tx_packets =
            static_cast<uint64_t>(row.OutUcastPkts) + static_cast<uint64_t>(row.OutNUcastPkts);
        info.rx_errors = static_cast<uint64_t>(row.InErrors);
        info.tx_errors = static_cast<uint64_t>(row.OutErrors);
        info.rx_drops = static_cast<uint64_t>(row.InDiscards);
        info.tx_drops = static_cast<uint64_t>(row.OutDiscards);
        // 结构性判定：没有硬件设备支撑的接口（虚拟网卡/回环/隧道）HardwareInterface 为 false。
        info.is_virtual = row.InterfaceAndOperStatusFlags.HardwareInterface == 0;
        snapshot.interfaces.push_back(std::move(info));
    }
    // 表由 API 分配，必须用 FreeMibTable 释放（早退路径均在释放之后）。
    FreeMibTable(table);
    return snapshot;
}

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

    std::optional<RawMemoryInfo> collect_memory() override { return query_memory_info(); }

    std::optional<RawSwapInfo> collect_swap() override { return query_swap_info(); }

    std::optional<std::vector<RawVolumeInfo>> collect_volumes() override { return query_volumes(); }

    std::vector<RawWatchPathInfo> collect_watch_paths(
        const std::vector<std::string>& paths) override {
        std::vector<RawWatchPathInfo> results;
        results.reserve(paths.size());
        for (const auto& path : paths) {
            RawWatchPathInfo info;
            info.path = path;
            // 路径不存在 ⇒ 如实报 path_not_found（GetVolumePathNameW 对不存在的尾部
            // 仍可能返回父卷，不能据此报成功，契约 sysmon §2.4）。
            const auto absolute = absolute_path(path);
            if (!absolute.has_value()) {
                info.reason = "path_not_found";
                results.push_back(std::move(info));
                continue;
            }
            // 契约 sysmon §2.4：path 是"解析用的路径（绝对路径）"，上报规范化结果而不是
            // 调用方原始输入（分隔符已统一为 `\`、`.`/`..` 已解析）。
            info.path = *absolute;
            std::error_code exists_ec;
            const bool exists =
                std::filesystem::exists(std::filesystem::path(utf8_to_wide(*absolute)), exists_ec);
            if (!exists || exists_ec) {
                info.reason = "path_not_found";
                results.push_back(std::move(info));
                continue;
            }
            const auto mount = resolve_volume_mount(*absolute);
            if (!mount.has_value()) {
                // 路径存在但解析不到挂载点（极端长路径/卷服务异常）：不猜卷（契约 sysmon §2.4）。
                info.reason = "collect_failed";
                results.push_back(std::move(info));
                continue;
            }
            // 卷容量只覆盖固定盘根（契约 sysmon §2.4）：落卷落在网络盘/可移动盘/UNC 上时
            // 该挂载点不会出现在 volumes[] 里，照实报 mount 会让整帧交叉校验失败。
            // 契约要求此类报 unsupported（"卷不在采集范围"），mount 留 null。
            //
            // 注意**不能**用"是否是固定盘"作为唯一判据：NTFS 支持把一个卷挂载成目录
            // （`C:\mnt\vol2`）。这类路径的挂载点在 C: 上、GetDriveType 也是 FIXED，
            // 但 `C:\mnt\vol2` 不等于 volumes[] 里的 `C:\`，同样会打红交叉校验。
            // 判据统一为：挂载点的盘符根 == 该盘根的规范化形式（即挂载点就是卷根）。
            const auto mount_root = drive_root_of(*mount);
            const std::wstring wide_mount_root = utf8_to_wide(mount_root);
            const bool in_scope =
                wide_mount_root.size() >= 3 &&
                GetDriveTypeW(wide_mount_root.substr(0, 3).c_str()) == DRIVE_FIXED &&
                *mount == mount_root;
            if (!in_scope) {
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
        return query_shm_dir(shm_dir);
    }

    RawDiskIoSnapshot collect_disk_io() override { return query_disk_io(); }

    RawNetworkSnapshot collect_network() override { return query_network(); }

private:
    WindowsLoadAverageSampler load_sampler_;
};

}  // namespace

std::unique_ptr<SystemCollector> make_system_collector() {
    return std::make_unique<WindowsSystemCollector>();
}

}  // namespace dztrader::platform

#endif  // defined(_WIN32)
