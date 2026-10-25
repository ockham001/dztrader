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
    for (const char* section :
         {"sampling", "disk", "network", "history", "realtime", "thresholds"}) {
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

// ===== #123：旧 `alerts` 段迁移（契约 sysmon §3.1）=====

// 最完整的旧形状 → 新形状：可映射的键全部搬过去，已删除的键丢弃，旧段消失。
TEST_F(ConfigTest, LegacyAlertsSectionMigratesToThresholds) {
    write_json(cfg_path_, nlohmann::json{
                              {"log", {{"level", "debug"}}},
                              {"alerts",
                               {{"cpu_sustain_used_pct", 91.5},
                                {"cpu_sustain_samples", 7},
                                {"memory_used_pct", 80.0},
                                {"swap_used_pct", 40.0},
                                {"volume_free_pct", 5.0},
                                {"volume_free_bytes", 1'073'741'824},
                                {"cooldown_sec", 300}}},
                          });

    const auto cfg = load_config(cfg_path_);

    // 返回值是**新形状**且已通过校验（迁移不报错、不阻塞启动）。
    EXPECT_EQ(platform::SysmonConfig::validate(cfg), std::nullopt);
    ASSERT_TRUE(cfg.contains("thresholds"));
    EXPECT_DOUBLE_EQ(cfg["thresholds"]["cpu_sustain_used_pct"].get<double>(), 91.5);
    EXPECT_EQ(cfg["thresholds"]["cpu_sustain_samples"], 7);
    EXPECT_DOUBLE_EQ(cfg["thresholds"]["memory_used_pct"].get<double>(), 80.0);
    EXPECT_DOUBLE_EQ(cfg["thresholds"]["swap_used_pct"].get<double>(), 40.0);
    EXPECT_DOUBLE_EQ(cfg["thresholds"]["volume_free_pct"].get<double>(), 5.0);
    // 不可映射的键丢弃；缺失的新键（log_repeat_sec）补默认值。
    EXPECT_FALSE(cfg["thresholds"].contains("volume_free_bytes"));
    EXPECT_FALSE(cfg["thresholds"].contains("cooldown_sec"));
    EXPECT_EQ(cfg["thresholds"]["log_repeat_sec"], 600);
    EXPECT_FALSE(cfg.contains("alerts"));

    // 文件也改写为新形状（下次启动不再重复迁移），且 log 等其它 section 原样保留。
    const auto persisted = read_json(cfg_path_);
    EXPECT_FALSE(persisted.contains("alerts"));
    ASSERT_TRUE(persisted.contains("thresholds"));
    EXPECT_DOUBLE_EQ(persisted["thresholds"]["cpu_sustain_used_pct"].get<double>(), 91.5);
    EXPECT_FALSE(persisted["thresholds"].contains("volume_free_bytes"));
    EXPECT_EQ(persisted["log"]["level"], "debug");
}

// 只含部分键的旧形状：搬能搬的，其余补默认值（不得因为旧段不完整而校验失败）。
TEST_F(ConfigTest, LegacyPartialAlertsSectionMigratesWithDefaults) {
    write_json(cfg_path_, nlohmann::json{{"alerts", {{"memory_used_pct", 75.0}}}});

    const auto cfg = load_config(cfg_path_);
    EXPECT_EQ(platform::SysmonConfig::validate(cfg), std::nullopt);
    EXPECT_DOUBLE_EQ(cfg["thresholds"]["memory_used_pct"].get<double>(), 75.0);
    // 其余键都是默认值（旧段没有 → 默认基底补上）。
    EXPECT_DOUBLE_EQ(cfg["thresholds"]["swap_used_pct"].get<double>(), 50.0);
    EXPECT_EQ(cfg["thresholds"]["log_repeat_sec"], 600);
    EXPECT_FALSE(read_json(cfg_path_).contains("alerts"));
}

// 旧段含未知键：未知键丢弃、不阻塞（总则 §8：未知字段忽略），已知键照常迁移。
TEST_F(ConfigTest, LegacyAlertsSectionWithUnknownKeysDropsThem) {
    write_json(cfg_path_, nlohmann::json{
                              {"alerts",
                               {{"memory_used_pct", 70.0},
                                {"unknown_future_key", 42},
                                {"another_legacy_thing", "x"}}},
                          });

    const auto cfg = load_config(cfg_path_);
    EXPECT_EQ(platform::SysmonConfig::validate(cfg), std::nullopt);
    EXPECT_DOUBLE_EQ(cfg["thresholds"]["memory_used_pct"].get<double>(), 70.0);
    EXPECT_FALSE(cfg["thresholds"].contains("unknown_future_key"));
    EXPECT_FALSE(cfg["thresholds"].contains("another_legacy_thing"));
    EXPECT_FALSE(cfg.contains("alerts"));
}

// 新旧段同时存在时**新段优先**（用户可能已经手工写好新形状，不能被旧值覆盖）。
TEST_F(ConfigTest, ExistingThresholdsSectionWinsOverLegacyAlerts) {
    write_json(cfg_path_, nlohmann::json{
                              {"alerts", {{"memory_used_pct", 11.0}, {"swap_used_pct", 22.0}}},
                              {"thresholds", {{"memory_used_pct", 88.0}}},
                          });

    const auto cfg = load_config(cfg_path_);
    EXPECT_DOUBLE_EQ(cfg["thresholds"]["memory_used_pct"].get<double>(), 88.0);  // 新段优先
    EXPECT_DOUBLE_EQ(cfg["thresholds"]["swap_used_pct"].get<double>(), 22.0);    // 新段没写的才搬
    EXPECT_FALSE(cfg.contains("alerts"));
}

// 没有旧段时迁移是**纯无操作**：不 sanitize、不重写文件（避免每次启动都改用户的文件）。
TEST_F(ConfigTest, NoLegacySectionLeavesFileUntouched) {
    generate_default_config(cfg_path_);
    const auto before = read_json(cfg_path_);
    auto marker = before;
    marker["realtime"]["push_interval_sec"] = 9;
    marker["custom"]["keep"] = 1;
    write_json(cfg_path_, marker);

    (void)load_config(cfg_path_);
    const auto after = read_json(cfg_path_);
    EXPECT_EQ(after["realtime"]["push_interval_sec"], 9);
    EXPECT_EQ(after["custom"]["keep"], 1);
    EXPECT_TRUE(after.contains("thresholds"));
    EXPECT_FALSE(after.contains("alerts"));
}

// 迁移单元（不经文件）：直接作用于整文件 JSON，报告映射与丢弃了哪些键。
TEST_F(ConfigTest, MigrateLegacyAlertsReportsMappedAndDroppedKeys) {
    nlohmann::json full = {
        {"alerts",
         {{"cpu_sustain_used_pct", 91.0},
          {"memory_used_pct", 80.0},
          {"volume_free_bytes", 1'073'741'824},
          {"cooldown_sec", 300},
          {"unknown_key", 1}}},
        {"log", {{"level", "info"}}},
    };

    const auto migration = migrate_legacy_alerts(full);
    EXPECT_TRUE(migration.migrated);
    EXPECT_EQ(migration.mapped_keys,
              (std::vector<std::string>{"cpu_sustain_used_pct", "memory_used_pct"}));
    EXPECT_EQ(migration.dropped_keys,
              (std::vector<std::string>{"volume_free_bytes", "cooldown_sec"}));
    EXPECT_FALSE(full.contains("alerts"));
    EXPECT_DOUBLE_EQ(full["thresholds"]["cpu_sustain_used_pct"].get<double>(), 91.0);
    EXPECT_EQ(full["log"]["level"], "info");  // 其它 section 不受影响

    // 幂等：再跑一次不产生任何变化、也不报告迁移（下次启动不会重复迁移）。
    const auto again = migrate_legacy_alerts(full);
    EXPECT_FALSE(again.migrated);
    EXPECT_TRUE(again.mapped_keys.empty());
    EXPECT_TRUE(again.dropped_keys.empty());
}

// 默认配置生成器直接产出新形状（仓库不提交模板；新装机器不应写出旧段）。
TEST_F(ConfigTest, DefaultConfigProducesThresholdsNotAlerts) {
    generate_default_config(cfg_path_);
    const auto full = read_json(cfg_path_);
    EXPECT_FALSE(full.contains("alerts"));
    ASSERT_TRUE(full.contains("thresholds"));
    EXPECT_EQ(full["thresholds"]["log_repeat_sec"], 600);
    EXPECT_DOUBLE_EQ(full["thresholds"]["volume_free_pct"].get<double>(), 10.0);
    EXPECT_FALSE(full["thresholds"].contains("volume_free_bytes"));
    EXPECT_FALSE(full["thresholds"].contains("cooldown_sec"));
}

// 运行期 patch 只接受新段：旧段名不再被识别（会被当成未知字段忽略），因此阈值改不动。
TEST_F(ConfigTest, RuntimePatchOnLegacySectionNameIsIgnored) {
    generate_default_config(cfg_path_);
    SysmonConfigStore store(cfg_path_, load_config(cfg_path_));
    store.set_patch(nlohmann::json{{"alerts", {{"memory_used_pct", 42.0}}}});
    EXPECT_DOUBLE_EQ(store.current()["thresholds"]["memory_used_pct"].get<double>(), 90.0);

    store.set_patch(nlohmann::json{{"thresholds", {{"memory_used_pct", 42.0}}}});
    EXPECT_DOUBLE_EQ(store.current()["thresholds"]["memory_used_pct"].get<double>(), 42.0);
}

// #75：`disk.watch_paths` 是关注路径的配置来源（契约 sysmon §3.1）。
// 这两个用例锁定「配置非空则以其为准、为空则回退默认路径」的契约分支；
// 默认值（空数组）也必须能从默认配置生成出来，否则运行期永远走不到配置分支。
TEST_F(ConfigTest, DiskWatchPathsDefaultToEmptyMeaningRealDefaults) {
    generate_default_config(cfg_path_);
    const auto cfg = load_config(cfg_path_);
    const auto typed = cfg.get<dztrader::platform::SysmonConfig>();

    // 默认值为空数组：表示"使用默认关注路径（共享内存目录与数据库目录）"，
    // 语义见契约 sysmon §3.1 —— 不是"不监控任何路径"。
    EXPECT_TRUE(typed.disk.watch_paths.empty());
    EXPECT_TRUE(typed.disk.devices.empty());
}

TEST_F(ConfigTest, DiskWatchPathsRoundTripThroughSetPatch) {
    generate_default_config(cfg_path_);
    SysmonConfigStore store(cfg_path_, load_config(cfg_path_));

    // 运行期配置非空关注路径：必须持久化并在内存镜像里生效（#81 的前置能力）。
    store.set_patch(
        nlohmann::json{{"disk", {{"watch_paths", nlohmann::json::array({"/srv/a", "/srv/b"})}}}});
    EXPECT_EQ(store.current()["disk"]["watch_paths"], nlohmann::json::array({"/srv/a", "/srv/b"}));

    // 重新加载（模拟进程重启）后仍在：配置是真源，不依赖进程内存。
    const auto reloaded = load_config(cfg_path_);
    const auto typed = reloaded.get<dztrader::platform::SysmonConfig>();
    ASSERT_EQ(typed.disk.watch_paths.size(), 2U);
    EXPECT_EQ(typed.disk.watch_paths[0], "/srv/a");
    EXPECT_EQ(typed.disk.watch_paths[1], "/srv/b");

    // 空元素非法（契约 sysmon §3.1：元素非空）。
    EXPECT_THROW(store.set_patch(nlohmann::json{{"disk", {{"watch_paths", {""}}}}}),
                 std::runtime_error);
}

}  // namespace
}  // namespace dztrader::sysmon
