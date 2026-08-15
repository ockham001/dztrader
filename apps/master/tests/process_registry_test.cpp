/**
 * @file process_registry_test.cpp
 * @brief Unit tests for ProcessRegistry (json load + find_exe_by_stem).
 */

#include "process_registry.h"

#include <dztrader/core/env.h>
#include <dztrader/core/exception.h>

#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>

namespace dztrader::master {
namespace {

class ProcessRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "master_registry_test";
        std::filesystem::remove_all(tmp_dir_);
        std::filesystem::create_directories(tmp_dir_);
        // Save original DZTRADER_HOME
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
    }

    void TearDown() override {
        // Restore DZTRADER_HOME
        if (orig_home_) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        std::filesystem::remove_all(tmp_dir_);
    }

    void set_home(const std::filesystem::path& home) {
        dztrader::env::set("DZTRADER_HOME", home.string());
    }

    void write_json(const std::string& content) {
        config_path_ = tmp_dir_ / "dztraderd.json";
        std::ofstream ofs(config_path_);
        ofs << content;
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path config_path_;
    std::optional<std::string> orig_home_;
};

TEST_F(ProcessRegistryTest, EmptyDirectoryNoError) {
    set_home(tmp_dir_);
    write_json(R"({"master": {}})");

    ProcessRegistry registry;
    // app_root() 锚点固定为构建目录；本测试验证最小配置下 load() 不抛异常（"NoError" 语义）。
    EXPECT_NO_THROW(registry.load(config_path_));
}

TEST_F(ProcessRegistryTest, StrategyFromConfig) {
    set_home(tmp_dir_);
    write_json(R"({
        "master": {},
        "strategy": [
            {"name": "test_strat", "exe": "/path/to/strat"}
        ]
    })");

    ProcessRegistry registry;
    registry.load(config_path_);

    auto* strat = registry.find("test_strat");
    ASSERT_NE(strat, nullptr);
    EXPECT_EQ(strat->category, Category::Strategy);
    EXPECT_FALSE(strat->restart.enabled);
}

TEST_F(ProcessRegistryTest, FindNonExistent) {
    set_home(tmp_dir_);
    write_json(R"({"master": {}})");

    ProcessRegistry registry;
    registry.load(config_path_);

    EXPECT_EQ(registry.find("nonexistent"), nullptr);
}

// update_display_name: 找到条目并更新, 同值不重复更新, 未找到无操作
TEST_F(ProcessRegistryTest, UpdateDisplayName) {
    set_home(tmp_dir_);
    write_json(R"({
        "master": {},
        "md": {
            "dzmd_ctp": {
                "exe": "dzmd_ctp",
                "args": ["--name", "dzmd_ctp"]
            }
        }
    })");

    ProcessRegistry registry;
    registry.load(config_path_);

    // 初始: display_name 为空 (json 中未声明)
    const auto* entry = registry.find("dzmd_ctp");
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(entry->display_name.empty());

    // 更新 display_name
    EXPECT_TRUE(registry.update_display_name("dzmd_ctp", "CTP期货行情"));
    EXPECT_EQ(registry.find("dzmd_ctp")->display_name, "CTP期货行情");

    // 同值再更新: 返回 false (无实际变更)
    EXPECT_FALSE(registry.update_display_name("dzmd_ctp", "CTP期货行情"));

    // 未找到条目: 返回 false, 无副作用
    EXPECT_FALSE(registry.update_display_name("nonexistent", "whatever"));
}

TEST_F(ProcessRegistryTest, RegisterUnregisterStrategy) {
    ProcessRegistry registry;

    ProcessEntry entry;
    entry.name = "dynamic_strat";
    entry.category = Category::Strategy;
    entry.exe = "/path/to/strat";
    entry.restart = default_restart_policy(Category::Strategy);

    registry.register_strategy(entry);
    ASSERT_NE(registry.find("dynamic_strat"), nullptr);

    registry.unregister("dynamic_strat");
    EXPECT_EQ(registry.find("dynamic_strat"), nullptr);
}

TEST_F(ProcessRegistryTest, RegisterDuplicateThrows) {
    ProcessRegistry registry;

    ProcessEntry entry;
    entry.name = "dup_strat";
    entry.category = Category::Strategy;
    entry.exe = "/path/to/strat";

    registry.register_strategy(entry);
    ASSERT_NE(registry.find("dup_strat"), nullptr);

    // 重复注册应抛出 dztrader::Exception（DZ_EC_ALREADY_EXISTS）
    EXPECT_THROW(registry.register_strategy(entry), dztrader::Exception);
}

}  // namespace
}  // namespace dztrader::master
