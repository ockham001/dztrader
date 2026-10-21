/**
 * @file system_collector_windows.cpp
 * @brief Windows 系统指标采集实现（契约 sysmon）。
 *
 * 仅当编译目标为 Windows 时编译；其他平台产生空翻译单元（Linux 实现见
 * system_collector_linux.cpp，工厂函数每平台只定义一次）。
 *
 * 版本探测必须走不受 manifest 影响的 ntdll!RtlGetVersion（契约 sysmon §2.2 与
 * platform_requirements.h 明文禁止 GetVersionEx / VerifyVersionInfo / IsWindows*）。
 *
 * 规格与算法参考 psutil（BSD-3-Clause）：仅借鉴 OS API 选择、边界处理语义与测试不变量，
 * 不翻译其代码、不引入依赖；归属声明见仓库根 THIRD_PARTY_NOTICES.md。
 *
 * 采集失败一律降级：系统信息返回空串/0，CPU 计数器返回 std::nullopt，不抛异常
 * （契约 sysmon §2.7：采集失败跳过本次计量由上层记缺口）。
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
#include <winreg.h>

#include <dztrader/platform/system_collector.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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

/// 采集一次 CPU 累计计数器；GetSystemTimes 失败返回 std::nullopt。
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
    return times;
}

/// Windows 采集器：无状态实现，不缓存（系统信息由调用方在启动时取一次）。
class WindowsSystemCollector final : public SystemCollector {
public:
    RawSystemInfo collect_system_info() override { return collect_system_info_windows(); }

    std::optional<RawCpuTimes> collect_cpu_times() override { return query_cpu_times(); }
};

}  // namespace

std::unique_ptr<SystemCollector> make_system_collector() {
    return std::make_unique<WindowsSystemCollector>();
}

}  // namespace dztrader::platform

#endif  // defined(_WIN32)
