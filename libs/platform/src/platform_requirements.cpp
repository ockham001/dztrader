/**
 * @file platform_requirements.cpp
 * @brief dzsysmon 启动期平台基线检查实现（契约 sysmon）。
 *
 * 编译期检查见 platform_requirements.h 的 `#error` 门禁；本文件实现同源的运行期检查：
 * - Windows：ntdll!RtlGetVersion（不受 manifest 兼容性垫片影响）判断 NT 10.0 + build 10240；
 * - Linux：uname 判断 x86_64 与内核 >= 4.18，gnu_get_libc_version 判断 glibc >= 2.28。
 *
 * 失败返回英文错误消息（小写开头、无句号），成功返回 std::nullopt。
 */

#include <dztrader/platform/platform_requirements.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
// 接口头保持在 windows.h 之前：Windows 编译期只锁定 64 位目标（NT 版本基线由下方
// RtlGetVersion 运行期检查承担——构建侧 _WIN32_WINNT 是 API 可用性声明，不能作为
// 运行期版本判据）。由二选一的 #if 保证不会同时包含两平台头。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <gnu/libc-version.h>
#include <sys/utsname.h>
#endif

namespace dztrader::platform {
namespace {

#if defined(_WIN32)

/// NT 10.0（Windows 10 / Server 2016）首个 build 号（契约 sysmon 平台基线）。
constexpr uint32_t kWindowsMinimumMajor = 10;
constexpr uint32_t kWindowsMinimumBuild = 10240;

/// RtlGetVersion 函数签名（NTSTATUS = LONG；成功返回 0 = STATUS_SUCCESS）。
using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

std::optional<std::string> check_windows_baseline() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return std::string("cannot query nt version | reason=ntdll_unavailable");
    }
    const auto rtl_get_version =
        reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl_get_version == nullptr) {
        return std::string("cannot query nt version | reason=rtlgetversion_unavailable");
    }

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = static_cast<DWORD>(sizeof(version));
    const LONG status = rtl_get_version(&version);
    if (status != 0) {
        return "cannot query nt version | status=" + std::to_string(status);
    }

    const uint32_t major = static_cast<uint32_t>(version.dwMajorVersion);
    const uint32_t build = static_cast<uint32_t>(version.dwBuildNumber);
    if (major > kWindowsMinimumMajor ||
        (major == kWindowsMinimumMajor && build >= kWindowsMinimumBuild)) {
        return std::nullopt;
    }
    return "unsupported windows version | required=10.0.10240 actual=" + std::to_string(major) +
           "." + std::to_string(version.dwMinorVersion) + "." + std::to_string(build);
}

#elif defined(__linux__)

/// Linux 基线：内核 >= 4.18，glibc >= 2.28（RHEL 8 / Debian 10 / Ubuntu 20.04）。
constexpr uint32_t kLinuxMinimumKernelMajor = 4;
constexpr uint32_t kLinuxMinimumKernelMinor = 18;
constexpr uint32_t kLinuxMinimumGlibcMajor = 2;
constexpr uint32_t kLinuxMinimumGlibcMinor = 28;

/// 解析无符号十进制整数；空串、负号、非数字、溢出均返回 std::nullopt。
std::optional<uint32_t> parse_u32(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    uint32_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || ptr != last) {
        return std::nullopt;
    }
    return value;
}

/// 解析版本号前两段（"major.minor"，允许后续 ".patch-suffix"）；失败返回 std::nullopt。
std::optional<std::pair<uint32_t, uint32_t>> parse_version_prefix(std::string_view text) {
    const auto first_dot = text.find('.');
    if (first_dot == std::string_view::npos || first_dot == 0) {
        return std::nullopt;
    }
    const auto second_dot = text.find('.', first_dot + 1);
    const auto minor_length =
        second_dot == std::string_view::npos ? std::string_view::npos : second_dot - first_dot - 1;
    const auto major = parse_u32(text.substr(0, first_dot));
    const auto minor = parse_u32(text.substr(first_dot + 1, minor_length));
    if (!major || !minor) {
        return std::nullopt;
    }
    return std::pair<uint32_t, uint32_t>{*major, *minor};
}

/// 版本号（major, minor）是否低于给定下限。
bool below_version(uint32_t major, uint32_t minor, uint32_t minimum_major, uint32_t minimum_minor) {
    return major < minimum_major || (major == minimum_major && minor < minimum_minor);
}

std::optional<std::string> check_linux_baseline() {
    struct utsname uname_info{};
    if (uname(&uname_info) != 0) {
        return std::string("cannot query kernel version | reason=uname_failed");
    }

    const std::string_view machine = uname_info.machine;
    if (machine != "x86_64") {
        return "unsupported architecture | arch=" + std::string(machine);
    }

    const std::string_view release = uname_info.release;
    const auto kernel = parse_version_prefix(release);
    if (!kernel) {
        return "cannot parse kernel version | release=" + std::string(release);
    }
    if (below_version(kernel->first, kernel->second, kLinuxMinimumKernelMajor,
                      kLinuxMinimumKernelMinor)) {
        return "unsupported kernel version | required=4.18 actual=" + std::string(release);
    }

    const char* glibc_version = gnu_get_libc_version();
    if (glibc_version == nullptr) {
        return std::string("cannot query glibc version | reason=null_version");
    }
    const std::string_view glibc = glibc_version;
    const auto glibc_parsed = parse_version_prefix(glibc);
    if (!glibc_parsed) {
        return "cannot parse glibc version | version=" + std::string(glibc);
    }
    if (below_version(glibc_parsed->first, glibc_parsed->second, kLinuxMinimumGlibcMajor,
                      kLinuxMinimumGlibcMinor)) {
        return "unsupported glibc version | required=2.28 actual=" + std::string(glibc);
    }

    return std::nullopt;
}

#endif

}  // namespace

std::optional<std::string> check_platform_baseline() {
#if defined(_WIN32)
    return check_windows_baseline();
#elif defined(__linux__)
    return check_linux_baseline();
#endif
}

}  // namespace dztrader::platform
