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

    // SysmonConfig 七个业务段全量落盘（clock 段由 #296 加入）
    for (const char* section :
         {"sampling", "disk", "network", "history", "realtime", "thresholds", "clock"}) {
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
    EXPECT_FALSE(cfg.contains("log"));  // 只返回七个业务段
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

// ===== #296：`clock` 配置段（契约 sysmon §3.1）=====

// AC1：缺失配置文件的进程首次启动自动生成含 `clock` 段的完整默认配置，零配置可运行。
// 生成器直接产出契约默认值（仓库不提交模板文件），且去掉 log 后能通过类型层校验。
TEST_F(ConfigTest, GenerateDefaultIncludesCompleteClockSection) {
    ASSERT_FALSE(fs::exists(cfg_path_));
    generate_default_config(cfg_path_);
    const auto full = read_json(cfg_path_);

    ASSERT_TRUE(full.contains("clock"));
    const auto& clock = full["clock"];
    EXPECT_TRUE(clock["enabled"].get<bool>());
    EXPECT_EQ(clock["servers"],
              nlohmann::json::array({"ntp.aliyun.com", "ntp.tencent.com", "cn.pool.ntp.org"}));
    EXPECT_EQ(clock["probe_times"],
              nlohmann::json::array({"08:30", "12:30", "15:30", "18:00", "20:30"}));
    EXPECT_EQ(clock["probe_timeout_ms"], 1500);
    EXPECT_EQ(clock["manual_min_interval_sec"], 10);
    EXPECT_EQ(clock["offset_warn_ms"], 100);
    EXPECT_EQ(clock["offset_error_ms"], 1000);
    EXPECT_EQ(clock["bound_alarm_ms"], 200);
    // 零配置可运行 = 生成的完整配置（去掉 log）通过类型层校验。
    nlohmann::json cfg = full;
    cfg.erase("log");
    EXPECT_EQ(platform::SysmonConfig::validate(cfg), std::nullopt);
}

// AC1（旧配置文件）：文件里**缺 clock 段**时按默认值补齐（与其它段一致），不报错、不阻塞启动。
TEST_F(ConfigTest, LoadFillsClockSectionDefaultsWhenMissing) {
    // 只写一个不含 clock 段的旧形状文件（其余段也不全，统一走默认基底 merge）。
    write_json(cfg_path_, nlohmann::json{{"sampling", {{"measure_interval_ms", 500}}}});
    const auto cfg = load_config(cfg_path_);

    EXPECT_EQ(cfg["sampling"]["measure_interval_ms"], 500);  // 用户值保留
    ASSERT_TRUE(cfg.contains("clock"));                      // 缺段按默认补齐
    EXPECT_TRUE(cfg["clock"]["enabled"].get<bool>());
    EXPECT_EQ(cfg["clock"]["probe_timeout_ms"], 1500);
    EXPECT_EQ(cfg["clock"]["offset_warn_ms"], 100);
    EXPECT_EQ(cfg["clock"]["offset_error_ms"], 1000);
    EXPECT_EQ(platform::SysmonConfig::validate(cfg), std::nullopt);
}

// AC2/AC3（持久化侧）：非法 `probe_times` 与 `offset_error_ms <= offset_warn_ms` 的 patch
// 必须被拒绝，且**内存镜像与文件都不变**（四件套的回滚由 store 的强异常安全保证）。
TEST_F(ConfigTest, StoreRejectsInvalidClockPatchAndKeepsOldValues) {
    generate_default_config(cfg_path_);
    SysmonConfigStore store(cfg_path_, load_config(cfg_path_));

    for (const auto& patch : {
             nlohmann::json{{"clock", {{"probe_times", {"25:00"}}}}},
             nlohmann::json{{"clock", {{"probe_times", {"8:3"}}}}},
             nlohmann::json{{"clock", {{"probe_times", {""}}}}},
             nlohmann::json{{"clock", {{"offset_error_ms", 100}}}},  // 等于 warn
             nlohmann::json{{"clock", {{"offset_error_ms", 50}}}},   // 小于 warn
             nlohmann::json{{"clock", {{"servers", nlohmann::json::array()}}}},
         }) {
        EXPECT_THROW(store.set_patch(patch), std::runtime_error) << patch.dump();
        // 回滚：镜像与文件都保持默认值。
        EXPECT_EQ(store.current()["clock"]["probe_times"],
                  (nlohmann::json::array({"08:30", "12:30", "15:30", "18:00", "20:30"})));
        EXPECT_EQ(store.current()["clock"]["offset_error_ms"], 1000);
        EXPECT_EQ(read_json(cfg_path_)["clock"]["offset_error_ms"], 1000);
    }
}

// 运行期修改 clock 段：成功时持久化到文件、其它 section 原样保留，重新加载后仍生效。
TEST_F(ConfigTest, StorePersistsClockPatchAndSurvivesReload) {
    generate_default_config(cfg_path_);
    auto full = read_json(cfg_path_);
    full["custom"]["keep"] = 1;
    write_json(cfg_path_, full);
    SysmonConfigStore store(cfg_path_, load_config(cfg_path_));

    store.set_patch(nlohmann::json{{"clock",
                                    {{"enabled", false},
                                     {"probe_times", nlohmann::json::array({"06:15", "22:45"})},
                                     {"servers", nlohmann::json::array({"10.0.0.1"})},
                                     {"probe_timeout_ms", 3000}}}});

    EXPECT_FALSE(store.current()["clock"]["enabled"].get<bool>());
    const auto persisted = read_json(cfg_path_);
    EXPECT_FALSE(persisted["clock"]["enabled"].get<bool>());
    EXPECT_EQ(persisted["clock"]["probe_times"], (nlohmann::json::array({"06:15", "22:45"})));
    EXPECT_EQ(persisted["log"]["level"], "info");  // log section 保留
    EXPECT_EQ(persisted["custom"]["keep"], 1);     // 未知 section 保留

    // 重新加载：持久化的值生效，未提及的键保持默认。
    const auto reloaded = load_config(cfg_path_);
    EXPECT_FALSE(reloaded["clock"]["enabled"].get<bool>());
    EXPECT_EQ(reloaded["clock"]["probe_timeout_ms"], 3000);
    EXPECT_EQ(reloaded["clock"]["offset_warn_ms"], 100);  // 未提及 → 默认
    EXPECT_EQ(platform::SysmonConfig::validate(reloaded), std::nullopt);
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

}  // namespace
}  // namespace dztrader::sysmon
