/**
 * @file orphan_guard_test.cpp
 * @brief OrphanGuard 集成测试（SQLite + file_lock + 真实子进程）。
 */

#include "orphan_guard.h"

#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>

#include <boost/asio/io_context.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dztrader::master {
namespace {

namespace process_v2 = boost::process::v2;

/// 进程唯一临时目录名（PID + 随机数）：ctest -j 并行时 gtest_discover_tests 把
/// 每个用例注册为独立 ctest 测试并行运行, 固定目录会让并行用例共用同一把
/// master.pid 文件锁互相踩踏 (FileLockPreventsDoubleInstance 的 try_lock 断言
/// 被其他用例持锁打穿)。与其他 master 测试的 unique_temp_dir 同款惯例。
std::filesystem::path unique_temp_dir() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           ("master_orphan_test_" +
            std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
            std::to_string(dist(gen)));
}

/// 日志行唯一后缀（避免当日日志文件中历史运行的残留行干扰断言）。
std::string unique_tag() {
    std::random_device rd;
    return std::to_string(rd());
}

std::filesystem::path worker_exe() {
    return dztrader::this_process::exe_dir() /
           "test_worker"
#ifdef _WIN32
           ".exe"
#endif
        ;
}

std::filesystem::path log_dir() { return std::filesystem::temp_directory_path() / "dz_test_logs"; }

/// 读取本测试 logger 写出的全部日志文本。
/// DailyFileSinkMt 在扩展名前插入日期，故按前缀匹配。
std::string read_suite_log() {
    std::string text;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(log_dir(), ec)) {
        const auto& path = entry.path();
        if (!entry.is_regular_file() ||
            path.filename().string().rfind("orphan_guard_test", 0) != 0) {
            continue;
        }
        std::ifstream ifs(path, std::ios::binary);
        text.append(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    return text;
}

/// 启动一个 test_worker（stdio 重定向到空设备，避免污染测试输出）。
process_v2::process spawn_worker(boost::asio::io_context& ioc,
                                 const std::vector<std::string>& args) {
    // process v2 使用 boost::filesystem::path
    auto exe = boost::filesystem::path(worker_exe().native());
    process_v2::default_process_launcher launcher;
    boost::system::error_code ec;
    auto proc = launcher(ioc.get_executor(), ec, exe, args,
                         process_v2::process_stdio{nullptr, nullptr, nullptr});
    if (ec) {
        throw std::runtime_error("spawn test_worker failed: " + ec.message());
    }
    return proc;
}

#ifndef _WIN32
/// fork 子进程持锁并握手通知父进程 (POSIX fcntl 记录锁为进程级,
/// 同进程二次加锁恒成功, 双实例语义必须跨进程验证)。
/// 返回子进程 pid, 子进程持锁 2 秒后退出。
pid_t fork_lock_holder(const std::filesystem::path& lock_path) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        return -1;
    }
    const pid_t child = ::fork();
    if (child != 0) {
        ::close(pipefd[1]);
        char msg = 0;
        const ssize_t n = ::read(pipefd[0], &msg, 1);
        ::close(pipefd[0]);
        return (n == 1 && msg == 1) ? child : -1;
    }
    // 子进程: 持锁 -> 握手 -> 睡 2 秒 -> 退出释放锁
    ::close(pipefd[0]);
    boost::interprocess::file_lock lock(lock_path.string().c_str());
    const bool locked = lock.try_lock();
    const char msg = locked ? 1 : 0;
    const ssize_t wret = ::write(pipefd[1], &msg, 1);
    (void)wret;
    ::close(pipefd[1]);
    if (locked) {
        ::sleep(2);
    }
    std::_Exit(locked ? 0 : 1);
}

/// fork 子进程尝试加同一把文件锁，返回是否成功（1=成功，0=失败）。
/// 用于证明父进程确实持有锁（父进程失败返回 -1）。
int fork_try_lock(const std::filesystem::path& lock_path) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        return -1;
    }
    const pid_t child = ::fork();
    if (child != 0) {
        ::close(pipefd[1]);
        char msg = -1;
        const ssize_t n = ::read(pipefd[0], &msg, 1);
        ::close(pipefd[0]);
        int status = 0;
        ::waitpid(child, &status, 0);
        return (n == 1) ? msg : -1;
    }
    ::close(pipefd[0]);
    boost::interprocess::file_lock lock(lock_path.string().c_str());
    const char msg = lock.try_lock() ? 1 : 0;
    const ssize_t wret = ::write(pipefd[1], &msg, 1);
    (void)wret;
    ::close(pipefd[1]);
    std::_Exit(0);
}
#endif

class OrphanGuardTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dztrader::log::LoggerSetup log_cfg;
        log_cfg.logger_name = "orphan_guard_test";
        log_cfg.log_dir = log_dir();
        log_cfg.enable_console = true;
        log_cfg.level = spdlog::level::debug;
        dztrader::log::set_default_logger(log_cfg);
    }

    void SetUp() override {
        tmp_dir_ = unique_temp_dir();
        std::filesystem::remove_all(tmp_dir_);
        std::filesystem::create_directories(tmp_dir_);

        // 保存并覆盖 DZTRADER_HOME，使 paths::cache() 指向临时目录
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        dztrader::env::set("DZTRADER_HOME", tmp_dir_.string());
        // paths::* 为进程级缓存: 每个用例换 home 后必须重置, 否则后续用例
        // 仍指向首个用例的目录 (ctest 逐例独立进程掩盖此问题, 直跑会红)
        dztrader::paths::reset_for_test();
    }

    void TearDown() override {
        if (orig_home_) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        dztrader::paths::reset_for_test();
        std::filesystem::remove_all(tmp_dir_);
    }

    std::filesystem::path tmp_dir_;
    std::optional<std::string> orig_home_;
};

TEST_F(OrphanGuardTest, StartupAcquiresLock) {
    const auto lock_path = tmp_dir_ / "cache" / "master.pid";
    const auto db_path = tmp_dir_ / "cache" / "children.db";

    OrphanGuard guard;
    ASSERT_NO_THROW(guard.startup());

    // startup 的实际副作用: 锁文件 + children 库被创建
    EXPECT_TRUE(std::filesystem::exists(lock_path));
    EXPECT_TRUE(std::filesystem::exists(db_path));

    // 互斥证明: 另一个进程（POSIX）/ 同进程第二个 file_lock（Windows）无法加锁
#ifdef _WIN32
    boost::interprocess::file_lock probe(lock_path.string().c_str());
    EXPECT_FALSE(probe.try_lock()) << "startup did not hold the instance lock";
#else
    EXPECT_EQ(fork_try_lock(lock_path), 0)
        << "another process acquired the instance lock held by startup";
#endif

    guard.cleanup();
    EXPECT_FALSE(std::filesystem::exists(lock_path));
}

TEST_F(OrphanGuardTest, StartupThrowsOnDoubleInstance) {
#ifdef _WIN32
    OrphanGuard guard1;
    guard1.startup();  // 第一个实例成功

    OrphanGuard guard2;
    // 第二个实例应抛出异常
    EXPECT_THROW(guard2.startup(), dztrader::Exception);

    guard1.cleanup();
#else
    // POSIX: fcntl 记录锁为进程级, 用 fork 子进程持锁模拟"另一实例"
    const auto lock_path = tmp_dir_ / "cache" / "master.pid";
    std::filesystem::create_directories(lock_path.parent_path());
    {
        std::ofstream ofs(lock_path);
    }

    const pid_t child = fork_lock_holder(lock_path);
    ASSERT_GE(child, 0) << "child failed to acquire lock";

    OrphanGuard guard;
    EXPECT_THROW(guard.startup(), dztrader::Exception);

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child lock holder exited abnormally";
#endif
}

TEST_F(OrphanGuardTest, StartupTerminatesLiveOrphan) {
    const auto exe = worker_exe();
    ASSERT_TRUE(std::filesystem::exists(exe)) << exe;

    boost::asio::io_context ioc;
    auto child = spawn_worker(ioc, {"orphan_live", "60"});
    const auto pid = child.id();
    ASSERT_GT(pid, 0);

    {
        // 模拟 master 崩溃: register_child 后不 cleanup, children 表残留
        OrphanGuard crashed_instance;
        crashed_instance.startup();
        crashed_instance.register_child(pid, "orphan_live", exe);
    }

    OrphanGuard guard;
    EXPECT_NO_THROW(guard.startup());
    guard.cleanup();

    // 真实子进程被终止
    bool exited = false;
    int exit_code = -1;
    child.async_wait([&](boost::system::error_code, int code) {
        exited = true;
        exit_code = code;
    });
    ioc.run_for(std::chrono::seconds(5));
    EXPECT_TRUE(exited) << "live orphan was not terminated";
    EXPECT_NE(exit_code, 0) << "orphan should be force-killed";

    // 分支证据: 走的是 exe 匹配后的终止路径
    const std::string logs = read_suite_log();
    EXPECT_NE(logs.find("terminating orphan | pid=" + std::to_string(pid) + " name=orphan_live"),
              std::string::npos)
        << logs;
}

TEST_F(OrphanGuardTest, StartupSkipsDeadPid) {
    const auto exe = worker_exe();
    ASSERT_TRUE(std::filesystem::exists(exe));

    // 启动一个短命 worker 并等它自然退出（PID 随后失效）
    boost::asio::io_context ioc;
    auto child = spawn_worker(ioc, {"orphan_dead", "1"});
    const auto pid = child.id();
    bool exited = false;
    child.async_wait([&](boost::system::error_code, int) { exited = true; });
    ioc.run_for(std::chrono::seconds(5));
    ASSERT_TRUE(exited);

    const std::string name = "orphan_dead_" + unique_tag();
    {
        OrphanGuard crashed_instance;
        crashed_instance.startup();
        crashed_instance.register_child(pid, name, exe);
    }

    OrphanGuard guard;
    EXPECT_NO_THROW(guard.startup());
    guard.cleanup();

    // 分支证据: PID 已死, 跳过而不是尝试终止
    const std::string logs = read_suite_log();
    EXPECT_NE(logs.find("orphan process dead | pid=" + std::to_string(pid) + " name=" + name),
              std::string::npos)
        << logs;
}

TEST_F(OrphanGuardTest, StartupSkipsPidReuse) {
    // PID 复用: 用本测试进程（存活）的 PID, 但登记成 test_worker 的 exe。
    // exe 不匹配时必须跳过 —— 若误终止, 本测试进程会被杀死。
    const auto self_pid = static_cast<process_v2::pid_type>(dztrader::this_process::pid());
    const std::string name = "orphan_reuse_" + unique_tag();
    {
        OrphanGuard crashed_instance;
        crashed_instance.startup();
        crashed_instance.register_child(self_pid, name, worker_exe());
    }

    OrphanGuard guard;
    EXPECT_NO_THROW(guard.startup());
    guard.cleanup();

    // 分支证据: 走的是 PID 复用跳过路径
    const std::string logs = read_suite_log();
    EXPECT_NE(logs.find("pid reused by other process, skipping | pid=" + std::to_string(self_pid)),
              std::string::npos)
        << logs;
}

TEST_F(OrphanGuardTest, FileLockPreventsDoubleInstance) {
    auto lock_path = tmp_dir_ / "cache" / "master.pid";
    std::filesystem::create_directories(lock_path.parent_path());

    // 创建锁文件
    {
        std::ofstream ofs(lock_path);
    }

#ifdef _WIN32
    // Windows: file_lock 为句柄级互斥, 同进程二次加锁即失败
    boost::interprocess::file_lock lock1(lock_path.string().c_str());
    EXPECT_TRUE(lock1.try_lock());

    // 同一文件的第二个锁应失败
    boost::interprocess::file_lock lock2(lock_path.string().c_str());
    EXPECT_FALSE(lock2.try_lock());

    // 解锁后第二个锁应成功
    lock1.unlock();
    EXPECT_TRUE(lock2.try_lock());
#else
    // POSIX: fcntl 记录锁为进程级, 跨进程互斥用 fork 子进程持锁验证
    boost::interprocess::file_lock parent_lock(lock_path.string().c_str());

    const pid_t child = fork_lock_holder(lock_path);
    ASSERT_GE(child, 0) << "child failed to acquire lock";
    EXPECT_FALSE(parent_lock.try_lock());

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    EXPECT_EQ(WEXITSTATUS(status), 0);

    // 子进程退出释放锁后, 父进程应能加锁
    EXPECT_TRUE(parent_lock.try_lock());
#endif
}

}  // namespace
}  // namespace dztrader::master
