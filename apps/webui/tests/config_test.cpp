#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "config.h"

namespace fs = std::filesystem;

using dztrader::webui::load_webui_config;

class WebuiConfigTest : public ::testing::Test {
protected:
    fs::path tmp_;
    void SetUp() override {
        tmp_ = fs::temp_directory_path() / "webui_test.json";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove(tmp_, ec);
    }
    void write(const std::string& content) {
        std::ofstream(tmp_) << content;
    }
};

TEST_F(WebuiConfigTest, ParsesAllFields) {
    write(R"({
"server": {"listen": "0.0.0.0", "port": 8080},
"auth": {"jwt_secret": "test_secret_key_at_least_32_chars_long", "token_ttl_sec": 3600},
"admin": {"username": "admin", "password": "admin123"}
})");
    auto cfg = load_webui_config(tmp_);
    EXPECT_EQ(cfg.server_listen, "0.0.0.0");
    EXPECT_EQ(cfg.server_port, 8080);
    EXPECT_EQ(cfg.jwt_secret, "test_secret_key_at_least_32_chars_long");
    EXPECT_EQ(cfg.token_ttl_sec, 3600);
    EXPECT_EQ(cfg.admin_username, "admin");
    EXPECT_EQ(cfg.admin_password, "admin123");
    EXPECT_FALSE(cfg.admin_password_is_default);
}

TEST_F(WebuiConfigTest, UsesDefaultPasswordWhenPasswordMissing) {
    write(R"({
"server": {"listen": "0.0.0.0", "port": 8080},
"auth": {"jwt_secret": "test_secret_key_at_least_32_chars_long"},
"admin": {"username": "admin"}
})");
    auto cfg = load_webui_config(tmp_);
    EXPECT_EQ(cfg.admin_password, "88888888");
    EXPECT_TRUE(cfg.admin_password_is_default);
}

TEST_F(WebuiConfigTest, ThrowsOnMissingFile) {
    EXPECT_THROW(load_webui_config("/nonexistent/path.json"), std::runtime_error);
}