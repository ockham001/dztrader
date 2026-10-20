#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <iterator>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>

extern const std::filesystem::path& test_root();

namespace {

/// 死亡测试子进程的日志目录握手文件。
///
/// Windows 的 gtest death test 会重新执行二进制，父进程的 test_root() 不继承；
/// 环境变量在 Windows 子进程里也不可靠。改用文件握手：路径由「可执行文件绝对路径 +
/// 当前测试名」哈希得到，父进程写入、子进程读取，双方无需共享内存或环境块。
std::filesystem::path death_dir_file() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string test_name =
        info != nullptr ? std::string(info->test_suite_name()) + "." + info->name() : "unknown";
    const std::string key = dztrader::this_process::exe_path().string() + "|" + test_name;
    const auto hash = std::hash<std::string>{}(key);
    return std::filesystem::temp_directory_path() /
           ("dzlog-death-dir-" + std::to_string(hash) + ".txt");
}

void set_death_log_dir(const std::filesystem::path& dir) {
    std::ofstream out(death_dir_file(), std::ios::binary | std::ios::trunc);
    out << dir.string();
}

void clear_death_log_dir() {
    std::error_code ec;
    std::filesystem::remove(death_dir_file(), ec);
}

std::filesystem::path death_log_dir() {
    std::ifstream in(death_dir_file(), std::ios::binary);
    std::string value;
    std::getline(in, value);
    return value.empty() ? std::filesystem::path{} : std::filesystem::path(value);
}

void setup_log_at(const std::filesystem::path& log_dir, const std::string& logger_name) {
    dztrader::log::LoggerSetup config;
    config.logger_name = logger_name;
    config.enable_console = false;
    config.log_dir = log_dir;
    // flush_level=warn 保证 critical 日志在 abort 前落盘
    config.flush_level = spdlog::level::warn;
    dztrader::log::set_default_logger(config);
}

void setup_log(const std::string& logger_name) {
    setup_log_at(test_root() / logger_name, logger_name);
}

// 读取目录下全部日志内容（父进程在死亡测试子进程退出后读取）
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

}  // namespace

class TerminateHandlerTest : public ::testing::Test {
protected:
    void TearDown() override {
        clear_death_log_dir();
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
    const auto log_dir = test_root() / "th_abort";
    set_death_log_dir(log_dir);
    // handler 必须在死亡测试子进程内安装：Windows 的子进程是重新执行的二进制，
    // 父进程里安装的 handler 不存在。
    EXPECT_DEATH(
        {
            setup_log_at(death_log_dir(), "th_abort");
            // marker 证明子进程拿到了父进程的日志目录；若 marker 缺失，
            // 说明跨进程路径传递失败，而不是 handler 没落盘。
            {
                std::ofstream marker(death_log_dir() / "marker.txt", std::ios::binary);
                marker << "child-ran";
            }
            std::terminate();
        },
        ".*");

    EXPECT_TRUE(std::filesystem::exists(log_dir / "marker.txt"))
        << "death test child did not receive log dir | dir=" << log_dir;
    const std::string content = read_all_logs(log_dir);
    EXPECT_FALSE(content.empty()) << "terminate handler should write to log | dir=" << log_dir;
    EXPECT_NE(content.find("std::terminate called (no active exception)"), std::string::npos)
        << "log content: " << content;
}

TEST_F(TerminateHandlerTest, LogsUncaughtExceptionBeforeAbort) {
    const auto log_dir = test_root() / "th_uncaught";
    set_death_log_dir(log_dir);
    EXPECT_DEATH(
        {
            setup_log_at(death_log_dir(), "th_uncaught");
            {
                std::ofstream marker(death_log_dir() / "marker.txt", std::ios::binary);
                marker << "child-ran";
            }
            try {
                throw std::runtime_error("boom from death test");
            } catch (...) {
                std::terminate();
            }
        },
        ".*");

    EXPECT_TRUE(std::filesystem::exists(log_dir / "marker.txt"))
        << "death test child did not receive log dir | dir=" << log_dir;
    const std::string content = read_all_logs(log_dir);
    EXPECT_FALSE(content.empty()) << "terminate handler should write to log | dir=" << log_dir;
    EXPECT_NE(content.find("uncaught exception"), std::string::npos) << "log content: " << content;
    EXPECT_NE(content.find("boom from death test"), std::string::npos)
        << "log content: " << content;
}
