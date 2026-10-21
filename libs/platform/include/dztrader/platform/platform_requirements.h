#ifndef DZTRADER_PLATFORM_PLATFORM_REQUIREMENTS_H_
#define DZTRADER_PLATFORM_PLATFORM_REQUIREMENTS_H_

/**
 * @file platform_requirements.h
 * @brief dzsysmon 平台基线：编译期与启动期双重检查（契约 sysmon）。
 *
 * 基线（#41 spec「平台基线」）：
 * - Windows：NT 10.0 及以上（Windows 10 / Server 2016；Server 2016 = build 14393，
 *   Windows 10 首个版本 = build 10240），x86_64。版本探测必须走不受 manifest 影响的
 *   途径（实现侧用 ntdll!RtlGetVersion，禁止 GetVersionEx / VerifyVersionInfo / IsWindows*）。
 * - Linux：x86_64；内核与 glibc 下限由既有工具链推导（GCC 13 / C++20），
 *   即 RHEL 系 8+ / Debian 系 10+ / Ubuntu 20.04+：内核 >= 4.18、glibc >= 2.28。
 *
 * 非上述组合必须 `#error`，禁止静默回退（AGENTS「跨平台」硬门禁）。
 * 启动期检查失败 ⇒ 调用方记录错误并以非 0 退出。
 */

// 标准库先行：glibc 的 __GLIBC__ / __GLIBC_MINOR__ 由 <features.h> 提供（经 libstdc++ 头
// 间接包含），放在平台检查之前才能让下方的 glibc 编译期下限检查真正生效。
#include <optional>
#include <string>

#if !defined(_WIN32) && !defined(__linux__)
#error "dzsysmon: only windows x86_64 and linux x86_64 are supported"
#endif

#if defined(_WIN32)
#if !defined(_WIN64) || (!defined(_M_X64) && !defined(__x86_64__))
#error "dzsysmon: only windows x64 is supported"
#endif
// 注：Windows 的 NT 10.0 运行期基线无法在编译期可靠断言——构建侧 `_WIN32_WINNT` /
// `WINVER` 是 API 可用性声明（本项目按最低 API 0x0602 构建），不是运行期版本判据；
// 且 windows.h 会把 WINVER 定义为该值。因此 Windows 侧编译期只锁定 64 位目标，
// 真实版本基线由 `check_platform_baseline()` 在启动期用 RtlGetVersion 检查。
#endif

#if defined(__linux__)
#if !defined(__x86_64__)
#error "dzsysmon: linux x86_64 only"
#endif
#if !defined(__GLIBC__)
#error "dzsysmon: glibc based linux is required (musl is not supported)"
#endif
#if defined(__GNUC__) && !defined(__clang__) && (__GNUC__ < 13)
#error "dzsysmon: gcc 13 or later is required (project toolchain baseline)"
#endif
#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 28))
#error "dzsysmon: glibc 2.28 or later is required (rhel 8 / debian 10 / ubuntu 20.04 baseline)"
#endif
#endif

namespace dztrader::platform {

/// 启动期平台基线检查（与头文件编译期检查同源）。
/// @return 通过返回 std::nullopt；不满足基线返回非空错误消息（英文、小写开头、无句号）。
std::optional<std::string> check_platform_baseline();

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_PLATFORM_REQUIREMENTS_H_
