/**
 * @file config_test.cpp
 * @brief Unit tests for config parsing (Category, RestartPolicy, ProcessEntry, JSON).
 */

#include "config.h"
#include "test_temp_dir.h"

#include <dztrader/core/env.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string_view>

namespace dztrader::master {
namespace {

/// 测试内临时覆盖环境变量, 析构时恢复原值 (未设置则 unset)。
class ScopedEnvOverride {
public:
    ScopedEnvOverride(std::string_view key, std::string value)
        : key_(key),
          original_(dztrader::env::get(key)) {
        dztrader::env::set(key_, value);
    }

    ~ScopedEnvOverride() {
        try {
            if (original_) {
                dztrader::env::set(key_, *original_);
            } else {
                dztrader::env::unset(key_);
            }
        } catch (...) {
        }
    }

    ScopedEnvOverride(const ScopedEnvOverride&) = delete;
    ScopedEnvOverride& operator=(const ScopedEnvOverride&) = delete;

private:
    std::string key_;
    std::optional<std::string> original_;
};

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = dztrader::test::unique_temp_dir("master_config_test");
        std::filesystem::create_directories(tmp_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

    void write_json(const std::string& content) {
        config_path_ = tmp_dir_ / "dztraderd.json";
        std::ofstream ofs(config_path_);
        ofs << content;
    }

    nlohmann::json read_json() const { return read_json(config_path_); }

    nlohmann::json read_json(const std::filesystem::path& path) const {
        std::ifstream ifs(path);
        nlohmann::json data;
        ifs >> data;
        return data;
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path config_path_;
};

TEST_F(ConfigTest, CategoryStr) {
    EXPECT_STREQ(category_str(Category::GatewayMd), "md");
    EXPECT_STREQ(category_str(Category::GatewayTd), "td");
    EXPECT_STREQ(category_str(Category::Strategy), "stg");
    EXPECT_STREQ(category_str(Category::WebUI), "webui");
    EXPECT_STREQ(category_str(Category::Internal), "internal");
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

    // 内部进程默认重启策略与既有网关一致
    auto internal_policy = default_restart_policy(Category::Internal);
    EXPECT_TRUE(internal_policy.enabled);
    EXPECT_EQ(internal_policy.max_attempts, 5);
    EXPECT_EQ(internal_policy.backoff_sec, 5);
}

TEST_F(ConfigTest, ParseMinimalJson) {
    write_json(R"({"master": {}})");
    auto cfg = parse_master_json(config_path_);
    EXPECT_EQ(cfg.master.single_stop_timeout_sec, 3);
    EXPECT_TRUE(cfg.entries.empty());
}

// ---- internal 段 (内部进程, 形制与 md/td 网关段一致) ----

TEST_F(ConfigTest, ParseInternalSection) {
    write_json(R"({
        "master": {},
        "md": {"dzmd_ctp": {"args": []}},
        "internal": {
            "dzsysmon": {
                "args": ["--config", "custom.json"],
                "env": {"DZ_SYSMON_DEBUG": "1"},
                "restart": {"enabled": true, "max_attempts": 3, "backoff_sec": 7},
                "display_name": "系统监控"
            }
        }
    })");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 2u);
    const auto wi = std::find_if(cfg.entries.begin(), cfg.entries.end(),
                                 [](const ProcessEntry& e) { return e.name == "dzsysmon"; });
    ASSERT_NE(wi, cfg.entries.end());
    EXPECT_EQ(wi->category, Category::Internal);
    EXPECT_EQ(wi->args, (std::vector<std::string>{"--config", "custom.json"}));
    ASSERT_EQ(wi->env.count("DZ_SYSMON_DEBUG"), 1u);
    EXPECT_EQ(wi->env.at("DZ_SYSMON_DEBUG"), "1");
    EXPECT_TRUE(wi->restart.enabled);
    EXPECT_EQ(wi->restart.max_attempts, 3);
    EXPECT_EQ(wi->restart.backoff_sec, 7);
    EXPECT_EQ(wi->display_name, "系统监控");
    // 契约 process: 非策略进程 exe/start_dir 不持久化, 留空由 launch_child 扫描填充
    EXPECT_TRUE(wi->exe.empty());
    EXPECT_TRUE(wi->start_dir.empty());
}

// 段缺失 = 无内部进程; 段类型非 object 沿用既有"静默忽略"行为
TEST_F(ConfigTest, ParseMissingInternalSectionYieldsNoInternalEntries) {
    for (const char* json : {R"({"master": {}})", R"({"master": {}, "internal": []})"}) {
        write_json(json);
        auto cfg = parse_master_json(config_path_);
        EXPECT_TRUE(cfg.entries.empty()) << json;
    }
}

// 未知顶级段静默忽略 (既有行为不变, 不引入严格校验)
TEST_F(ConfigTest, ParseUnknownSectionIgnored) {
    write_json(R"({
        "master": {},
        "unknown_section": {"whatever": 1},
        "sysmon": {"dzsysmon": {"args": []}},
        "md": {"dzmd_ctp": {"args": []}}
    })");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "dzmd_ctp");
    EXPECT_EQ(cfg.entries[0].category, Category::GatewayMd);
}

// ---- parse_master_json 错误路径 ----

TEST_F(ConfigTest, ParseMasterJsonMissingFileThrows) {
    const auto missing = tmp_dir_ / "nonexistent.json";
    ASSERT_FALSE(std::filesystem::exists(missing));
    EXPECT_THROW(parse_master_json(missing), std::runtime_error);
}

TEST_F(ConfigTest, ParseMasterJsonCorruptJsonThrows) {
    write_json("{not valid json");
    EXPECT_THROW(parse_master_json(config_path_), std::runtime_error);
}

TEST_F(ConfigTest, ParseMasterJsonRootArrayThrows) {
    write_json(R"([{"master": {}}])");
    EXPECT_THROW(parse_master_json(config_path_), std::runtime_error);
}

TEST_F(ConfigTest, ParseMasterJsonWrongFieldTypeThrows) {
    // restart.max_attempts 期望整数, 传字符串是用户编辑配置的首要故障形态
    write_json(R"({
        "md": {"dzmd_ctp": {"restart": {"max_attempts": "five"}}}
    })");
    EXPECT_THROW(parse_master_json(config_path_), nlohmann::json::type_error);
}

TEST_F(ConfigTest, ParseStrategyMissingNameThrows) {
    write_json(R"({
        "strategy": [{"exe": "/path/to/alpha", "md_source": "dzmd_ctp"}]
    })");
    EXPECT_THROW(parse_master_json(config_path_), nlohmann::json::out_of_range);
}

TEST_F(ConfigTest, ParseStrategyMissingExeThrows) {
    write_json(R"({
        "strategy": [{"name": "alpha", "md_source": "dzmd_ctp"}]
    })");
    EXPECT_THROW(parse_master_json(config_path_), nlohmann::json::out_of_range);
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
    write_json(R"({
        "master": {"single_stop_timeout_sec": 9},
        "md": {
            "dzmd_ctp": {"args": []},
            "dzmd_other": {"args": []}
        }
    })");
    // 目标不存在: 无操作, 条目集合与 master 段值不变
    EXPECT_NO_THROW(remove_gateway_section(config_path_, Category::GatewayMd, "nonexistent"));
    auto cfg1 = parse_master_json(config_path_);
    ASSERT_EQ(cfg1.entries.size(), 2u);
    EXPECT_NE(std::find_if(cfg1.entries.begin(), cfg1.entries.end(),
                           [](const ProcessEntry& e) { return e.name == "dzmd_ctp"; }),
              cfg1.entries.end());
    EXPECT_NE(std::find_if(cfg1.entries.begin(), cfg1.entries.end(),
                           [](const ProcessEntry& e) { return e.name == "dzmd_other"; }),
              cfg1.entries.end());
    EXPECT_EQ(cfg1.master.single_stop_timeout_sec, 9);

    // 重复删除同一条目: 第二次为幂等无操作, 其余条目与 master 段值不变
    remove_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp");
    remove_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp");
    auto cfg2 = parse_master_json(config_path_);
    ASSERT_EQ(cfg2.entries.size(), 1u);
    EXPECT_EQ(cfg2.entries[0].name, "dzmd_other");
    EXPECT_EQ(cfg2.master.single_stop_timeout_sec, 9);
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
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp", {"--name", "dzmd_ctp"},
                          policy);
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "dzmd_ctp");
    // 契约 process: 内部进程 exe 和 start_dir 不持久化到 json, parse 不读取, 留空由 launch_child 调
    // find_exe_by_stem 实时填充
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
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp", {"--name", "dzmd_ctp"},
                          policy);
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
    // 契约 process: dzweb 的 exe 和 start_dir 不持久化到 json, parse 不读取, 留空由 launch_child 调
    // find_exe_by_stem 实时填充
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

// ---- remove_webui_section (生产在移除 dzweb 时调用) ----

TEST_F(ConfigTest, RemoveWebuiSectionDeletesSection) {
    write_json(R"({
        "master": {},
        "webui": {"args": ["--name", "dzweb"]}
    })");
    remove_webui_section(config_path_);
    auto cfg = parse_master_json(config_path_);
    EXPECT_TRUE(cfg.entries.empty());
    EXPECT_FALSE(read_json().contains("webui"));
}

// 缺 webui 段: 幂等无操作, 其他段原样保留
TEST_F(ConfigTest, RemoveWebuiSectionMissingIsIdempotent) {
    write_json(R"({
        "master": {"single_stop_timeout_sec": 9},
        "md": {"dzmd_ctp": {"args": []}}
    })");
    EXPECT_NO_THROW(remove_webui_section(config_path_));
    EXPECT_NO_THROW(remove_webui_section(config_path_));

    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "dzmd_ctp");
    EXPECT_EQ(cfg.master.single_stop_timeout_sec, 9);
}

TEST_F(ConfigTest, RemoveWebuiSectionPreservesOtherSections) {
    write_json(R"({
        "log": {"level": "debug"},
        "master": {"single_stop_timeout_sec": 7},
        "shm": {"meta_file_size": 2097152},
        "webui": {"args": []},
        "md": {"dzmd_ctp": {"args": []}}
    })");
    remove_webui_section(config_path_);

    const auto data = read_json();
    EXPECT_FALSE(data.contains("webui"));
    EXPECT_EQ(data["log"]["level"], "debug");
    EXPECT_EQ(data["master"]["single_stop_timeout_sec"], 7);
    EXPECT_EQ(data["shm"]["meta_file_size"], 2097152);
    EXPECT_TRUE(data["md"].contains("dzmd_ctp"));
}

TEST_F(ConfigTest, RemoveWebuiSectionCorruptJsonThrows) {
    write_json("{not valid json");
    EXPECT_THROW(remove_webui_section(config_path_), std::runtime_error);
}

// gateway/webui/strategy 三个写入函数都是 load-modify-save: 其他顶层段必须原样保留
TEST_F(ConfigTest, WriteSectionsPreserveOtherTopLevelSections) {
    write_json(R"({
        "log": {"level": "debug", "flush_on": "trace"},
        "master": {"single_stop_timeout_sec": 9, "md_ready_timeout_sec": 7},
        "shm": {"meta_file_size": 2097152, "event": {"page_size_mb": 64}},
        "md": {"dzmd_ctp": {"args": []}}
    })");
    const platform::RestartPolicy policy{.enabled = true, .max_attempts = 5, .backoff_sec = 5};
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp2", {"--arg"}, policy);
    write_webui_section(config_path_, {"--name", "dzweb"}, policy);
    write_strategy_section(config_path_, {{"name", "alpha"},
                                          {"exe", "/path/to/alpha"},
                                          {"md_source", "dzmd_ctp"},
                                          {"args", nlohmann::json::array()}});

    const auto data = read_json();
    EXPECT_EQ(data["log"]["level"], "debug");
    EXPECT_EQ(data["log"]["flush_on"], "trace");
    EXPECT_EQ(data["master"]["single_stop_timeout_sec"], 9);
    EXPECT_EQ(data["master"]["md_ready_timeout_sec"], 7);
    EXPECT_EQ(data["shm"]["meta_file_size"], 2097152);
    EXPECT_EQ(data["shm"]["event"]["page_size_mb"], 64);
    EXPECT_TRUE(data["md"].contains("dzmd_ctp"));
    EXPECT_TRUE(data["md"].contains("dzmd_ctp2"));
    EXPECT_TRUE(data.contains("webui"));

    // 解析结果同样保持: 原配置语义没有被三个写入函数破坏
    auto cfg = parse_master_json(config_path_);
    EXPECT_EQ(cfg.master.single_stop_timeout_sec, 9);
    EXPECT_EQ(cfg.master.md_ready_timeout_sec, 7);
    EXPECT_EQ(cfg.shm_global.meta_file_size, 2097152);
    EXPECT_EQ(cfg.entries.size(), 4u);
}

// display_name 持久化往返: write 非空 display_name → parse 读回应一致
TEST_F(ConfigTest, WriteGatewaySectionPersistsDisplayName) {
    write_json(R"({"master": {}})");
    platform::RestartPolicy policy{.enabled = true, .max_attempts = 5, .backoff_sec = 5};
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp", {"--name", "dzmd_ctp"},
                          policy, "CTP期货行情");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].display_name, "CTP期货行情");
}

// display_name 为空时: 不写入 json 字段, parse 读回为空串 (向后兼容旧 json)
TEST_F(ConfigTest, WriteGatewaySectionEmptyDisplayNameSkipped) {
    write_json(R"({"master": {}})");
    platform::RestartPolicy policy{.enabled = true, .max_attempts = 5, .backoff_sec = 5};
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp", {"--name", "dzmd_ctp"},
                          policy,
                          "");  // 空 display_name
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_TRUE(cfg.entries[0].display_name.empty());

    // 直接解析 JSON: 空 display_name 与空 exe/start_dir 都不写 key
    // (仅看解析结果无法区分"写空串"与"不写字段", 解析侧两者都是空串)
    const auto data = read_json();
    ASSERT_TRUE(data.contains("md"));
    const auto& gw = data["md"]["dzmd_ctp"];
    EXPECT_FALSE(gw.contains("display_name"));
    EXPECT_FALSE(gw.contains("exe"));
    EXPECT_FALSE(gw.contains("start_dir"));
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
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp", {"--name", "dzmd_ctp"},
                          policy);
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
    ASSERT_TRUE(md->env.contains("LOG_LEVEL"));
    EXPECT_EQ(md->env.at("LOG_LEVEL"), "debug");
    EXPECT_TRUE(md->restart.enabled);

    EXPECT_EQ(td->category, Category::GatewayTd);
    EXPECT_TRUE(td->args.empty());
    EXPECT_TRUE(td->restart.enabled);
}

TEST_F(ConfigTest, ParseStrategyEntries) {
    // 绝对路径必须用平台原生形式（Windows 的 /home/... 会被解析成当前盘符）。
    const auto absolute_exe = dztrader::test::unique_temp_dir("dz_config_abs") / "my_strategy";
    const nlohmann::json config = {
        {"master", nlohmann::json::object()},
        {"strategy", nlohmann::json::array({{{"name", "my_strategy"},
                                             {"exe", absolute_exe.string()},
                                             {"md_source", "dzmd_ctp"},
                                             {"args", {"--config", "strategy.toml"}}},
                                            {{"name", "relative_strategy"},
                                             {"exe", "strategies/rel_strategy"},
                                             {"md_source", "dzmd_ctp"}}})}};
    write_json(config.dump());
    const ScopedEnvOverride home_override("DZTRADER_HOME", tmp_dir_.string());
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 2u);

    // 绝对 exe: 原样保留; 未给 start_dir 时默认取 exe 父目录
    const auto& s = cfg.entries[0];
    EXPECT_EQ(s.name, "my_strategy");
    EXPECT_EQ(s.category, Category::Strategy);
    EXPECT_EQ(s.args.size(), 2u);
    EXPECT_FALSE(s.restart.enabled);  // strategy default: no restart
    EXPECT_EQ(s.md_source, "dzmd_ctp");
    EXPECT_EQ(s.exe, absolute_exe);
    EXPECT_EQ(s.start_dir, absolute_exe.parent_path());

    // 相对 exe: 拼 DZTRADER_HOME; start_dir 默认取解析后 exe 的父目录
    const auto& rel = cfg.entries[1];
    EXPECT_EQ(rel.name, "relative_strategy");
    EXPECT_EQ(rel.exe, tmp_dir_ / "strategies" / "rel_strategy");
    EXPECT_EQ(rel.start_dir, tmp_dir_ / "strategies");
}

// 策略携带 md_source: 解析进 ProcessEntry, 供启动编排与 env 注入
TEST_F(ConfigTest, ParseStrategyWithMdSource) {
    write_json(R"({
        "strategy": [
            {
                "name": "alpha",
                "exe": "/path/to/alpha",
                "md_source": "dzmd_ctp"
            }
        ]
    })");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].md_source, "dzmd_ctp");
}

// 策略缺 md_source: 配置错误 (解析抛异常)
TEST_F(ConfigTest, ParseStrategyMissingMdSourceThrows) {
    write_json(R"({
        "strategy": [
            {"name": "alpha", "exe": "/path/to/alpha"}
        ]
    })");
    EXPECT_THROW(parse_master_json(config_path_), std::runtime_error);
}

// 策略 md_source 为空串: 配置错误
TEST_F(ConfigTest, ParseStrategyEmptyMdSourceThrows) {
    write_json(R"({
        "strategy": [
            {"name": "alpha", "exe": "/path/to/alpha", "md_source": ""}
        ]
    })");
    EXPECT_THROW(parse_master_json(config_path_), std::runtime_error);
}

// 策略 md_source 含非法字符 (/): 配置错误
TEST_F(ConfigTest, ParseStrategyMdSourceInvalidCharThrows) {
    write_json(R"({
        "strategy": [
            {"name": "alpha", "exe": "/path/to/alpha", "md_source": "a/b"}
        ]
    })");
    EXPECT_THROW(parse_master_json(config_path_), std::runtime_error);
}

// 策略 md_source 过长 (>63): 配置错误
TEST_F(ConfigTest, ParseStrategyMdSourceTooLongThrows) {
    write_json(R"({
        "strategy": [
            {"name": "alpha", "exe": "/path/to/alpha",
             "md_source": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
        ]
    })");
    EXPECT_THROW(parse_master_json(config_path_), std::runtime_error);
}

// 非 Strategy 条目携带 md_source: 配置错误 (契约 4.1 规则 6)
TEST_F(ConfigTest, ParseGatewayWithMdSourceThrows) {
    write_json(R"({
        "md": {
            "dzmd_ctp": {"args": [], "md_source": "ignored"}
        }
    })");
    EXPECT_THROW(parse_master_json(config_path_), std::runtime_error);
}

// webui 条目携带 md_source: 同样配置错误
TEST_F(ConfigTest, ParseWebuiWithMdSourceThrows) {
    write_json(R"({
        "webui": {"args": [], "md_source": "ignored"}
    })");
    EXPECT_THROW(parse_master_json(config_path_), std::runtime_error);
}

// ---- 策略条目持久化 (strategy section 数组) ----

// write_strategy_section 新增策略条目 (strategy section 从无到有)
TEST_F(ConfigTest, WriteStrategySectionCreatesEntry) {
    write_json(R"({"master": {}})");
    nlohmann::json entry = {
        {"name", "alpha"},
        {"exe", "/path/to/alpha"},
        {"args", nlohmann::json::array()},
        {"md_source", "dzmd_ctp"},
    };
    write_strategy_section(config_path_, entry);
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "alpha");
    EXPECT_EQ(cfg.entries[0].md_source, "dzmd_ctp");
}

// write_strategy_section 更新既有条目时保留 md_source/exe (不被 args 覆盖写丢)
TEST_F(ConfigTest, WriteStrategySectionPreservesMdSourceOnUpdate) {
    write_json(R"({
        "strategy": [
            {"name": "alpha", "exe": "/path/to/alpha", "md_source": "dzmd_ctp", "args": ["old"]}
        ]
    })");
    // 模拟 SET_PROCESS_CONFIG 更新 args/env/restart (不含 md_source)
    nlohmann::json entry = {
        {"name", "alpha"},
        {"args", nlohmann::json::array({"new_arg"})},
        {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}},
    };
    write_strategy_section(config_path_, entry);
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "alpha");
    // 既有 md_source/exe 保留 (修复: 误写 md.<name> 段会把 md_source 写丢)
    EXPECT_EQ(cfg.entries[0].md_source, "dzmd_ctp");
    EXPECT_EQ(cfg.entries[0].args, std::vector<std::string>({"new_arg"}));
}

// remove_strategy_section 删除指定策略条目, 其他条目保留
TEST_F(ConfigTest, RemoveStrategySectionDeletesEntry) {
    write_json(R"({
        "strategy": [
            {"name": "alpha", "exe": "/a", "md_source": "dzmd_ctp"},
            {"name": "beta", "exe": "/b", "md_source": "dzmd_ctp"}
        ]
    })");
    remove_strategy_section(config_path_, "alpha");
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "beta");
}

// remove_strategy_section 目标不存在: 无操作不抛
TEST_F(ConfigTest, RemoveStrategySectionIdempotent) {
    write_json(R"({"strategy": [{"name": "alpha", "exe": "/a", "md_source": "dzmd_ctp"}]})");
    EXPECT_NO_THROW(remove_strategy_section(config_path_, "nonexistent"));
    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "alpha");
}

// ---- MasterConfig.md_ready_timeout_sec ----

TEST_F(ConfigTest, MasterConfigMdReadyTimeoutDefault) {
    // 旧文件无该字段 -> 默认 5
    auto cfg = MasterConfig::load(tmp_dir_ / "nonexistent.json");
    EXPECT_EQ(cfg.md_ready_timeout_sec, 5);
}

TEST_F(ConfigTest, MasterConfigSingleStopTimeoutLoadAndClamp) {
    write_json(R"({"master": {"single_stop_timeout_sec": 0}})");
    auto zero_cfg = parse_master_json(config_path_);
    EXPECT_EQ(zero_cfg.master.single_stop_timeout_sec, 1);

    write_json(R"({"master": {"single_stop_timeout_sec": -1}})");
    auto negative_cfg = parse_master_json(config_path_);
    EXPECT_EQ(negative_cfg.master.single_stop_timeout_sec, 1);
}

TEST_F(ConfigTest, MasterConfigMdReadyTimeoutLoadAndClamp) {
    write_json(R"({"master": {"md_ready_timeout_sec": 0}})");
    auto zero_cfg = parse_master_json(config_path_);
    // <1 clamp 到 1 (与 single_stop_timeout_sec 同规则)
    EXPECT_EQ(zero_cfg.master.md_ready_timeout_sec, 1);

    write_json(R"({"master": {"md_ready_timeout_sec": -1}})");
    auto negative_cfg = parse_master_json(config_path_);
    EXPECT_EQ(negative_cfg.master.md_ready_timeout_sec, 1);
}

TEST_F(ConfigTest, MasterConfigSavePreservesMdReadyTimeout) {
    nlohmann::json full = {
        {"master", {{"single_stop_timeout_sec", 3}, {"md_ready_timeout_sec", 7}}}};
    auto path = tmp_dir_ / "master_md_ready.json";
    std::ofstream(path) << full.dump(2);

    MasterConfig cfg;
    cfg.md_ready_timeout_sec = 9;
    cfg.save(path);

    std::ifstream ifs(path);
    nlohmann::json saved;
    ifs >> saved;
    EXPECT_EQ(saved["master"]["md_ready_timeout_sec"], 9);
    EXPECT_EQ(saved["master"]["single_stop_timeout_sec"], 3);
}

TEST_F(ConfigTest, ParseStrategyWithRestart) {
    write_json(R"({
        "strategy": [
            {
                "name": "auto_restart_strat",
                "exe": "/path/to/strat",
                "md_source": "dzmd_ctp",
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
    write_gateway_section(config_path_, Category::GatewayMd, "dzmd_ctp", {"--arg1"},
                          {.enabled = true, .max_attempts = 3, .backoff_sec = 5}, "CTP");
    write_gateway_section(config_path_, Category::GatewayTd, "dztd_ctp", {"--arg2"},
                          {.enabled = true, .max_attempts = 5, .backoff_sec = 10}, "");

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

// ---- internal 段持久化: 写入/删除按类别路由到 internal (不误写 md/td) ----

TEST_F(ConfigTest, WriteInternalSectionRoutesToInternalSection) {
    write_json(R"({"master": {}})");
    write_gateway_section(config_path_, Category::Internal, "dzsysmon", {"--config", "x.json"},
                          {.enabled = true, .max_attempts = 3, .backoff_sec = 7}, "系统监控");

    const auto data = read_json();
    ASSERT_TRUE(data.contains("internal"));
    ASSERT_TRUE(data["internal"].contains("dzsysmon"));
    EXPECT_FALSE(data.contains("md"));
    EXPECT_FALSE(data.contains("td"));
    const auto& entry = data["internal"]["dzsysmon"];
    EXPECT_FALSE(entry.contains("exe"));
    EXPECT_FALSE(entry.contains("start_dir"));
    EXPECT_EQ(entry["display_name"], "系统监控");

    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "dzsysmon");
    EXPECT_EQ(cfg.entries[0].category, Category::Internal);
    EXPECT_EQ(cfg.entries[0].args.size(), 2u);
    EXPECT_EQ(cfg.entries[0].restart.max_attempts, 3);
}

TEST_F(ConfigTest, RemoveInternalSectionByCategory) {
    write_json(R"({
        "internal": {
            "dzsysmon": {"args": []},
            "other_internal": {"args": []}
        }
    })");
    remove_gateway_section(config_path_, Category::Internal, "dzsysmon");

    auto cfg = parse_master_json(config_path_);
    ASSERT_EQ(cfg.entries.size(), 1u);
    EXPECT_EQ(cfg.entries[0].name, "other_internal");
    EXPECT_EQ(cfg.entries[0].category, Category::Internal);
}

// ---- MasterConfig (master section, 仅 single_stop_timeout_sec) ----

TEST_F(ConfigTest, MasterConfigLoadDefault) {
    // 文件不存在 -> 默认值
    auto cfg = MasterConfig::load(tmp_dir_ / "nonexistent.json");
    EXPECT_EQ(cfg.single_stop_timeout_sec, 3);
}

TEST_F(ConfigTest, MasterConfigLoadFromSection) {
    nlohmann::json full = {{"master", {{"single_stop_timeout_sec", 10}}}};
    auto path = tmp_dir_ / "master_test.json";
    std::ofstream(path) << full.dump(2);

    auto cfg = MasterConfig::load(path);
    EXPECT_EQ(cfg.single_stop_timeout_sec, 10);
}

TEST_F(ConfigTest, MasterConfigSavePreservesOtherFields) {
    // 初始文件: master 段含 single_stop_timeout_sec + 自定义字段 other_field
    nlohmann::json full = {{"master", {{"single_stop_timeout_sec", 5}, {"other_field", "keep_me"}}},
                           {"log", {{"level", "info"}}}};
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

// [master] 段清理策略字段: 旧文件无字段走默认 (与旧硬编码 200/24 一致); 双 0 往返
TEST_F(ConfigTest, MasterConfigCleanupPolicyRoundTrip) {
    // 旧配置文件无此字段 -> WITH_DEFAULT 补默认值 (自愈)
    write_json(R"({"master": {}})");
    auto old_cfg = MasterConfig::load(config_path_);
    EXPECT_EQ(old_cfg.cleanup_max_page_count, 200u);
    EXPECT_EQ(old_cfg.cleanup_max_page_age_hours, 24u);

    // 显式双 0 (不清理) 写入配置文件后 load 读回验证 (模拟人工改配置后重启读取)
    write_json(R"({"master": {"cleanup_max_page_count": 0, "cleanup_max_page_age_hours": 0}})");
    auto cfg = MasterConfig::load(config_path_);
    EXPECT_EQ(cfg.cleanup_max_page_count, 0u);
    EXPECT_EQ(cfg.cleanup_max_page_age_hours, 0u);
}

// ---- ShmGlobalConfig (shm section, 仅 meta_file_size) ----

TEST_F(ConfigTest, ShmGlobalConfigLoadDefault) {
    auto cfg = ShmGlobalConfig::load(tmp_dir_ / "nonexistent.json");
    EXPECT_EQ(cfg.meta_file_size, 1 * 1024 * 1024);
}

TEST_F(ConfigTest, ShmGlobalConfigLoadFromSection) {
    nlohmann::json full = {{"shm",
                            {
                                {"meta_file_size", 2 * 1024 * 1024},
                                {"event", {{"page_size_mb", 32}}}  // event 子段应被忽略
                            }}};
    auto path = tmp_dir_ / "shm_global_test.json";
    std::ofstream(path) << full.dump(2);

    auto cfg = ShmGlobalConfig::load(path);
    EXPECT_EQ(cfg.meta_file_size, 2 * 1024 * 1024);
}

TEST_F(ConfigTest, ShmGlobalConfigSavePreservesEventSubsection) {
    // 初始: shm.meta_file_size=1MB + shm.event={page_size_mb:32,...}
    nlohmann::json full = {{"shm",
                            {{"meta_file_size", 1 * 1024 * 1024},
                             {"event", {{"page_size_mb", 32}, {"check_interval_min", 5}}}}}};
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
    const auto it = std::find_if(cfg.entries.begin(), cfg.entries.end(),
                                 [](const ProcessEntry& e) { return e.name == "dzweb"; });
    ASSERT_NE(it, cfg.entries.end());
    const auto& w = *it;
    EXPECT_EQ(w.category, Category::WebUI);
    // 契约 process: exe 和 start_dir 不持久化, 留空由 launch_child 填充
    EXPECT_TRUE(w.exe.empty());
    EXPECT_TRUE(w.start_dir.empty());
    EXPECT_TRUE(w.restart.enabled);
    EXPECT_EQ(w.restart.max_attempts, 5);
}

TEST_F(ConfigTest, DefaultConfigCreatesParentDirectoriesAndDefaults) {
    const auto path = tmp_dir_ / "nested" / "configs" / "dztraderd.json";
    ASSERT_FALSE(std::filesystem::exists(path.parent_path()));
    generate_default_config(path);
    ASSERT_TRUE(std::filesystem::exists(path));

    // 默认文件是仓库无 *.example 模板时唯一的默认配置守卫
    const auto data = read_json(path);
    EXPECT_EQ(data["log"]["level"], "info");
    EXPECT_EQ(data["log"]["flush_on"], "warning");
    EXPECT_EQ(data["master"]["single_stop_timeout_sec"], 3);
    EXPECT_EQ(data["shm"]["meta_file_size"], 1 * 1024 * 1024);
    EXPECT_EQ(data["shm"]["event"]["page_size_mb"], 32);
    EXPECT_EQ(data["shm"]["event"]["check_interval_min"], 5);

    auto cfg = parse_master_json(path);
    EXPECT_EQ(cfg.master.single_stop_timeout_sec, 3);
    EXPECT_EQ(cfg.master.md_ready_timeout_sec, 5);
    EXPECT_EQ(cfg.shm_global.meta_file_size, 1 * 1024 * 1024);
    // 默认条目: dzweb + internal.dzsysmon
    ASSERT_EQ(cfg.entries.size(), 2u);
    EXPECT_EQ(std::count_if(cfg.entries.begin(), cfg.entries.end(),
                            [](const ProcessEntry& e) { return e.name == "dzweb"; }),
              1);
    EXPECT_EQ(std::count_if(cfg.entries.begin(), cfg.entries.end(),
                            [](const ProcessEntry& e) { return e.name == "dzsysmon"; }),
              1);
}

TEST_F(ConfigTest, DefaultConfigNotOverwriteExisting) {
    auto path = tmp_dir_ / "default_dztraderd.json";
    std::ofstream(path) << R"({"master": {"single_stop_timeout_sec": 10}})";
    generate_default_config(path);
    auto cfg = parse_master_json(path);
    EXPECT_EQ(cfg.master.single_stop_timeout_sec, 10);
    EXPECT_TRUE(cfg.entries.empty());  // 已有文件不含 webui 段, 不覆盖
}

// 默认配置生成器写入 internal.dzsysmon 默认条目（issue #70: 随可执行文件一起加入）
TEST_F(ConfigTest, DefaultConfigIncludesDzsysmonInternalEntry) {
    auto path = tmp_dir_ / "default_internal.json";
    generate_default_config(path);
    const auto data = read_json(path);
    ASSERT_TRUE(data.contains("internal"));
    ASSERT_TRUE(data["internal"].is_object());
    ASSERT_TRUE(data["internal"].contains("dzsysmon"));
    EXPECT_TRUE(data["internal"]["dzsysmon"]["args"].is_array());

    auto cfg = parse_master_json(path);
    const auto it = std::find_if(cfg.entries.begin(), cfg.entries.end(), [](const ProcessEntry& e) {
        return e.category == Category::Internal;
    });
    ASSERT_NE(it, cfg.entries.end());
    EXPECT_EQ(it->name, "dzsysmon");
    EXPECT_TRUE(it->restart.enabled);
    EXPECT_EQ(it->restart.max_attempts, 5);
    EXPECT_EQ(it->restart.backoff_sec, 5);
}

}  // namespace
}  // namespace dztrader::master
