#include "../src/daily_sink.h"
#include <dztrader/log/log.h>
#include <dztrader/core/exception.h>
#include <dztrader/error.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <regex>
#include <spdlog/spdlog.h>
#include <string>

extern const std::filesystem::path& test_root();

namespace {

// 读取 logger 当天日志文件的最后一行
std::string read_last_log_line(const std::filesystem::path& log_dir,
                               const std::string& logger_name) {
    // daily_sink 命名规则: <logger_name>_YYYY-MM-DD.log
    std::string prefix = logger_name + "_";
    std::filesystem::path target;
    for (auto& entry : std::filesystem::directory_iterator(log_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();
        if (fname.starts_with(prefix) && fname.ends_with(".log")) {
            target = entry.path();
            break;
        }
    }
    if (target.empty()) return "";
    std::ifstream ifs(target);
    std::string line, last;
    while (std::getline(ifs, line)) {
        if (!line.empty()) last = line;
    }
    return last;
}

class NanosecondPatternTest : public ::testing::Test {
protected:
    void TearDown() override {
        spdlog::shutdown();
        std::set_terminate(nullptr);
    }
};

TEST_F(NanosecondPatternTest, FilePatternContainsNanosecondSpecifier) {
    // spdlog 1.17.0: %F = 纳秒（9 位）
    std::string pattern(dztrader::log::FILE_PATTERN);
    EXPECT_NE(pattern.find("%F"), std::string::npos)
        << "FILE_PATTERN should contain %F (nanoseconds in spdlog 1.17.0)";
}

TEST_F(NanosecondPatternTest, WrittenLogHasNineDigitFraction) {
    dztrader::log::LoggerSetup config;
    config.logger_name = "nano_test";
    config.enable_console = false;
    config.log_dir = test_root() / "nano_test_dir";
    config.level = spdlog::level::info;
    config.flush_level = spdlog::level::info;
    std::filesystem::create_directories(config.log_dir);
    ASSERT_NO_THROW(dztrader::log::set_default_logger(config));

    SPDLOG_INFO("nanosecond precision test");
    spdlog::default_logger()->flush();

    std::string line = read_last_log_line(config.log_dir, "nano_test");
    ASSERT_FALSE(line.empty()) << "log file should have at least one line";

    // 期望时间戳格式: YYYY-MM-DDTHH:MM:SS.nnnnnnnnn+HH:MM
    // 即小数点后 9 位数字（纳秒）
    std::regex ts_re(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{9}[+-]\d{2}:\d{2})");
    EXPECT_TRUE(std::regex_search(line, ts_re))
        << "log line should have nanosecond timestamp, got: " << line;
}

}  // namespace
