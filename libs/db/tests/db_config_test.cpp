/**
 * @file db_config_test.cpp
 * @brief db::resolve_config 单测：优先级 / 自动生成 / 非法 JSON / 默认 path / env 映射
 *
 * 本套件在 SetUp 内自管临时 DZTRADER_HOME（AGENTS.md 硬门禁 2 的测试豁免）；
 * 同一二进制内所有用例共用同一 home 路径（paths::home() 按进程缓存），
 * 每个用例重新创建目录并清理相关环境变量。
 *
 * home 目录名带 PID + 随机后缀：`gtest_discover_tests` 把每个用例注册成独立 ctest
 * 进程，`ctest -j` 下固定名目录会被并发用例互相 `remove_all`（#136 实测
 * `StructuralViolationsThrowAndKeepFile` 因此偶发失败）。
 */
#include <gtest/gtest.h>

#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/db/config.h>
#include <dztrader/error.h>

#include <nlohmann/json.hpp>

#include "test_temp_dir.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using dztrader::db::Config;

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << content;
}

bool has_option(const Config& config, std::string_view key) {
    return config.options.find(key) != config.options.end();
}

std::string option(const Config& config, std::string_view key) {
    const auto it = config.options.find(key);
    return it == config.options.end() ? std::string() : it->second;
}

DzErrorCode resolve_error() {
    try {
        (void)dztrader::db::resolve_config("td");
    } catch (const dztrader::Exception& e) {
        return e.code();
    }
    return DZ_EC_OK;
}

/// 配置解析套件：临时 DZTRADER_HOME + 临时 configs/database.json。
class DbConfigTest : public ::testing::Test {
protected:
    fs::path home_;
    fs::path config_file_;
    std::optional<std::string> original_home_;

    void SetUp() override {
        original_home_ = dztrader::env::get("DZTRADER_HOME");
        home_ = dztrader::test::unique_temp_dir("dz_test_db_config");
        fs::remove_all(home_);
        fs::create_directories(home_ / "configs");
        fs::create_directories(home_ / "db");
        dztrader::env::set("DZTRADER_HOME", home_.string());
        config_file_ = home_ / "configs" / "database.json";
        clear_env();
    }

    void TearDown() override {
        clear_env();
        // 恢复调用方传入的 DZTRADER_HOME (ctest 注入 .cache/test_home), 并清掉临时 home
        if (original_home_.has_value()) {
            dztrader::env::set("DZTRADER_HOME", *original_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        std::error_code ec;
        fs::remove_all(home_, ec);
    }

    static void clear_env() {
        for (const char* key :
             {"DZTRADER_DB_BACKEND", "DZTRADER_DB_OPT_PATH", "DZTRADER_DB_OPT_BUSY_TIMEOUT_MS",
              "DZTRADER_DB_OPT_CACHE_SIZE_KB", "DZTRADER_DB_OPT_HOST", "DZTRADER_DB_OPT_PORT",
              "DZTRADER_DB_OPT_DATABASE", "DZTRADER_DB_OPT_BAD-KEY", "DZTRADER_DB_OPT_"}) {
            dztrader::env::unset(key);
        }
    }
};

// 默认 (无文件): sqlite + paths::td_db() + busy_timeout_ms=5000, 并自动生成配置段;
// 生成内容不含 path (与 DZTRADER_HOME 位置解耦), 再次解析幂等不改文件。
TEST_F(DbConfigTest, DefaultsGenerateConfigFileIdempotently) {
    ASSERT_FALSE(fs::exists(config_file_));
    const Config config = dztrader::db::resolve_config("td");
    EXPECT_EQ("sqlite", config.backend);
    EXPECT_EQ(dztrader::paths::td_db().string(), option(config, "path"));
    EXPECT_EQ("5000", option(config, "busy_timeout_ms"));

    ASSERT_TRUE(fs::exists(config_file_));
    const std::string first = read_file(config_file_);
    const auto json = nlohmann::json::parse(first);
    ASSERT_TRUE(json.contains("td"));
    EXPECT_EQ("sqlite", json["td"]["backend"].get<std::string>());
    EXPECT_EQ("5000", json["td"]["options"]["busy_timeout_ms"].get<std::string>());
    EXPECT_FALSE(json["td"]["options"].contains("path"));

    (void)dztrader::db::resolve_config("td");
    EXPECT_EQ(first, read_file(config_file_));
}

// 文件段覆盖内置默认; options 标量 (字符串/数字) 统一转字符串。
TEST_F(DbConfigTest, FileSectionOverridesBuiltinDefault) {
    write_file(config_file_, R"({"td":{"backend":"sqlite","options":{"path":"/tmp/from_file.db",)"
                             R"("busy_timeout_ms":"1234","cache_size_kb":2048}}})");
    const Config config = dztrader::db::resolve_config("td");
    EXPECT_EQ("sqlite", config.backend);
    EXPECT_EQ("/tmp/from_file.db", option(config, "path"));
    EXPECT_EQ("1234", option(config, "busy_timeout_ms"));
    EXPECT_EQ("2048", option(config, "cache_size_kb"));
}

// 环境变量覆盖文件段 (DZTRADER_DB_OPT_<KEY> -> options[<key 小写>])。
TEST_F(DbConfigTest, EnvOptionsOverrideFileSection) {
    write_file(config_file_, R"({"td":{"backend":"sqlite","options":{"path":"/tmp/from_file.db",)"
                             R"("busy_timeout_ms":"1234"}}})");
    dztrader::env::set("DZTRADER_DB_OPT_PATH", "/tmp/from_env.db");
    dztrader::env::set("DZTRADER_DB_OPT_BUSY_TIMEOUT_MS", "2222");
    const Config config = dztrader::db::resolve_config("td");
    EXPECT_EQ("/tmp/from_env.db", option(config, "path"));
    EXPECT_EQ("2222", option(config, "busy_timeout_ms"));
}

// 环境变量切换 backend -> 采用该后端的内置默认; mysql 不注入 sqlite 的默认 path。
TEST_F(DbConfigTest, EnvBackendSwitchesToMysqlDefaults) {
    write_file(config_file_, R"({"td":{"backend":"sqlite","options":{"busy_timeout_ms":"1234"}}})");
    dztrader::env::set("DZTRADER_DB_BACKEND", "mysql");
    const Config config = dztrader::db::resolve_config("td");
    EXPECT_EQ("mysql", config.backend);
    EXPECT_FALSE(has_option(config, "path"));
    EXPECT_EQ("127.0.0.1", option(config, "host"));
    EXPECT_EQ("3306", option(config, "port"));
    EXPECT_EQ("dztrader", option(config, "database"));
    EXPECT_EQ("5000", option(config, "connect_timeout_ms"));
    EXPECT_EQ("1234", option(config, "busy_timeout_ms"));  // 文件键保留, 驱动忽略未知键
}

// 文件段指定 mysql: 内置 mysql 默认 + 文件 options 覆盖。
TEST_F(DbConfigTest, FileBackendMysqlAppliesMysqlDefaults) {
    write_file(config_file_, R"({"td":{"backend":"mysql","options":{"user":"dz"}}})");
    const Config config = dztrader::db::resolve_config("td");
    EXPECT_EQ("mysql", config.backend);
    EXPECT_EQ("dz", option(config, "user"));
    EXPECT_EQ("127.0.0.1", option(config, "host"));
    EXPECT_EQ("3306", option(config, "port"));
    EXPECT_FALSE(has_option(config, "path"));
}

// 非法 JSON: 抛 DZ_EC_INVALID_PARAM (不静默回落默认) 且不改写损坏文件。
TEST_F(DbConfigTest, InvalidJsonThrowsAndKeepsFile) {
    const std::string broken = R"({"td": { broken )";
    write_file(config_file_, broken);
    try {
        (void)dztrader::db::resolve_config("td");
        FAIL() << "resolve_config should throw on invalid json";
    } catch (const dztrader::Exception& e) {
        EXPECT_EQ(DZ_EC_INVALID_PARAM, e.code());
    }
    EXPECT_EQ(broken, read_file(config_file_));
}

// 根不是 object: 抛 DZ_EC_INVALID_PARAM。
TEST_F(DbConfigTest, NonObjectRootThrows) {
    write_file(config_file_, "[1, 2, 3]");
    try {
        (void)dztrader::db::resolve_config("td");
        FAIL() << "resolve_config should throw on non-object root";
    } catch (const dztrader::Exception& e) {
        EXPECT_EQ(DZ_EC_INVALID_PARAM, e.code());
    }
}

// 结构校验分支: 段/字段类型必须与 config.h 的承诺一致, 一律 DZ_EC_INVALID_PARAM 且不改写文件
TEST_F(DbConfigTest, StructuralViolationsThrowAndKeepFile) {
    const std::string cases[] = {
        R"({"td": 1})",                            // 段非 object
        R"({"td": {"backend": 5}})",               // backend 非 string
        R"({"td": {"options": []}})",              // options 非 object
        R"({"td": {"options": {"path": []}}})",    // option 值为数组
        R"({"td": {"options": {"path": {}}}})",    // option 值为对象
        R"({"td": {"options": {"path": null}}})",  // option 值为 null
    };
    for (const std::string& content : cases) {
        SCOPED_TRACE(content);
        write_file(config_file_, content);
        EXPECT_EQ(resolve_error(), DZ_EC_INVALID_PARAM);
        EXPECT_EQ(read_file(config_file_), content);
    }
}

// 配置文件不可读/默认段写入失败 -> DZ_EC_SYSTEM (configs 被普通文件占位)
TEST_F(DbConfigTest, UnwritableConfigsPathMapsToSystem) {
    fs::remove_all(home_ / "configs");
    write_file(home_ / "configs", "not a directory");
    EXPECT_EQ(resolve_error(), DZ_EC_SYSTEM);
}

// domain 段缺失: 生成该段默认, 保留文件其他段; 已有段不被重写。
TEST_F(DbConfigTest, MissingSectionGeneratesAndKeepsOtherSections) {
    write_file(config_file_, R"({"web":{"backend":"sqlite","options":{"path":"/tmp/web.db"}}})");
    const Config td = dztrader::db::resolve_config("td");
    EXPECT_EQ("sqlite", td.backend);
    EXPECT_EQ(dztrader::paths::td_db().string(), option(td, "path"));

    const auto json = nlohmann::json::parse(read_file(config_file_));
    ASSERT_TRUE(json.contains("td"));
    ASSERT_TRUE(json.contains("web"));
    EXPECT_EQ("/tmp/web.db", json["web"]["options"]["path"].get<std::string>());

    const Config web = dztrader::db::resolve_config("web");
    EXPECT_EQ("/tmp/web.db", option(web, "path"));

    const std::string after = read_file(config_file_);
    (void)dztrader::db::resolve_config("td");
    EXPECT_EQ(after, read_file(config_file_));
}

// env 键映射: 合法键小写化; 非法键 (含 '-'、空) 忽略; 空值视为未设置。
TEST_F(DbConfigTest, EnvOptionKeysAreMappedAndInvalidIgnored) {
    dztrader::env::set("DZTRADER_DB_OPT_CACHE_SIZE_KB", "4096");
    dztrader::env::set("DZTRADER_DB_OPT_BAD-KEY", "1");
    dztrader::env::set("DZTRADER_DB_OPT_", "2");
    const Config config = dztrader::db::resolve_config("td");
    EXPECT_EQ("4096", option(config, "cache_size_kb"));
    EXPECT_FALSE(has_option(config, "bad-key"));
    EXPECT_FALSE(has_option(config, ""));

    dztrader::env::set("DZTRADER_DB_OPT_PATH", "");
    const Config empty_value = dztrader::db::resolve_config("td");
    EXPECT_EQ(dztrader::paths::td_db().string(), option(empty_value, "path"));
}

// 未知 backend 原样解析 (resolve_config 不做准入校验), 由 backend_available/Database::open 报错。
TEST_F(DbConfigTest, UnknownBackendIsResolvedButNotAvailable) {
    dztrader::env::set("DZTRADER_DB_BACKEND", "mongo");
    const Config config = dztrader::db::resolve_config("td");
    EXPECT_EQ("mongo", config.backend);
    EXPECT_FALSE(dztrader::db::backend_available("mongo"));
    EXPECT_FALSE(has_option(config, "path"));
}

// 注册缝探针: sqlite 恒可用; mysql 取决于 DZ_WITH_MYSQL 编译开关; 未知名不可用。
TEST_F(DbConfigTest, BackendAvailableFollowsRegistry) {
    EXPECT_TRUE(dztrader::db::backend_available("sqlite"));
    EXPECT_FALSE(dztrader::db::backend_available("nope"));
#if defined(DZ_WITH_MYSQL)
    EXPECT_TRUE(dztrader::db::backend_available("mysql"));
#else
    EXPECT_FALSE(dztrader::db::backend_available("mysql"));
#endif
}

}  // namespace
