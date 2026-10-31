#include <gtest/gtest.h>
#include <dztrader/core/this_process.h>
#include <nlohmann/json.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include "config.h"

namespace fs = std::filesystem;

using dztrader::webui::generate_default_config;
using dztrader::webui::is_loopback_listen;
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

// ---- 监听地址回环判定（#201）----
// 只有明确的本机回环字面量才算回环；其余（含 0.0.0.0 / :: 通配与非本机地址）一律
// 视为对外暴露 —— 方向刻意保守：宁可多报一条启动告警，不可漏报管理面暴露。

TEST(WebuiListenLoopbackTest, LoopbackLiteralsAreRecognized) {
    for (const char* listen : {"127.0.0.1", "127.0.0.2", "127.255.255.254", "::1", "[::1]",
                               "localhost", "LOCALHOST", " localhost "}) {
        EXPECT_TRUE(is_loopback_listen(listen)) << "listen=" << listen;
    }
}

TEST(WebuiListenLoopbackTest, WildcardAndRoutableAddressesAreExposed) {
    for (const char* listen : {"0.0.0.0", "::", "0:0:0:0:0:0:0:0", "[::]", "192.168.1.5",
                               "10.0.0.1", "example.com", ""}) {
        EXPECT_FALSE(is_loopback_listen(listen)) << "listen=" << listen;
    }
}

// 前后缀误判防线：以 127./localhost 开头但并非合法回环字面量的字符串（含带端口的
// 写法）不得被判成回环，否则 0.0.0.0 之外的暴露面会漏报。
TEST(WebuiListenLoopbackTest, PrefixLookalikesAreNotLoopback) {
    for (const char* listen : {"127.0.0.1.evil.example", "127.0.0", "127.0.0.256", "127.0.0.1:8080",
                               "localhost.example", "127.0.0.1x", "::10", "0.0.0.0.0"}) {
        EXPECT_FALSE(is_loopback_listen(listen)) << "listen=" << listen;
    }
}

// ---- 自动生成的默认配置说明暴露面（#201）----
// JSON 无注释语法，注释以 server._comment 承载（加载器忽略未知键）。
TEST_F(WebuiConfigTest, GeneratedConfigDocumentsListenExposure) {
    fs::remove(tmp_);
    generate_default_config(tmp_);
    ASSERT_TRUE(fs::exists(tmp_));

    std::ifstream ifs(tmp_);
    ASSERT_TRUE(static_cast<bool>(ifs));
    nlohmann::json data;
    ifs >> data;
    ASSERT_TRUE(data["server"].contains("_comment"))
        << "generated webui.json must document the listen exposure";

    const auto comment = data["server"]["_comment"].get<std::string>();
    std::string comment_lower = comment;
    for (char& c : comment_lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    EXPECT_NE(comment_lower.find("listen"), std::string::npos) << comment;
    EXPECT_NE(comment_lower.find("tls"), std::string::npos) << comment;
    EXPECT_NE(comment.find("127.0.0.1"), std::string::npos) << comment;

    // 注释键不参与加载：默认值照常生效，不因多出未知键而抛
    const auto cfg = load_webui_config(tmp_);
    EXPECT_EQ(cfg.server_listen, "0.0.0.0");
    EXPECT_EQ(cfg.server_port, 8080);
}
