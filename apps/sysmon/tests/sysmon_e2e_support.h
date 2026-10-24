#ifndef DZTRADER_SYSMON_TESTS_SYSMON_E2E_SUPPORT_H_
#define DZTRADER_SYSMON_TESTS_SYSMON_E2E_SUPPORT_H_

/**
 * @file sysmon_e2e_support.h
 * @brief dzsysmon 进程级 e2e 用例的共享夹具（#77/#78/#80）：真实子进程 + 机器全局事件通道。
 *
 * 为什么抽成共享头：三个 e2e（network_config / history / alert）需要同一套
 * 「信号量占用检测 + 子进程路径定位 + 临时 DZTRADER_HOME 随机后缀 + 命名信号量清理」。
 * 按 unit-test-standard §5.2，重复的同款 fixture 必须收敛或注明理由；这些函数是测试环境的
 * 机械约定（不是某个用例的业务夹具），收敛到一处后各用例只保留自己的业务 Setup/TearDown。
 *
 * 头文件只依赖平台/公共库，不含 gtest：调用方负责把 `GTEST_SKIP` / `FAIL` 的语义写在用例里。
 */

#include <dztrader/core/env.h>
#include <dztrader/core/this_process.h>
#include <dztrader/shm/named_semaphore.h>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace dztrader::sysmon::e2e_support {

/// 本机不满足 e2e 前置条件时默认 GTEST_SKIP；CI/需要硬验证时置 DZ_SYSMON_E2E_STRICT=1 改为 FAIL。
inline bool e2e_strict() { return dztrader::env::get("DZ_SYSMON_E2E_STRICT") == "1"; }

/// 事件通道信号量是否已被占用（机器全局命名空间；dev 栈/并行测试运行时会互相唤醒错帧）。
inline bool event_semaphore_busy() {
#ifdef _WIN32
    HANDLE handle = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, "dz.sem.dzevent");
    if (handle != nullptr) {
        CloseHandle(handle);
        return true;
    }
    return false;
#else
    // POSIX 命名信号量落在 /dev/shm，glibc 的文件名为 `sem.<name>`，而本仓 name 已带
    // `dz.sem.` 前缀 → 最终是 `/dev/shm/sem.dz.sem.<name>`。
    std::error_code ec;
    return std::filesystem::exists("/dev/shm/sem.dz.sem.dzevent", ec);
#endif
}

/// 清理用例创建的命名信号量（POSIX 需显式 sem_unlink；Windows 句柄关闭即回收）。
inline void remove_semaphore(const std::string& name) {
    std::error_code ec;
#ifdef _WIN32
    shm::NamedSemaphore::remove(name);
#else
    std::filesystem::remove("/dev/shm/sem.dz.sem." + name, ec);
#endif
}

/// 临时目录/reader 名的随机后缀：同机并行运行不撞名（pid + random）。
inline std::string unique_suffix() {
    std::random_device rd;
    return std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
           std::to_string(rd());
}

/// 真实 dzsysmon 可执行文件：与测试二进制同目录的**上一级**（DZ_OUTPUT_DIR）。
/// 测试二进制在 DZ_TESTS_DIR（= <build>/tests），dzsysmon 在 <build>/。
inline std::filesystem::path sysmon_executable() {
    auto exe = dztrader::this_process::exe_dir().parent_path() / "dzsysmon";
#ifdef _WIN32
    exe += ".exe";
#endif
    return exe;
}

}  // namespace dztrader::sysmon::e2e_support

#endif  // DZTRADER_SYSMON_TESTS_SYSMON_E2E_SUPPORT_H_
