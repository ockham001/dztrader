#include <dztrader/log/log.h>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
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
/// 环境变量与 temp 目录在子进程里都不保证一致。gtest 明确把子进程 cwd 设为
/// original_working_dir，因此把握手文件放在 cwd，文件名带当前测试名避免并行冲突。
std::filesystem::path death_dir_file() {
    // gtest 把 --gtest_filter 原样传给死亡测试子进程；用它做文件名比
    // current_test_info() 更早可用，也避免父/子进程的测试上下文差异。
    std::string name = ::testing::GTEST_FLAG(filter);
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    if (name.empty()) {
        name = "all";
    }
    return std::filesystem::current_path() / (".dzlog-death-dir-" + name + ".txt");
}

void set_death_log_dir(const std::filesystem::path& dir) {
    std::ofstream out(death_dir_file(), std::ios::binary | std::ios::trunc);
    out << dir.string();
    out.close();
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
    ASSERT_TRUE(std::filesystem::exists(death_dir_file()))
        << "failed to write death dir handshake | file=" << death_dir_file();
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
    ASSERT_TRUE(std::filesystem::exists(death_dir_file()))
        << "failed to write death dir handshake | file=" << death_dir_file();
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
