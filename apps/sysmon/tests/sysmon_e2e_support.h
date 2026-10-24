#ifndef DZTRADER_SYSMON_TESTS_SYSMON_E2E_SUPPORT_H_
#define DZTRADER_SYSMON_TESTS_SYSMON_E2E_SUPPORT_H_

/**
 * @file sysmon_e2e_support.h
 * @brief dzsysmon 进程级 e2e 用例的共享机械约定（#77/#78/#80）：真实子进程 + 机器全局事件通道。
 *
 * 收敛范围（unit-test-standard §5.2）：信号量占用检测、子进程路径定位/启停、临时
 * `DZTRADER_HOME` 随机后缀、命名信号量清理——这些是**所有** dzsysmon e2e 相同的环境约定。
 * 各用例仍各自持有其配置内容、事件通道尺寸与帧序断言：三者（network/history/alert）的配置、
 * 等待窗口与断言差异大，进一步抽 base fixture 需要把这些差异参数化，属于测试基建改动；
 * 本票只收敛逐字复制的部分，其余保留在各用例内并在文件头说明。
 *
 * 头文件不含 gtest：`GTEST_SKIP` / `FAIL` 的语义由调用方决定。
 */

#include <dztrader/core/env.h>
#include <dztrader/core/this_process.h>
#include <dztrader/shm/named_semaphore.h>

#include <boost/asio/io_context.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process/v2/default_launcher.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace dztrader::sysmon::test {

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

/**
 * @brief 启动真实 dzsysmon 子进程（与 master 的 ChildProcess 同款启动方式）。
 *
 * 启动前 `io` 会被 restart；失败通过 `ec` 返回（非抛出），由调用方决定 SKIP/FAIL。
 *
 * @param io 调用方 io_context（子进程句柄绑定其 executor）
 * @param ec 失败原因；成功时为空
 * @return 子进程句柄（调用方负责 stop_dzsysmon）
 */
inline std::unique_ptr<boost::process::v2::process> spawn_dzsysmon(boost::asio::io_context& io,
                                                                   boost::system::error_code& ec) {
    io.restart();
    boost::process::v2::default_process_launcher launcher;
    const auto exe = boost::filesystem::path(sysmon_executable().native());
    return std::make_unique<boost::process::v2::process>(
        launcher(io.get_executor(), ec, exe, std::vector<std::string>{},
                 boost::process::v2::process_stdio{nullptr, nullptr, nullptr}));
}

/// 强杀 dzsysmon 并回收句柄：e2e 只观察帧序，不测优雅退出（优雅退出由进程级用例单独覆盖）。
inline void stop_dzsysmon(std::unique_ptr<boost::process::v2::process>& child) {
    if (!child) {
        return;
    }
    boost::system::error_code ec;
    child->terminate(ec);
    child->wait(ec);
    child.reset();
}

}  // namespace dztrader::sysmon::test

#endif  // DZTRADER_SYSMON_TESTS_SYSMON_E2E_SUPPORT_H_
