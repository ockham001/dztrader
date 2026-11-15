/**
 * @file child_process_test.cpp
 * @brief ChildProcess 集成测试（使用 test_worker）。
 */

#include "child_process.h"
#include "io_context_wait.h"
#include "test_temp_dir.h"

#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace dztrader::master {
namespace {

/// 测试日志目录（与 SetUpTestSuite 的 LoggerSetup 一致）。
std::filesystem::path log_dir() { return dztrader::test::unique_temp_dir_tracked("dz_test_logs"); }

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

    // 等退出回调; 超时只是上限（事件到达即早退），不用固定墙钟时长当同步手段
    EXPECT_TRUE(dztrader::test::run_io_until(ioc_, [&] { return exited; }));
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

    // 等 SIGTERM 后的退出回调; 超时只是上限
    EXPECT_TRUE(dztrader::test::run_io_until(ioc_, [&] { return exited; }));
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
        //
        // 注意 handler 数**不是常量 2**：poll() 统计的是所有被执行的 handler，其中包含
        // asio 复合操作（async_read_until）的内部 handler——数量随完成路径变化（高负载
        // 下实测为 3，本用例曾因此翻红）。所以排空到 io_context 静默为止，只断言**至少
        // 2 个 handler 被执行**（两根管道各一个；复合操作可能再多一个内部 handler）。
        std::size_t drained = 0;
        for (int i = 0; i < 100; ++i) {
            const std::size_t handlers = ioc_.poll();
            if (handlers == 0) {
                break;
            }
            drained += handlers;
        }
        ASSERT_GE(drained, 2u) << "cancel should drain both pending pipe reads";

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

    // 等子进程退出、回调触发（回调返回后引用释放）
    EXPECT_TRUE(dztrader::test::run_io_until(ioc_, [&] { return exited; }));
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

    // 先排空取消前已就绪的输出 handler（子进程尚未输出时为 0）；封顶避免 handler
    // 持续就绪时挂死。
    for (int i = 0; i < 100; ++i) {
        if (ioc_.poll() == 0) {
            break;
        }
    }

    child->cancel();

    // cancel 关闭两个管道: 挂起的 stdout/stderr 读操作以 operation_aborted
    // 完成（唯一的公开可观察面是这些 handler 被投递执行）。handler 数不是常量 2
    // （poll 计入 asio 复合操作的内部 handler，见 SharedPtrKeepsAlive 的说明），
    // 排空到静默为止，只断言至少 2 个 handler 被执行（两根管道各一个）。
    std::size_t drained = 0;
    for (int i = 0; i < 100; ++i) {
        const std::size_t handlers = ioc_.poll();
        if (handlers == 0) {
            break;
        }
        drained += handlers;
    }
    EXPECT_GE(drained, 2u) << "cancel should complete both pending pipe reads";

    // cancel 只关管道, 不改变进程状态（同 terminate 的 stopping 区分）
    EXPECT_EQ(child->state(), ChildState::Running);

    bool exited = false;
    child->async_wait([&](boost::system::error_code, int) { exited = true; });

    child->terminate();
    EXPECT_EQ(child->state(), ChildState::Stopping);

    EXPECT_TRUE(dztrader::test::run_io_until(ioc_, [&] { return exited; }));
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

    const std::string entry_name = worker_exe_.filename().string();
    const std::string pid_str = std::to_string(pid);
    // escape_for_kv 转义: 双引号转 \" , 反斜杠转双反斜杠
    const std::string expected_stdout =
        "forwarded child stdout | child_pid=" + pid_str + " child_entry=" + entry_name +
        " child_msg=\"worker stdout | name=fwd_test note=\\\"a\\\\b\\\"c\\\"\"";
    const std::string expected_stderr =
        "forwarded child stderr | child_pid=" + pid_str + " child_entry=" + entry_name +
        " child_msg=\"worker stderr | name=fwd_test note=\\\"a\\\\b\\\"c\\\"\"";

    // 等退出回调 **且** 两根管道的转发日志都落盘。超时只是上限；只等 exited 就停会漏掉
    // 与退出回调同批投递的管道读 handler（两者顺序不确定），让日志断言在高负载下假红。
    ASSERT_TRUE(dztrader::test::run_io_until(ioc_, [&] {
        if (!exited) {
            return false;
        }
        const std::string current = read_suite_log();
        return current.find(expected_stdout) != std::string::npos &&
               current.find(expected_stderr) != std::string::npos;
    }));

    EXPECT_EQ(exit_code, 7);  // 非零退出码原样透传
    EXPECT_EQ(child->state(), ChildState::Stopped);

    const std::string logs = read_suite_log();
    EXPECT_NE(logs.find(expected_stdout), std::string::npos) << logs;
    EXPECT_NE(logs.find(expected_stderr), std::string::npos) << logs;
}

}  // namespace
}  // namespace dztrader::master
