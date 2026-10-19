#include <gtest/gtest.h>
#include <dztrader/core/this_process.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include "config.h"

namespace fs = std::filesystem;

using dztrader::webui::load_webui_config;

class WebuiConfigTest : public ::testing::Test {
protected:
    fs::path tmp_;
    void SetUp() override {
        // §3.5：ctest -j 下同二进制各用例是独立进程，固定文件名会互相覆盖
        std::random_device rd;
        tmp_ = fs::temp_directory_path() /
               ("dz_webui_config_test_" +
                std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
                std::to_string(rd()));
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove(tmp_, ec);
    }
    void write(const std::string& content) { std::ofstream(tmp_) << content; }
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

TEST_F(WebuiConfigTest, ThrowsOnInvalidJson) {
    write("{ not json");
    EXPECT_THROW(load_webui_config(tmp_), std::runtime_error);
}

TEST_F(WebuiConfigTest, ThrowsWhenRootIsNotObject) {
    write("[]");
    EXPECT_THROW(load_webui_config(tmp_), std::runtime_error);
}

TEST_F(WebuiConfigTest, MissingAuthSectionYieldsEmptySecretAndDefaultTtl) {
    write(R"({
"server": {"listen": "127.0.0.1", "port": 9000},
"admin": {"username": "admin"}
})");
    auto cfg = load_webui_config(tmp_);
    EXPECT_TRUE(cfg.jwt_secret.empty());  // 缺失 auth 段 = 空密钥（加载器默认值）
    EXPECT_EQ(cfg.token_ttl_sec, 3600u);
}
