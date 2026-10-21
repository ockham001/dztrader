#include "config.h"

#include <dztrader/core/this_process.h>
#include <dztrader/platform/sysmon.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

namespace dztrader::sysmon {
namespace {

namespace fs = std::filesystem;

/// 配置往返/持久化测试：每个用例独立临时目录（ctest -j 下互不干扰）。
class ConfigTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;
    fs::path cfg_path_;

    void SetUp() override {
        std::random_device rd;
        tmp_dir_ = fs::temp_directory_path() /
                   ("dz_sysmon_config_test_" +
                    std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
                    std::to_string(rd()));
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
        fs::create_directories(tmp_dir_);
        cfg_path_ = tmp_dir_ / "dzsysmon.json";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    static nlohmann::json read_json(const fs::path& path) {
        std::ifstream ifs(path);
        nlohmann::json value;
        ifs >> value;
        return value;
    }

    static void write_json(const fs::path& path, const nlohmann::json& value) {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        ofs << value.dump(2);
    }
};

TEST_F(ConfigTest, GenerateDefaultWritesCompleteConfig) {
    generate_default_config(cfg_path_);
    ASSERT_TRUE(fs::exists(cfg_path_));
    const auto full = read_json(cfg_path_);

    // log section：生产默认 info/warning
    ASSERT_TRUE(full.contains("log"));
    EXPECT_EQ(full["log"]["level"], "info");
    EXPECT_EQ(full["log"]["flush_on"], "warning");

    // SysmonConfig 六个业务段全量落盘
    for (const char* section : {"sampling", "disk", "network", "history", "realtime", "alerts"}) {
        EXPECT_TRUE(full.contains(section)) << section;
    }
    EXPECT_EQ(full["sampling"]["measure_interval_ms"], 1000);
    EXPECT_EQ(full["sampling"]["record_interval_sec"], 60);
    EXPECT_EQ(full["realtime"]["push_interval_sec"], 5);
    EXPECT_EQ(full["history"]["max_bytes"].get<uint64_t>(), 2'147'483'648ULL);
    EXPECT_EQ(full["network"]["hide_virtual"], true);

    // 去掉 log 后应通过平台类型层校验
    nlohmann::json cfg = full;
    cfg.erase("log");
    EXPECT_EQ(platform::SysmonConfig::validate(cfg), std::nullopt);
}

TEST_F(ConfigTest, GenerateDefaultDoesNotOverwriteExisting) {
    generate_default_config(cfg_path_);
    auto edited = read_json(cfg_path_);
    edited["realtime"]["push_interval_sec"] = 9;
    write_json(cfg_path_, edited);

    generate_default_config(cfg_path_);
    EXPECT_EQ(read_json(cfg_path_)["realtime"]["push_interval_sec"], 9);
}

TEST_F(ConfigTest, LoadFillsDefaultsForMissingFields) {
    write_json(cfg_path_, nlohmann::json{{"sampling", {{"measure_interval_ms", 500}}}});
    const auto cfg = load_config(cfg_path_);

    EXPECT_EQ(cfg["sampling"]["measure_interval_ms"], 500);  // 用户值保留
    EXPECT_EQ(cfg["sampling"]["record_interval_sec"], 60);   // 其余字段补默认
    EXPECT_EQ(cfg["realtime"]["push_interval_sec"], 5);
    EXPECT_FALSE(cfg.contains("log"));  // 只返回六个业务段
    EXPECT_EQ(platform::SysmonConfig::validate(cfg), std::nullopt);
}

TEST_F(ConfigTest, LoadGeneratesDefaultWhenMissing) {
    ASSERT_FALSE(fs::exists(cfg_path_));
    const auto cfg = load_config(cfg_path_);

    EXPECT_TRUE(fs::exists(cfg_path_));  // 缺失自愈
    EXPECT_EQ(cfg["sampling"]["measure_interval_ms"], 1000);
}

TEST_F(ConfigTest, LoadRejectsOutOfRangeValue) {
    write_json(cfg_path_, nlohmann::json{{"sampling", {{"measure_interval_ms", 50}}}});
    try {
        (void)load_config(cfg_path_);
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("measure_interval_ms"), std::string::npos);
    }
}

TEST_F(ConfigTest, LoadRejectsBrokenJson) {
    {
        std::ofstream ofs(cfg_path_, std::ios::binary | std::ios::trunc);
        ofs << "{not valid json";
    }
    EXPECT_THROW((void)load_config(cfg_path_), std::runtime_error);
}

TEST_F(ConfigTest, LoadRejectsNonObjectRoot) {
    write_json(cfg_path_, nlohmann::json::array({1, 2, 3}));
    EXPECT_THROW((void)load_config(cfg_path_), std::runtime_error);
}

TEST_F(ConfigTest, StorePatchMergesNestedObjects) {
    generate_default_config(cfg_path_);
    SysmonConfigStore store(cfg_path_, load_config(cfg_path_));

    store.set_patch(nlohmann::json{{"sampling", {{"groups", {{"cpu", false}}}}}});
    const auto groups = store.current()["sampling"]["groups"];
    EXPECT_EQ(groups["cpu"], false);
    EXPECT_EQ(groups["memory"], true);  // 未提到的键保持原值
}

TEST_F(ConfigTest, StoreSetPatchPersistsAndPreservesOtherSections) {
    generate_default_config(cfg_path_);
    auto full = read_json(cfg_path_);
    full["custom"]["keep"] = 1;  // 未知 section 必须保留
    write_json(cfg_path_, full);

    SysmonConfigStore store(cfg_path_, load_config(cfg_path_));
    store.set_patch(nlohmann::json{{"realtime", {{"push_interval_sec", 7}}}});

    EXPECT_EQ(store.current()["realtime"]["push_interval_sec"], 7);
    const auto persisted = read_json(cfg_path_);
    EXPECT_EQ(persisted["realtime"]["push_interval_sec"], 7);
    EXPECT_EQ(persisted["log"]["level"], "info");  // log section 保留
    EXPECT_EQ(persisted["custom"]["keep"], 1);     // 未知 section 保留
    EXPECT_EQ(load_config(cfg_path_)["realtime"]["push_interval_sec"], 7);
}

TEST_F(ConfigTest, StoreRejectsInvalidPatchAndKeepsMemory) {
    generate_default_config(cfg_path_);
    SysmonConfigStore store(cfg_path_, load_config(cfg_path_));

    EXPECT_THROW(store.set_patch(nlohmann::json{{"realtime", {{"push_interval_sec", 0}}}}),
                 std::runtime_error);
    EXPECT_EQ(store.current()["realtime"]["push_interval_sec"], 5);       // 内存不变
    EXPECT_EQ(read_json(cfg_path_)["realtime"]["push_interval_sec"], 5);  // 文件不变
}

TEST_F(ConfigTest, StoreRejectsNullPatch) {
    generate_default_config(cfg_path_);
    SysmonConfigStore store(cfg_path_, load_config(cfg_path_));
    EXPECT_THROW(store.set_patch(nlohmann::json{{"realtime", nullptr}}), std::runtime_error);
    EXPECT_EQ(store.current()["realtime"]["push_interval_sec"], 5);
}

TEST_F(ConfigTest, StorePersistFailureKeepsMemory) {
    generate_default_config(cfg_path_);
    SysmonConfigStore store(cfg_path_, load_config(cfg_path_));

    // 持久化阶段 load-modify-save 读文件失败（文件被外部改坏）→ 抛异常且内存回滚
    {
        std::ofstream ofs(cfg_path_, std::ios::binary | std::ios::trunc);
        ofs << "{broken";
    }
    EXPECT_THROW(store.set_patch(nlohmann::json{{"realtime", {{"push_interval_sec", 8}}}}),
                 std::runtime_error);
    EXPECT_EQ(store.current()["realtime"]["push_interval_sec"], 5);
}

}  // namespace
}  // namespace dztrader::sysmon
