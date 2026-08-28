/**
 * @file orphan_guard_test.cpp
 * @brief OrphanGuard 集成测试（SQLite + file_lock）。
 */

#include "orphan_guard.h"

#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>

#include <SQLiteCpp/Database.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <random>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dztrader::master {
namespace {

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
#endif

class OrphanGuardTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dztrader::log::LoggerSetup log_cfg;
        log_cfg.logger_name = "orphan_guard_test";
        log_cfg.log_dir = std::filesystem::temp_directory_path() / "dz_test_logs";
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
    }

    void TearDown() override {
        if (orig_home_) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        std::filesystem::remove_all(tmp_dir_);
    }

    std::filesystem::path tmp_dir_;
    std::optional<std::string> orig_home_;
};

TEST_F(OrphanGuardTest, StartupAcquiresLock) {
    OrphanGuard guard;
    // startup() 现在在失败时抛出异常而非调用 std::exit
    EXPECT_NO_THROW(guard.startup());
    guard.cleanup();
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
    { std::ofstream ofs(lock_path); }

    const pid_t child = fork_lock_holder(lock_path);
    ASSERT_GE(child, 0) << "child failed to acquire lock";

    OrphanGuard guard;
    EXPECT_THROW(guard.startup(), dztrader::Exception);

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child lock holder exited abnormally";
#endif
}

TEST_F(OrphanGuardTest, SQLiteReadWrite) {
    auto db_path = tmp_dir_ / "cache" / "children.db";
    std::filesystem::create_directories(db_path.parent_path());

    SQLite::Database db(db_path.string(),
        SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("PRAGMA journal_mode=WAL");
    db.exec("PRAGMA busy_timeout=5000");
    db.exec(
        "CREATE TABLE IF NOT EXISTS children ("
        "  pid   INTEGER PRIMARY KEY,"
        "  name  TEXT NOT NULL,"
        "  exe   TEXT NOT NULL,"
        "  ts    INTEGER NOT NULL"
        ")"
    );

    // 插入
    SQLite::Statement insert(db,
        "INSERT INTO children (pid, name, exe, ts) VALUES (?, ?, ?, ?)");
    insert.bind(1, 12345);
    insert.bind(2, "test_child");
    insert.bind(3, "/path/to/exe");
    insert.bind(4, 1000);
    insert.exec();

    // 读取
    SQLite::Statement query(db, "SELECT pid, name, exe FROM children WHERE pid = 12345");
    ASSERT_TRUE(query.executeStep());
    EXPECT_EQ(query.getColumn(0).getInt64(), 12345);
    EXPECT_EQ(query.getColumn(1).getString(), "test_child");
    EXPECT_EQ(query.getColumn(2).getString(), "/path/to/exe");

    // 删除
    SQLite::Statement del(db, "DELETE FROM children WHERE pid = ?");
    del.bind(1, 12345);
    del.exec();

    // 验证已删除
    SQLite::Statement query2(db, "SELECT COUNT(*) FROM children");
    ASSERT_TRUE(query2.executeStep());
    EXPECT_EQ(query2.getColumn(0).getInt64(), 0);
}

TEST_F(OrphanGuardTest, FileLockPreventsDoubleInstance) {
    auto lock_path = tmp_dir_ / "cache" / "master.pid";
    std::filesystem::create_directories(lock_path.parent_path());

    // 创建锁文件
    { std::ofstream ofs(lock_path); }

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

TEST_F(OrphanGuardTest, PidRecordStructure) {
    PidRecord rec;
    rec.pid = 42;
    rec.name = "test";
    rec.exe_path = "/usr/bin/test";

    EXPECT_EQ(rec.pid, 42u);
    EXPECT_EQ(rec.name, "test");
    EXPECT_EQ(rec.exe_path, "/usr/bin/test");
}

}  // namespace
}  // namespace dztrader::master
