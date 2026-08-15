/**
 * @file child_process_test.cpp
 * @brief ChildProcess 集成测试（使用 test_worker）。
 */

#include "child_process.h"

#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <filesystem>
#include <chrono>
#include <thread>

namespace dztrader::master {
namespace {

class ChildProcessTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dztrader::log::LoggerSetup log_cfg;
        log_cfg.logger_name = "child_process_test";
        log_cfg.log_dir = std::filesystem::temp_directory_path() / "dz_test_logs";
        log_cfg.enable_console = true;
        log_cfg.level = spdlog::level::debug;
        dztrader::log::set_default_logger(log_cfg);
    }

    void SetUp() override {
        // test_worker 与测试二进制同目录（tests/）
        worker_exe_ = dztrader::this_process::exe_dir() / "test_worker"
#ifdef _WIN32
            ".exe"
#endif
            ;
    }

    ProcessEntry make_entry(const std::string& name,
                            const std::vector<std::string>& args = {}) {
        ProcessEntry entry;
        entry.name = name;
        entry.category = Category::Strategy;
        entry.exe = worker_exe_;
        entry.args = args;
        entry.start_dir = worker_exe_.parent_path().empty()
            ? std::filesystem::current_path()
            : worker_exe_.parent_path();
        entry.restart = default_restart_policy(Category::Strategy);
        return entry;
    }

    std::filesystem::path worker_exe_;
    boost::asio::io_context ioc_;
};

TEST_F(ChildProcessTest, StartAndExit) {
    auto entry = make_entry("worker1", {"worker1", "2"});  // 运行 2 秒
    auto child = ChildProcess::create(ioc_, entry);

    boost::system::error_code ec;
    ASSERT_TRUE(child->start(ec)) << "启动失败: " << ec.message();
    EXPECT_EQ(child->state(), ChildState::Running);
    EXPECT_GT(child->pid(), 0u);

    // 等待退出
    bool exited = false;
    int exit_code = -1;
    child->async_wait([&](boost::system::error_code, int code) {
        exited = true;
        exit_code = code;
    });

    // 运行 io_context 处理异步事件
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));
    EXPECT_TRUE(exited);
    EXPECT_EQ(exit_code, 0);  // 正常退出
    EXPECT_EQ(child->state(), ChildState::Stopped);
}

TEST_F(ChildProcessTest, Terminate) {
    auto entry = make_entry("worker2", {"worker2", "60"});  // 运行 60 秒
    auto child = ChildProcess::create(ioc_, entry);

    boost::system::error_code ec;
    ASSERT_TRUE(child->start(ec));

    EXPECT_EQ(child->state(), ChildState::Running);

    // 等待子进程启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 在 terminate 前设置退出回调
    bool exited = false;
    child->async_wait([&](boost::system::error_code, int) {
        exited = true;
    });

    child->terminate();
    EXPECT_EQ(child->state(), ChildState::Stopping);

    // 重启 io_context（可能被之前的 run_for 停止）
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(2));
    EXPECT_TRUE(exited);
    EXPECT_EQ(child->state(), ChildState::Stopped);
}

TEST_F(ChildProcessTest, StartInvalidExe) {
    ProcessEntry entry;
    entry.name = "invalid";
    entry.category = Category::Strategy;
    entry.exe = "/nonexistent/path/to/binary";
    entry.start_dir = "/tmp";
    entry.restart = default_restart_policy(Category::Strategy);

    auto child = ChildProcess::create(ioc_, entry);

    boost::system::error_code ec;
    EXPECT_FALSE(child->start(ec));
    EXPECT_EQ(child->state(), ChildState::Stopped);
}

TEST_F(ChildProcessTest, SharedPtrKeepsAlive) {
    // 验证 async_wait 回调中的 shared_ptr 能保持子进程存活
    auto entry = make_entry("lifecycle_test", {"lifecycle_test", "1"});

    std::weak_ptr<ChildProcess> weak_child;
    bool exited = false;
    {
        auto child = ChildProcess::create(ioc_, entry);
        weak_child = child;

        boost::system::error_code ec;
        ASSERT_TRUE(child->start(ec));

        child->async_wait([&](boost::system::error_code, int) {
            exited = true;
            // 此时回调的 shared_ptr 仍然存活
            // 即使调用者已释放引用
        });

        // child 在此作用域结束，但 async_wait 回调持有 shared_ptr
    }

    // weak_child 不应过期 — async_wait 回调持有引用
    // （子进程仍在运行，回调尚未触发）
    EXPECT_FALSE(weak_child.expired());

    // 运行 io_context 让子进程退出和回调触发
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));
    EXPECT_TRUE(exited);
}

TEST_F(ChildProcessTest, NameAndEntry) {
    auto entry = make_entry("name_test");
    auto child = ChildProcess::create(ioc_, entry);

    EXPECT_EQ(child->name(), "name_test");
    EXPECT_EQ(child->entry().name, "name_test");
}

TEST_F(ChildProcessTest, CancelClosesPipes) {
    auto entry = make_entry("cancel_test", {"cancel_test", "60"});
    auto child = ChildProcess::create(ioc_, entry);

    boost::system::error_code ec;
    ASSERT_TRUE(child->start(ec));

    // cancel 应关闭管道而不崩溃
    child->cancel();

    // 清理
    child->terminate();
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(2));
}

}  // namespace
}  // namespace dztrader::master
