#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>

#include <boost/asio/io_context.hpp>
#include <boost/process/v2/process.hpp>

#include <exception>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

extern const std::filesystem::path& test_root();

namespace {

void setup_log(const std::string& logger_name) {
    dztrader::log::LoggerSetup config;
    config.logger_name = logger_name;
    config.enable_console = false;
    config.log_dir = test_root() / logger_name;
    // flush_level=warn 保证 critical 日志在 abort 前落盘
    config.flush_level = spdlog::level::warn;
    dztrader::log::set_default_logger(config);
}

// 读取目录下全部日志内容（父进程在 helper 进程退出后读取）
std::string read_all_logs(const std::filesystem::path& log_dir) {
    std::string content;
    std::error_code ec;
    if (!std::filesystem::is_directory(log_dir, ec)) {
        return content;
    }
    for (const auto& entry : std::filesystem::directory_iterator(log_dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".log") {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        content.append(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        content.push_back('\n');
    }
    return content;
}

std::filesystem::path helper_path() {
    std::string name = "dzlog_terminate_helper";
#ifdef _WIN32
    name += ".exe";
#endif
    return dztrader::this_process::exe_path().parent_path() / name;
}

/// 用普通子进程验证 terminate handler，而不是 gtest death test。
///
/// Windows 的 gtest death test 会重新执行测试二进制，父进程的 test_root()/环境变量
/// 无法可靠传给子进程；普通进程的 argv 是跨平台可靠的。
int run_helper(const std::filesystem::path& log_dir, const std::string& mode) {
    boost::asio::io_context ioc;
    boost::process::v2::process proc(
        ioc, helper_path().string(),
        std::vector<std::string>{"--log-dir", log_dir.string(), "--mode", mode});
    return proc.wait();
}

}  // namespace

class TerminateHandlerTest : public ::testing::Test {
protected:
    void TearDown() override {
        spdlog::shutdown();
        std::set_terminate(nullptr);
    }
};

TEST_F(TerminateHandlerTest, TerminateHandlerInstalledAfterSetDefaultLogger) {
    auto default_handler = std::get_terminate();
    setup_log("th_installed");
    auto handler = std::get_terminate();
    ASSERT_TRUE(handler != nullptr);
    EXPECT_NE(handler, default_handler);
}

TEST_F(TerminateHandlerTest, TerminatesProcessViaAbort) {
    ASSERT_TRUE(std::filesystem::exists(helper_path())) << "helper not found: " << helper_path();

    const auto log_dir = test_root() / "th_abort";
    EXPECT_NE(run_helper(log_dir, "abort"), 0);

    const std::string content = read_all_logs(log_dir);
    EXPECT_FALSE(content.empty()) << "terminate handler should write to log | dir=" << log_dir;
    EXPECT_NE(content.find("std::terminate called (no active exception)"), std::string::npos)
        << "log content: " << content;
}

TEST_F(TerminateHandlerTest, LogsUncaughtExceptionBeforeAbort) {
    ASSERT_TRUE(std::filesystem::exists(helper_path())) << "helper not found: " << helper_path();

    const auto log_dir = test_root() / "th_uncaught";
    EXPECT_NE(run_helper(log_dir, "uncaught"), 0);

    const std::string content = read_all_logs(log_dir);
    EXPECT_FALSE(content.empty()) << "terminate handler should write to log | dir=" << log_dir;
    EXPECT_NE(content.find("uncaught exception"), std::string::npos) << "log content: " << content;
    EXPECT_NE(content.find("boom from helper"), std::string::npos) << "log content: " << content;
}
