/**
 * @file config_test.cpp
 * @brief Unit tests for config parsing (Category, RestartPolicy, ProcessEntry, JSON).
 */

#include "config.h"

#include <dztrader/core/env.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace dztrader::master {
namespace {

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "master_config_test";
        std::filesystem::create_directories(tmp_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

    void write_json(const std::string& content) {
        config_path_ = tmp_dir_ / "dztraderd.json";
        std::ofstream ofs(config_path_);
        ofs << content;
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path config_path_;
};

TEST_F(ConfigTest, CategoryStr) {
    EXPECT_STREQ(category_str(Category::GatewayMd), "md");
    EXPECT_STREQ(category_str(Category::GatewayTd), "td");
    EXPECT_STREQ(category_str(Category::Strategy), "stg");
    EXPECT_STREQ(category_str(Category::WebUI), "webui");
}

TEST_F(ConfigTest, DefaultRestartPolicy) {
    auto md_policy = default_restart_policy(Category::GatewayMd);
    EXPECT_TRUE(md_policy.enabled);
    EXPECT_EQ(md_policy.max_attempts, 5);
    EXPECT_EQ(md_policy.backoff_sec, 5);

    auto td_policy = default_restart_policy(Category::GatewayTd);
    EXPECT_TRUE(td_policy.enabled);
    EXPECT_EQ(td_policy.max_attempts, 5);

    auto strat_policy = default_restart_policy(Category::Strategy);
    EXPECT_FALSE(strat_policy.enabled);
    EXPECT_EQ(strat_policy.max_attempts, 0);

    auto webui_policy = default_restart_policy(Category::WebUI);
    EXPECT_TRUE(webui_policy.enabled);
    EXPECT_EQ(webui_policy.max_attempts, 5);
    EXPECT_EQ(webui_policy.backoff_sec, 5);
}

TEST_F(ConfigTest, ParseMinimalJson) {
    write_json(R"({"master": {}})");
    auto cfg = parse_master_json(config_path_);
    EXPECT_EQ(cfg.master.single_stop_timeout_sec, 3);
    EXPECT_TRUE(cfg.entries.empty());
}

// ---- F1: remove_gateway_section 删除指定网关段 ----
TEST_F(ConfigTest, RemoveGatewaySectionDeletesEntry) {
    write_json(R"({
        "master": {},
        "md": {
            "dzmd_ctp": {
                "exe": "dzmd_ctp",
                "args": [],
                "start_dir": ".",
                "restart": {"enabled": true, "max_attempts": 5, "backoff_sec": 5}
            }
        },
        "td": {
            "dztd_ctp": {"exe": "dztd_ctp", "args": []}
        }
    })");
    remove_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp");
    auto cfg = parse_master_json(config_path_);
    EXPECT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "dztd_ctp");
}

TEST_F(ConfigTest, RemoveGatewaySectionIdempotent) {
    write_json(R"({"master": {}})");
    // 不含 md 段, remove 应无操作不抛异常
    EXPECT_NO_THROW(remove_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp"));
}

TEST_F(ConfigTest, RemoveGatewaySectionPreservesOthers) {
    write_json(R"({
        "master": {},
        "md": {"dzmd_ctp": {"exe": "dzmd_ctp"}}
    })");
    remove_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp");
    auto cfg = parse_master_json(config_path_);
    EXPECT_TRUE(cfg.entries.empty());
    EXPECT_EQ(cfg.master.single_stop_timeout_sec, 3);  // master 段保留
}

// ---- F2: write_gateway_section 写入网关段 ----
TEST_F(ConfigTest, WriteGatewaySectionCreatesNew) {
    write_json(R"({"master": {}})");
    platform::RestartPolicy policy{.enabled = true, .max_attempts = 5, .backoff_sec = 5};
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp",
                          {"--name", "dzmd_ctp"}, policy);
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "dzmd_ctp");
    // 契约 process: 内部进程 exe 和 start_dir 不持久化到 json, parse 不读取, 留空由 launch_child 调 find_exe_by_stem 实时填充
    EXPECT_TRUE(cfg.entries[0].exe.empty());
    EXPECT_TRUE(cfg.entries[0].start_dir.empty());
    EXPECT_EQ(cfg.entries[0].args.size(), 2u);
}

TEST_F(ConfigTest, WriteGatewaySectionOverwritesExisting) {
    write_json(R"({
        "master": {},
        "md": {"dzmd_ctp": {"exe": "old_exe", "args": ["old_arg"]}}
    })");
    platform::RestartPolicy policy{.enabled = false, .max_attempts = 0, .backoff_sec = 5};
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp",
                          {"--name", "dzmd_ctp"}, policy);
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    // 契约 process: exe 不持久化, 旧 json 中的 "old_exe" 被静默忽略, parse 不读取
    EXPECT_TRUE(cfg.entries[0].exe.empty());
    EXPECT_TRUE(cfg.entries[0].start_dir.empty());
    EXPECT_EQ(cfg.entries[0].args.size(), 2u);
    EXPECT_FALSE(cfg.entries[0].restart.enabled);
}

// ---- F2: write_webui_section 写入 [webui] 段 (契约 process: dzweb 写 webui 而非 md.dzweb) ----
TEST_F(ConfigTest, WriteWebuiSectionCreatesNew) {
    write_json(R"({"master": {}})");
    platform::RestartPolicy policy{.enabled = true, .max_attempts = 5, .backoff_sec = 5};
    write_webui_section(config_path_, {"--name", "dzweb"}, policy);
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "dzweb");
    EXPECT_EQ(cfg.entries[0].category, Category::WebUI);
    // 契约 process: dzweb 的 exe 和 start_dir 不持久化到 json, parse 不读取, 留空由 launch_child 调 find_exe_by_stem 实时填充
    EXPECT_TRUE(cfg.entries[0].exe.empty());
    EXPECT_TRUE(cfg.entries[0].start_dir.empty());
    EXPECT_EQ(cfg.entries[0].args.size(), 2u);

    // 直接解析 json 验证段名是 webui 而非 md.dzweb
    std::ifstream ifs(config_path_);
    nlohmann::json data;
    ifs >> data;
    ASSERT_TRUE(data.contains("webui"));
    EXPECT_FALSE(data.contains("md"));
    EXPECT_FALSE(data.contains("td"));
    EXPECT_FALSE(data.contains("gateways"));
    // 契约 process: webui 段不含 exe 和 start_dir 字段
    const auto& webui = data["webui"];
    EXPECT_FALSE(webui.contains("exe"));
    EXPECT_FALSE(webui.contains("start_dir"));
}

TEST_F(ConfigTest, WriteWebuiSectionOverwritesExisting) {
    write_json(R"({
        "master": {},
        "webui": {"exe": "old_exe", "args": ["old_arg"], "start_dir": "."}
    })");
    platform::RestartPolicy policy{.enabled = false, .max_attempts = 0, .backoff_sec = 5};
    write_webui_section(config_path_, {"--name", "dzweb"}, policy);
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    // 契约 process: exe 不持久化, 旧 json 中的 "old_exe" 被静默忽略, parse 不读取
    EXPECT_TRUE(cfg.entries[0].exe.empty());
    EXPECT_TRUE(cfg.entries[0].start_dir.empty());
    EXPECT_EQ(cfg.entries[0].args.size(), 2u);
    EXPECT_FALSE(cfg.entries[0].restart.enabled);

    // 直接解析 json 验证 webui 段不含 exe 和 start_dir 字段 (被覆盖后不再保留旧字段)
    std::ifstream ifs(config_path_);
    nlohmann::json data;
    ifs >> data;
    ASSERT_TRUE(data.contains("webui"));
    const auto& webui = data["webui"];
    EXPECT_FALSE(webui.contains("exe"));
    EXPECT_FALSE(webui.contains("start_dir"));
}

// display_name 持久化往返: write 非空 display_name → parse 读回应一致
TEST_F(ConfigTest, WriteGatewaySectionPersistsDisplayName) {
    write_json(R"({"master": {}})");
    platform::RestartPolicy policy{.enabled = true, .max_attempts = 5, .backoff_sec = 5};
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp",
                          {"--name", "dzmd_ctp"}, policy,
                          "CTP期货行情");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].display_name, "CTP期货行情");
}

// display_name 为空时: 不写入 json 字段, parse 读回为空串 (向后兼容旧 json)
TEST_F(ConfigTest, WriteGatewaySectionEmptyDisplayNameSkipped) {
    write_json(R"({"master": {}})");
    platform::RestartPolicy policy{.enabled = true, .max_attempts = 5, .backoff_sec = 5};
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp",
                          {"--name", "dzmd_ctp"}, policy,
                          "");  // 空 display_name
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_TRUE(cfg.entries[0].display_name.empty());
}

// 旧 json 无 display_name 字段: parse 用 value 默认空串, 向后兼容
TEST_F(ConfigTest, ParseJsonWithoutDisplayNameDefaultsEmpty) {
    write_json(R"({
        "master": {},
        "md": {"dzmd_ctp": {"exe": "dzmd_ctp", "args": ["--name", "dzmd_ctp"]}}
    })");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_TRUE(cfg.entries[0].display_name.empty());
}

// ---- F1+F2 往返: write → remove → 不存在 ----
TEST_F(ConfigTest, WriteThenRemoveGatewaySection) {
    write_json(R"({"master": {}})");
    platform::RestartPolicy policy{.enabled = true, .max_attempts = 5, .backoff_sec = 5};
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp",
                          {"--name", "dzmd_ctp"}, policy);
    auto cfg1 = parse_master_json(config_path_);
    EXPECT_EQ(cfg1.entries.size(), 1u);

    remove_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp");
    auto cfg2 = parse_master_json(config_path_);
    EXPECT_TRUE(cfg2.entries.empty());
    // master 段保留
    EXPECT_EQ(cfg2.master.single_stop_timeout_sec, 3);
}

TEST_F(ConfigTest, ParseGatewayEntries) {
    write_json(R"({
        "master": {},
        "md": {
            "dzmd_ctp": {
                "exe": "dzmd_ctp",
                "args": ["--verbose"],
                "env": {"LOG_LEVEL": "debug"},
                "restart": {"max_attempts": 5, "backoff_sec": 5}
            }
        },
        "td": {
            "dztd_ctp": {"exe": "dztd_ctp"}
        }
    })");
    auto cfg = parse_master_json(config_path_);
    EXPECT_EQ(cfg.entries.size(), 2u);

    // Find dzmd_ctp
    const ProcessEntry* md = nullptr;
    const ProcessEntry* td = nullptr;
    for (const auto& e : cfg.entries) {
        if (e.name == "dzmd_ctp") md = &e;
        if (e.name == "dztd_ctp") td = &e;
    }
    ASSERT_NE(md, nullptr);
    ASSERT_NE(td, nullptr);

    EXPECT_EQ(md->category, Category::GatewayMd);
    EXPECT_EQ(md->args.size(), 1u);
    EXPECT_EQ(md->args[0], "--verbose");
    EXPECT_TRUE(md->env.contains("LOG_LEVEL"));
    EXPECT_TRUE(md->restart.enabled);

    EXPECT_EQ(td->category, Category::GatewayTd);
    EXPECT_TRUE(td->args.empty());
    EXPECT_TRUE(td->restart.enabled);
}

TEST_F(ConfigTest, ParseStrategyEntries) {
    write_json(R"({
        "master": {},
        "strategy": [
            {
                "name": "my_strategy",
                "exe": "/home/user/strategies/my_strategy",
                "args": ["--config", "strategy.toml"]
            }
        ]
    })");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);

    const auto& s = cfg.entries[0];
    EXPECT_EQ(s.name, "my_strategy");
    EXPECT_EQ(s.category, Category::Strategy);
    EXPECT_EQ(s.args.size(), 2u);
    EXPECT_FALSE(s.restart.enabled);  // strategy default: no restart
}

TEST_F(ConfigTest, ParseStrategyWithRestart) {
    write_json(R"({
        "strategy": [
            {
                "name": "auto_restart_strat",
                "exe": "/path/to/strat",
                "restart": {"enabled": true, "max_attempts": 3, "backoff_sec": 10}
            }
        ]
    })");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);

    const auto& s = cfg.entries[0];
    EXPECT_TRUE(s.restart.enabled);
    EXPECT_EQ(s.restart.max_attempts, 3);
    EXPECT_EQ(s.restart.backoff_sec, 10);
}

TEST_F(ConfigTest, GatewayEntryIgnoresExeAndStartDir) {
    // 契约 process: md 段的 exe 和 start_dir 字段被静默忽略
    // entry.exe 和 entry.start_dir 留空, 由 launch_child 调 find_exe_by_stem 实时填充
    // 旧测试名 StartDirDefaultsToExeParent 已失效 (exe 不再读取, 无 parent_path 可言)
    write_json(R"({
        "md": {"dzmd_ctp": {"exe": "dzmd_ctp"}}
    })");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);

    const auto& e = cfg.entries[0];
    EXPECT_TRUE(e.exe.empty());
    EXPECT_TRUE(e.start_dir.empty());
}

TEST_F(ConfigTest, DefaultLogLevel) {
    write_json(R"({"master": {}})");
    auto cfg = parse_master_json(config_path_);
    EXPECT_EQ(cfg.master.single_stop_timeout_sec, 3);
}

TEST_F(ConfigTest, EmptyJson) {
    // 空 JSON object 等价于空配置, 所有字段使用默认值
    write_json(R"({})");
    auto cfg = parse_master_json(config_path_);
    EXPECT_EQ(cfg.master.single_stop_timeout_sec, 3);
    EXPECT_TRUE(cfg.entries.empty());
}

TEST_F(ConfigTest, ParseWebuiEntry) {
    // 契约 process: webui 段的 exe 和 start_dir 字段被静默忽略 (旧 json 兼容)
    // entry.name 内部统一为 "dzweb"
    write_json(R"({
        "webui": {
            "exe": "dzweb",
            "args": [],
            "start_dir": ".",
            "restart": {"enabled": true, "max_attempts": 5, "backoff_sec": 5}
        }
    })");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);

    const auto& w = cfg.entries[0];
    EXPECT_EQ(w.name, "dzweb");
    EXPECT_EQ(w.category, Category::WebUI);
    // exe 和 start_dir 不从 json 读取, 留空由 launch_child 调 find_exe_by_stem 实时填充
    EXPECT_TRUE(w.exe.empty());
    EXPECT_TRUE(w.start_dir.empty());
    EXPECT_TRUE(w.args.empty());
    EXPECT_TRUE(w.restart.enabled);
    EXPECT_EQ(w.restart.max_attempts, 5);
}

TEST_F(ConfigTest, ParseWebuiEntryIgnoresExeAndStartDir) {
    // 契约 process: webui 段的 exe 字段被静默忽略, entry.exe 和 entry.start_dir 留空
    // 旧测试名 ParseWebuiDefaultsToExeParent 已失效 (exe 不再读取, 无 parent_path 可言)
    write_json(R"({
        "webui": {"exe": "dzweb"}
    })");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);

    const auto& w = cfg.entries[0];
    EXPECT_TRUE(w.exe.empty());
    EXPECT_TRUE(w.start_dir.empty());
    // WebUI 默认 restart 应启用（与 gateway 一致）
    EXPECT_TRUE(w.restart.enabled);
    EXPECT_EQ(w.restart.max_attempts, 5);
}

TEST_F(ConfigTest, NoWebuiEntryMeansNoWebuiStart) {
    write_json(R"({"master": {}})");
    auto cfg = parse_master_json(config_path_);
    EXPECT_TRUE(cfg.entries.empty());
}

// ---- md/td section 拆分新测试 ----

TEST_F(ConfigTest, ParseMdAndTdSections) {
    write_json(R"({
        "md": {
            "dzmd_ctp": {"args": [], "restart": {"enabled": true, "max_attempts": 5, "backoff_sec": 5}}
        },
        "td": {
            "dztd_ctp": {"args": [], "restart": {"enabled": true, "max_attempts": 5, "backoff_sec": 5}}
        }
    })");
    auto cfg = parse_master_json(config_path_);
    EXPECT_EQ(cfg.entries.size(), 2u);
    auto md = std::find_if(cfg.entries.begin(), cfg.entries.end(),
        [](const ProcessEntry& e) { return e.name == "dzmd_ctp"; });
    ASSERT_NE(md, cfg.entries.end());
    EXPECT_EQ(md->category, Category::GatewayMd);
    auto td = std::find_if(cfg.entries.begin(), cfg.entries.end(),
        [](const ProcessEntry& e) { return e.name == "dztd_ctp"; });
    ASSERT_NE(td, cfg.entries.end());
    EXPECT_EQ(td->category, Category::GatewayTd);
}

TEST_F(ConfigTest, ParseLogSection) {
    write_json(R"({"log": {"level": "debug", "flush_on": "trace"}})");
    // log section 不再由 parse_master_json 加载到 Config 结构体,
    // 而是由 ShmManager 内部的 LogConfig 类用 core::load_json_section 自行加载
    auto log_json = dztrader::core::load_json_section<nlohmann::json>(config_path_, "log");
    EXPECT_EQ(log_json["level"], "debug");
    EXPECT_EQ(log_json["flush_on"], "trace");
}

TEST_F(ConfigTest, ParseShmSections) {
    write_json(R"({
        "shm": {
            "meta_file_size": 2097152,
            "event": {"page_size_mb": 64, "check_interval_min": 10}
        }
    })");
    auto cfg = parse_master_json(config_path_);
    EXPECT_EQ(cfg.shm_global.meta_file_size, 2097152);
    // event 子段不在此结构体中——ShmManager 内部的 EventShmConfig 类自行加载
}

// ---- write/remove gateway section with Category 参数 ----

TEST_F(ConfigTest, WriteGatewaySectionMdAndTd) {
    write_json(R"({"md": {}, "td": {}})");
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp",
                          {"--arg1"}, {.enabled = true, .max_attempts = 3, .backoff_sec = 5}, "CTP");
    write_gateway_section(config_path_, Category::GatewayTd, "dztd_ctp",
                          {"--arg2"}, {.enabled = true, .max_attempts = 5, .backoff_sec = 10}, "");

    auto cfg = parse_master_json(config_path_);
    EXPECT_EQ(cfg.entries.size(), 2u);

    auto md = std::find_if(cfg.entries.begin(), cfg.entries.end(),
        [](const ProcessEntry& e) { return e.name == "dzmd_ctp"; });
    ASSERT_NE(md, cfg.entries.end());
    EXPECT_EQ(md->category, Category::GatewayMd);
    EXPECT_EQ(md->args.size(), 1u);
    EXPECT_EQ(md->args[0], "--arg1");
    EXPECT_TRUE(md->restart.enabled);
    EXPECT_EQ(md->restart.max_attempts, 3);
    EXPECT_EQ(md->restart.backoff_sec, 5);
    EXPECT_EQ(md->display_name, "CTP");

    auto td = std::find_if(cfg.entries.begin(), cfg.entries.end(),
        [](const ProcessEntry& e) { return e.name == "dztd_ctp"; });
    ASSERT_NE(td, cfg.entries.end());
    EXPECT_EQ(td->category, Category::GatewayTd);
    EXPECT_EQ(td->args.size(), 1u);
    EXPECT_EQ(td->args[0], "--arg2");
    EXPECT_EQ(td->restart.max_attempts, 5);
    EXPECT_EQ(td->restart.backoff_sec, 10);
    EXPECT_TRUE(td->display_name.empty());
}

TEST_F(ConfigTest, RemoveGatewaySectionByCategory) {
    write_json(R"({
        "md": {"dzmd_ctp": {"args": []}},
        "td": {"dztd_ctp": {"args": []}}
    })");
    remove_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp");

    auto cfg = parse_master_json(config_path_);
    EXPECT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "dztd_ctp");
    EXPECT_EQ(cfg.entries[0].category, Category::GatewayTd);
}

// ---- MasterConfig (master section, 仅 single_stop_timeout_sec) ----

TEST_F(ConfigTest, MasterConfigLoadDefault) {
    // 文件不存在 -> 默认值
    auto cfg = MasterConfig::load(tmp_dir_ / "nonexistent.json");
    EXPECT_EQ(cfg.single_stop_timeout_sec, 3);
}

TEST_F(ConfigTest, MasterConfigLoadFromSection) {
    nlohmann::json full = {
        {"master", {{"single_stop_timeout_sec", 10}}}
    };
    auto path = tmp_dir_ / "master_test.json";
    std::ofstream(path) << full.dump(2);

    auto cfg = MasterConfig::load(path);
    EXPECT_EQ(cfg.single_stop_timeout_sec, 10);
}

TEST_F(ConfigTest, MasterConfigSavePreservesOtherFields) {
    // 初始文件: master 段含 single_stop_timeout_sec + 自定义字段 other_field
    nlohmann::json full = {
        {"master", {{"single_stop_timeout_sec", 5}, {"other_field", "keep_me"}}},
        {"log", {{"level", "info"}}}
    };
    auto path = tmp_dir_ / "master_save_test.json";
    std::ofstream(path) << full.dump(2);

    // 修改 single_stop_timeout_sec 后 save
    MasterConfig cfg;
    cfg.single_stop_timeout_sec = 99;
    cfg.save(path);

    // 验证: master.single_stop_timeout_sec 已更新, other_field 保留, log 段保留
    std::ifstream ifs(path);
    nlohmann::json saved;
    ifs >> saved;
    EXPECT_EQ(saved["master"]["single_stop_timeout_sec"], 99);
    EXPECT_EQ(saved["master"]["other_field"], "keep_me");
    EXPECT_EQ(saved["log"]["level"], "info");
}

// ---- ShmGlobalConfig (shm section, 仅 meta_file_size) ----

TEST_F(ConfigTest, ShmGlobalConfigLoadDefault) {
    auto cfg = ShmGlobalConfig::load(tmp_dir_ / "nonexistent.json");
    EXPECT_EQ(cfg.meta_file_size, 1 * 1024 * 1024);
}

TEST_F(ConfigTest, ShmGlobalConfigLoadFromSection) {
    nlohmann::json full = {
        {"shm", {
            {"meta_file_size", 2 * 1024 * 1024},
            {"event", {{"page_size_mb", 32}}}  // event 子段应被忽略
        }}
    };
    auto path = tmp_dir_ / "shm_global_test.json";
    std::ofstream(path) << full.dump(2);

    auto cfg = ShmGlobalConfig::load(path);
    EXPECT_EQ(cfg.meta_file_size, 2 * 1024 * 1024);
}

TEST_F(ConfigTest, ShmGlobalConfigSavePreservesEventSubsection) {
    // 初始: shm.meta_file_size=1MB + shm.event={page_size_mb:32,...}
    nlohmann::json full = {
        {"shm", {
            {"meta_file_size", 1 * 1024 * 1024},
            {"event", {{"page_size_mb", 32}, {"check_interval_min", 5}}}
        }}
    };
    auto path = tmp_dir_ / "shm_global_save_test.json";
    std::ofstream(path) << full.dump(2);

    // 修改 meta_file_size 后 save
    ShmGlobalConfig cfg;
    cfg.meta_file_size = 4 * 1024 * 1024;
    cfg.save(path);

    // 验证: meta_file_size 更新, event 子段保留
    std::ifstream ifs(path);
    nlohmann::json saved;
    ifs >> saved;
    EXPECT_EQ(saved["shm"]["meta_file_size"], 4 * 1024 * 1024);
    EXPECT_EQ(saved["shm"]["event"]["page_size_mb"], 32);
    EXPECT_EQ(saved["shm"]["event"]["check_interval_min"], 5);
}

// ---- 默认配置生成: 包含 webui 段 (默认启动 dzweb 后台) ----

TEST_F(ConfigTest, DefaultConfigIncludesWebui) {
    auto path = tmp_dir_ / "default_dztraderd.json";
    generate_default_config(path);
    ASSERT_TRUE(std::filesystem::exists(path));

    auto cfg = parse_master_json(path);
    ASSERT_EQ(cfg.entries.size(), 1u);
    const auto& w = cfg.entries[0];
    EXPECT_EQ(w.name, "dzweb");
    EXPECT_EQ(w.category, Category::WebUI);
    // 契约 process: exe 和 start_dir 不持久化, 留空由 launch_child 填充
    EXPECT_TRUE(w.exe.empty());
    EXPECT_TRUE(w.start_dir.empty());
    EXPECT_TRUE(w.restart.enabled);
    EXPECT_EQ(w.restart.max_attempts, 5);
}

TEST_F(ConfigTest, DefaultConfigNotOverwriteExisting) {
    auto path = tmp_dir_ / "default_dztraderd.json";
    std::ofstream(path) << R"({"master": {"single_stop_timeout_sec": 10}})";
    generate_default_config(path);
    auto cfg = parse_master_json(path);
    EXPECT_EQ(cfg.master.single_stop_timeout_sec, 10);
    EXPECT_TRUE(cfg.entries.empty());  // 已有文件不含 webui 段, 不覆盖
}

}  // namespace
}  // namespace dztrader::master
