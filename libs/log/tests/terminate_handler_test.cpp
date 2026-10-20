#include <dztrader/log/log.h>
#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

extern const std::filesystem::path& test_root();

namespace {

#ifdef _WIN32
constexpr wchar_t DEATH_LOG_DIR_ENV[] = L"DZLOG_TEST_DEATH_LOG_DIR";

/// Windows 死亡测试通过重新执行本二进制创建子进程，父进程的 test_root() 不会被子进程继承；
/// 日志目录经进程环境块传递，子进程启动时从父进程环境块继承。
void set_death_log_dir(const std::filesystem::path& dir) {
    ::SetEnvironmentVariableW(DEATH_LOG_DIR_ENV, dir.c_str());
}

void clear_death_log_dir() { ::SetEnvironmentVariableW(DEATH_LOG_DIR_ENV, nullptr); }

std::filesystem::path death_log_dir() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = ::GetEnvironmentVariableW(DEATH_LOG_DIR_ENV, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer.data(), length));
}
#else
constexpr const char* DEATH_LOG_DIR_ENV = "DZLOG_TEST_DEATH_LOG_DIR";

/// Linux 死亡测试 fork 出子进程，环境与内存均被继承；统一走环境变量使两平台路径逻辑一致。
void set_death_log_dir(const std::filesystem::path& dir) {
    ::setenv(DEATH_LOG_DIR_ENV, dir.c_str(), 1);
}

void clear_death_log_dir() { ::unsetenv(DEATH_LOG_DIR_ENV); }

std::filesystem::path death_log_dir() {
    const char* value = std::getenv(DEATH_LOG_DIR_ENV);
    return value != nullptr ? std::filesystem::path(value) : std::filesystem::path{};
}
#endif

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
