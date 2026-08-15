#include "../src/daily_sink.h"
#include <dztrader/core/exception.h>
#include <dztrader/error.h>
#include <dztrader/log/log.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

extern const std::filesystem::path& test_root();

class SetDefaultLoggerTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        spdlog::shutdown();
        std::set_terminate(nullptr);
    }
};

namespace {

dztrader::log::LoggerSetup make_config(const std::string& test_name)
{
    dztrader::log::LoggerSetup config;
    config.logger_name = test_name;
    config.enable_console = false;
    config.log_dir = test_root() / test_name;
    return config;
}

void expect_throws_invalid_param(auto fn)
{
    try {
        fn();
        FAIL() << "Expected Exception";
    }
    catch (const dztrader::Exception& e) {
        EXPECT_EQ(e.code(), DZ_EC_INVALID_PARAM);
    }
}

}  // namespace

TEST_F(SetDefaultLoggerTest, SetDefaultLogger)
{
    auto config = make_config("basic");
    config.enable_console = true;
    config.level = spdlog::level::warn;
    config.flush_level = spdlog::level::warn;
    EXPECT_NO_THROW(dztrader::log::set_default_logger(config));

    auto logger = spdlog::default_logger();
    ASSERT_TRUE(logger != nullptr);
    EXPECT_EQ(logger->name(), "basic");

    auto& sinks = logger->sinks();
    bool has_daily = false;
    bool has_console = false;
    for (const auto& sink : sinks) {
        if (dynamic_cast<dztrader::log::DailyFileSinkMt*>(sink.get())) {
            has_daily = true;
        }
        if (dynamic_cast<spdlog::sinks::stdout_color_sink_mt*>(sink.get())) {
            has_console = true;
        }
    }
    EXPECT_TRUE(has_daily);
    EXPECT_TRUE(has_console);

    EXPECT_EQ(sinks.size(), 2u);

    EXPECT_EQ(logger->level(), spdlog::level::warn);
    EXPECT_EQ(logger->flush_level(), spdlog::level::warn);

    EXPECT_NO_THROW(spdlog::info("test log message"));
    EXPECT_NO_THROW(logger->flush());
}

TEST_F(SetDefaultLoggerTest, CreatesLogDirectory)
{
    auto dir = test_root() / "creates_dir";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ASSERT_FALSE(std::filesystem::exists(dir));

    auto config = make_config("creates_dir");
    EXPECT_NO_THROW(dztrader::log::set_default_logger(config));
    EXPECT_TRUE(std::filesystem::exists(dir));
}

TEST_F(SetDefaultLoggerTest, ConsoleDisabled)
{
    auto config = make_config("console_off");
    config.enable_console = false;
    EXPECT_NO_THROW(dztrader::log::set_default_logger(config));

    auto logger = spdlog::default_logger();
    ASSERT_TRUE(logger != nullptr);
    EXPECT_EQ(logger->sinks().size(), 1u);
}

TEST_F(SetDefaultLoggerTest, ServiceModeDisablesConsole)
{
    auto config = make_config("service_mode");
    config.enable_console = true;
    config.service_mode = true;
    EXPECT_NO_THROW(dztrader::log::set_default_logger(config));

    auto logger = spdlog::default_logger();
    ASSERT_TRUE(logger != nullptr);
    EXPECT_EQ(logger->sinks().size(), 1u);
}

TEST_F(SetDefaultLoggerTest, EmptyProcessNameThrows)
{
    dztrader::log::LoggerSetup config;
    config.logger_name = "";
    config.log_dir = test_root() / "empty_name";
    expect_throws_invalid_param([&] { dztrader::log::set_default_logger(config); });
}

TEST_F(SetDefaultLoggerTest, EmptyLogDirThrows)
{
    dztrader::log::LoggerSetup config;
    config.logger_name = "empty_dir_test";
    config.log_dir = "";
    expect_throws_invalid_param([&] { dztrader::log::set_default_logger(config); });
}

TEST_F(SetDefaultLoggerTest, ProcessNameWithSlashThrows)
{
    dztrader::log::LoggerSetup config;
    config.logger_name = "sub/dir";
    config.log_dir = test_root() / "path_sep_slash";
    expect_throws_invalid_param([&] { dztrader::log::set_default_logger(config); });
}

TEST_F(SetDefaultLoggerTest, ProcessNameWithBackslashThrows)
{
    dztrader::log::LoggerSetup config;
    config.logger_name = "sub\\dir";
    config.log_dir = test_root() / "path_sep_backslash";
    expect_throws_invalid_param([&] { dztrader::log::set_default_logger(config); });
}

TEST_F(SetDefaultLoggerTest, ProcessNameWithColonThrows)
{
    dztrader::log::LoggerSetup config;
    config.logger_name = "sub:dir";
    config.log_dir = test_root() / "path_sep_colon";
    expect_throws_invalid_param([&] { dztrader::log::set_default_logger(config); });
}

TEST_F(SetDefaultLoggerTest, SetDefaultLoggerIdempotent)
{
    auto config = make_config("idempotent");
    EXPECT_NO_THROW(dztrader::log::set_default_logger(config));

    auto logger1 = spdlog::default_logger();
    ASSERT_TRUE(logger1 != nullptr);
    auto sinks_count_1 = logger1->sinks().size();
    auto level_1 = logger1->level();
    auto flush_1 = logger1->flush_level();

    EXPECT_NO_THROW(dztrader::log::set_default_logger(config));

    auto logger2 = spdlog::default_logger();
    ASSERT_TRUE(logger2 != nullptr);
    EXPECT_EQ(logger2->name(), "idempotent");
    EXPECT_EQ(logger2->sinks().size(), sinks_count_1);
    EXPECT_EQ(logger2->level(), level_1);
    EXPECT_EQ(logger2->flush_level(), flush_1);
}
