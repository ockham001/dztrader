#include <dztrader/log/log.h>
#include <exception>
#include <filesystem>
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

extern const std::filesystem::path& test_root();

class TerminateHandlerTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        spdlog::shutdown();
        std::set_terminate(nullptr);
    }

    void setup_log(const std::string& test_name)
    {
        dztrader::log::LoggerSetup config;
        config.logger_name = test_name;
        config.enable_console = false;
        config.log_dir = test_root() / test_name;
        config.flush_level = spdlog::level::warn;
        dztrader::log::set_default_logger(config);
    }
};

TEST_F(TerminateHandlerTest, TerminateHandlerInstalledAfterSetDefaultLogger)
{
    auto default_handler = std::get_terminate();
    setup_log("th_installed");
    auto handler = std::get_terminate();
    ASSERT_TRUE(handler != nullptr);
    EXPECT_NE(handler, default_handler);
}

TEST_F(TerminateHandlerTest, TerminatesProcessViaAbort)
{
    setup_log("th_abort");
    EXPECT_DEATH(std::terminate(), ".*");
}
