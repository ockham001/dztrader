/**
 * @file child_process_test.cpp
 * @brief ChildProcess 集成测试（使用 test_worker）。
 */

#include "child_process.h"
#include "test_temp_dir.h"

#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace dztrader::master {
namespace {

/// 测试日志目录（与 SetUpTestSuite 的 LoggerSetup 一致）。
std::filesystem::path log_dir() { return dztrader::test::unique_temp_dir("dz_test_logs"); }

/// 读取本测试 logger 写出的全部日志文本。
/// DailyFileSinkMt 在扩展名前插入日期，故按前缀匹配。
std::string read_suite_log() {
    std::string text;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(log_dir(), ec)) {
        const auto& path = entry.path();
        if (!entry.is_regular_file() ||
            path.filename().string().rfind("child_process_test", 0) != 0) {
            continue;
        }
        std::ifstream ifs(path, std::ios::binary);
        text.append(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    return text;
}

class ChildProcessTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dztrader::log::LoggerSetup log_cfg;
        log_cfg.logger_name = "child_process_test";
        log_cfg.log_dir = log_dir();
        log_cfg.enable_console = true;
        log_cfg.level = spdlog::level::debug;
        dztrader::log::set_default_logger(log_cfg);
    }

    void SetUp() override {
        // test_worker 与测试二进制同目录（tests/）
        worker_exe_ = dztrader::this_process::exe_dir() /
                      "test_worker"
#ifdef _WIN32
                      ".exe"
#endif
            ;
    }

    ProcessEntry make_entry(const std::string& name, const std::vector<std::string>& args = {}) {
        ProcessEntry entry;
        entry.name = name;
        entry.category = Category::Strategy;
        entry.exe = worker_exe_;
        entry.args = args;
        entry.start_dir = worker_exe_.parent_path().empty() ? std::filesystem::current_path()
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

    // 在 terminate 前设置退出回调
    bool exited = false;
    child->async_wait([&](boost::system::error_code, int) { exited = true; });

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
    // child_process.h 承诺: 失败返回 false 且 ec 被设置, 未启动时 pid 为 0
    EXPECT_TRUE(ec);
    EXPECT_NE(ec.value(), 0);
    EXPECT_EQ(child->pid(), 0u);
    EXPECT_EQ(child->state(), ChildState::Stopped);
}

TEST_F(ChildProcessTest, SharedPtrKeepsAlive) {
    // 验证 async_wait 回调中的 shared_ptr 能保持子进程存活。
    // 先 cancel 并排空管道读回调, 否则读回调自身的 shared_from_this()
    // 会掩盖 async_wait 的捕获是否生效。
    auto entry = make_entry("lifecycle_test", {"lifecycle_test", "60"});

    std::weak_ptr<ChildProcess> weak_child;
    bool exited = false;
    {
        auto child = ChildProcess::create(ioc_, entry);
        weak_child = child;

        boost::system::error_code ec;
        ASSERT_TRUE(child->start(ec));

        child->cancel();
        // 管道读的挂起操作因 close 而完成; 执行掉这些 handler 后,
        // 仅剩 async_wait 可能持有 self。
        std::size_t drained = ioc_.poll();
        for (int i = 0; i < 100 && drained < 2; ++i) {
            drained += ioc_.poll();
        }
        ASSERT_EQ(drained, 2u);

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

    {
        auto keep = weak_child.lock();
        ASSERT_TRUE(keep);
        keep->terminate();
    }

    // 运行 io_context 让子进程退出和回调触发，回调返回后引用释放
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(2));
    EXPECT_TRUE(exited);
    EXPECT_TRUE(weak_child.expired());
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
    ASSERT_EQ(child->state(), ChildState::Running);

    // 先排空取消前已就绪的输出 handler（子进程尚未输出时为 0）
    ioc_.poll();

    child->cancel();

    // cancel 关闭两个管道: 挂起的 stdout/stderr 读操作以 operation_aborted
    // 完成（唯一的公开可观察面是这些 handler 被投递执行）。
    std::size_t drained = ioc_.poll();
    for (int i = 0; i < 100 && drained < 2; ++i) {
        drained += ioc_.poll();
    }
    EXPECT_EQ(drained, 2u) << "cancel should complete both pending pipe reads";

    // cancel 只关管道, 不改变进程状态（同 terminate 的 stopping 区分）
    EXPECT_EQ(child->state(), ChildState::Running);

    bool exited = false;
    child->async_wait([&](boost::system::error_code, int) { exited = true; });

    child->terminate();
    EXPECT_EQ(child->state(), ChildState::Stopping);

    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(2));
    EXPECT_TRUE(exited);
    EXPECT_EQ(child->state(), ChildState::Stopped);
}

TEST_F(ChildProcessTest, ForwardsStdoutStderrAndExitCode) {
    // worker 一次性输出模式: 打印 stdout + stderr 后以 7 退出
    auto entry = make_entry("fwd_test", {"fwd_test", "0", "7"});
    auto child = ChildProcess::create(ioc_, entry);

    boost::system::error_code ec;
    ASSERT_TRUE(child->start(ec));
    const auto pid = child->pid();

    bool exited = false;
    int exit_code = -1;
    child->async_wait([&](boost::system::error_code, int code) {
        exited = true;
        exit_code = code;
    });

    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));

    ASSERT_TRUE(exited);
    EXPECT_EQ(exit_code, 7);  // 非零退出码原样透传
    EXPECT_EQ(child->state(), ChildState::Stopped);

    const std::string logs = read_suite_log();
    const std::string entry_name = worker_exe_.filename().string();
    const std::string pid_str = std::to_string(pid);
    // escape_for_kv 转义: 双引号转 \" , 反斜杠转双反斜杠
    const std::string expected_stdout =
        "forwarded child stdout | child_pid=" + pid_str + " child_entry=" + entry_name +
        " child_msg=\"worker stdout | name=fwd_test note=\\\"a\\\\b\\\"c\\\"\"";
    const std::string expected_stderr =
        "forwarded child stderr | child_pid=" + pid_str + " child_entry=" + entry_name +
        " child_msg=\"worker stderr | name=fwd_test note=\\\"a\\\\b\\\"c\\\"\"";
    EXPECT_NE(logs.find(expected_stdout), std::string::npos) << logs;
    EXPECT_NE(logs.find(expected_stderr), std::string::npos) << logs;
}

}  // namespace
}  // namespace dztrader::master
