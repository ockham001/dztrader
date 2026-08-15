#include "config.h"

#include <dztrader/core/env.h>
#include <dztrader/core/exception.h>
#include <dztrader/core/json_section.h>
#include <dztrader/error.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>

namespace dztrader::master {

namespace {

platform::RestartPolicy parse_restart(const nlohmann::json& val, Category cat) {
    auto policy = default_restart_policy(cat);
    if (!val.contains("restart")) return policy;
    const auto& r = val["restart"];
    if (r.contains("enabled")) policy.enabled = r["enabled"].get<bool>();
    if (r.contains("max_attempts")) policy.max_attempts = r["max_attempts"].get<int>();
    if (r.contains("backoff_sec")) policy.backoff_sec = r["backoff_sec"].get<int>();
    return policy;
}

std::unordered_map<std::string, std::string> parse_env(const nlohmann::json& val) {
    std::unordered_map<std::string, std::string> result;
    if (!val.contains("env")) return result;
    for (const auto& [k, v] : val["env"].items()) {
        result[k] = v.get<std::string>();
    }
    return result;
}

std::vector<std::string> parse_args(const nlohmann::json& val) {
    if (!val.contains("args")) return {};
    std::vector<std::string> args;
    for (const auto& a : val["args"]) {
        args.push_back(a.get<std::string>());
    }
    return args;
}

std::filesystem::path resolve_exe(const std::string& exe_str) {
    std::filesystem::path exe_path(exe_str);
    if (exe_path.is_absolute()) return exe_path;
    auto home = dztrader::env::get("DZTRADER_HOME");
    if (home) return std::filesystem::path(*home) / exe_path;
    return exe_path;
}

/// 从 md/td section 的 JSON object 解析单个 ProcessEntry
ProcessEntry parse_gateway_entry(const std::string& name, Category cat, const nlohmann::json& val) {
    ProcessEntry entry;
    entry.name = name;
    entry.category = cat;
    // exe 和 start_dir 留空, 由 launch_child 调 find_exe_by_stem 实时扫描填充 (契约 04/05)
    entry.args = parse_args(val);
    entry.env = parse_env(val);
    entry.restart = parse_restart(val, cat);
    entry.display_name = val.value("display_name", "");
    return entry;
}

}  // namespace

const char* category_str(Category cat) {
    switch (cat) {
        case Category::GatewayMd:  return "md";
        case Category::GatewayTd:  return "td";
        case Category::Strategy:    return "stg";
        case Category::WebUI:       return "webui";
    }
    return "unknown";
}

platform::RestartPolicy default_restart_policy(Category cat) {
    switch (cat) {
        case Category::GatewayMd:
        case Category::GatewayTd:
        case Category::WebUI:
            return {.enabled = true, .max_attempts = 5, .backoff_sec = 5};
        case Category::Strategy:
            return {.enabled = false, .max_attempts = 0, .backoff_sec = 5};
    }
    return {};
}

Config parse_master_json(const std::filesystem::path& config_path) {
    if (!std::filesystem::exists(config_path)) {
        throw std::runtime_error("config file not found | path=" + config_path.string());
    }
    std::ifstream ifs(config_path);
    if (!ifs) {
        throw std::runtime_error("cannot open config file | path=" + config_path.string());
    }
    nlohmann::json data;
    try {
        ifs >> data;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::format("JSON parse failed | path={} error=\"{}\"",
                                             config_path.string(), e.what()));
    }
    if (!data.is_object()) {
        throw std::runtime_error("config root must be JSON object | path=" + config_path.string());
    }

    Config cfg;

    // log section 不在此加载——ShmManager 内部的 LogConfig 类自行加载

    // master section
    cfg.master = MasterConfig::load(config_path, "master");
    if (cfg.master.single_stop_timeout_sec < 1) {
        SPDLOG_WARN("single_stop_timeout_sec too small, clamped to 1 | config={}",
                    cfg.master.single_stop_timeout_sec);
        cfg.master.single_stop_timeout_sec = 1;
    }

    // shm section (ShmGlobalConfig::load 只读 meta_file_size, 忽略 event 子段)
    cfg.shm_global = ShmGlobalConfig::load(config_path, "shm");

    // shm.event 子段不在此加载——ShmManager 内部的 EventShmConfig 类自行加载

    // md section: 行情网关 (key=value 形式,key=进程名)
    if (data.contains("md") && data["md"].is_object()) {
        for (const auto& [name, val] : data["md"].items()) {
            cfg.entries.push_back(parse_gateway_entry(name, Category::GatewayMd, val));
        }
    }

    // td section: 交易网关
    if (data.contains("td") && data["td"].is_object()) {
        for (const auto& [name, val] : data["td"].items()) {
            cfg.entries.push_back(parse_gateway_entry(name, Category::GatewayTd, val));
        }
    }

    // webui section (单例,与 master 同级)
    if (data.contains("webui") && data["webui"].is_object()) {
        ProcessEntry entry;
        entry.name = "dzweb";
        entry.category = Category::WebUI;
        entry.args = parse_args(data["webui"]);
        entry.env = parse_env(data["webui"]);
        entry.restart = parse_restart(data["webui"], Category::WebUI);
        entry.display_name = data["webui"].value("display_name", "");
        cfg.entries.push_back(std::move(entry));
    }

    // strategy section (数组)
    if (data.contains("strategy") && data["strategy"].is_array()) {
        for (const auto& val : data["strategy"]) {
            ProcessEntry entry;
            entry.name = val.at("name").get<std::string>();
            entry.category = Category::Strategy;
            entry.exe = resolve_exe(val.at("exe").get<std::string>());
            entry.args = parse_args(val);
            entry.env = parse_env(val);
            if (val.contains("start_dir") && !val["start_dir"].get<std::string>().empty()) {
                entry.start_dir = val["start_dir"].get<std::filesystem::path>();
            } else {
                entry.start_dir = entry.exe.parent_path();
            }
            entry.restart = parse_restart(val, Category::Strategy);
            cfg.entries.push_back(std::move(entry));
        }
    }

    return cfg;
}

void write_gateway_section(const std::filesystem::path& config_path,
                           Category category,
                           const std::string& gateway_name,
                           const std::vector<std::string>& args,
                           const platform::RestartPolicy& restart,
                           const std::string& display_name) {
    std::string section = (category == Category::GatewayMd) ? "md" : "td";

    // load-modify-save: 读现有 section (map<name, entry>) -> 修改单个 gateway -> 原子写回
    nlohmann::json section_obj = nlohmann::json::object();
    if (std::filesystem::exists(config_path)) {
        std::ifstream ifs(config_path);
        if (ifs) {
            try {
                nlohmann::json full;
                ifs >> full;
                if (full.is_object() && full.contains(section)) {
                    section_obj = full[section];
                }
            } catch (const std::exception&) {
                // 旧文件损坏,save_json_section 内部已处理备份
            }
        }
    }

    // 构造单个 gateway 子对象 (契约 05: 不写 exe/start_dir)
    nlohmann::json gw;
    gw["args"] = args;
    gw["restart"] = {
        {"enabled", restart.enabled},
        {"max_attempts", restart.max_attempts},
        {"backoff_sec", restart.backoff_sec}
    };
    if (!display_name.empty()) gw["display_name"] = display_name;
    section_obj[gateway_name] = gw;

    // 原子写整个 section (tmp+rename,保留其他 section 不变)
    dztrader::core::save_json_section<nlohmann::json>(config_path, section, section_obj);
}

void write_webui_section(const std::filesystem::path& config_path,
                         const std::vector<std::string>& args,
                         const platform::RestartPolicy& restart,
                         const std::string& display_name) {
    nlohmann::json webui_obj;
    webui_obj["args"] = args;
    webui_obj["restart"] = {
        {"enabled", restart.enabled},
        {"max_attempts", restart.max_attempts},
        {"backoff_sec", restart.backoff_sec}
    };
    if (!display_name.empty()) webui_obj["display_name"] = display_name;

    dztrader::core::save_json_section<nlohmann::json>(config_path, "webui", webui_obj);
}

void remove_gateway_section(const std::filesystem::path& config_path,
                            Category category,
                            const std::string& gateway_name) {
    std::string section = (category == Category::GatewayMd) ? "md" : "td";

    // load-modify-save: 读 section -> erase key -> 原子写回
    if (!std::filesystem::exists(config_path)) return;
    nlohmann::json section_obj = nlohmann::json::object();
    {
        std::ifstream ifs(config_path);
        if (!ifs) return;
        try {
            nlohmann::json full;
            ifs >> full;
            if (!full.is_object() || !full.contains(section)) return;
            section_obj = full[section];
        } catch (const std::exception& e) {
            throw std::runtime_error(std::format(
                "failed to parse {} for remove gateway | error=\"{}\"",
                config_path.string(), e.what()));
        }
    }
    if (!section_obj.is_object() || !section_obj.contains(gateway_name)) return;
    section_obj.erase(gateway_name);

    dztrader::core::save_json_section<nlohmann::json>(config_path, section, section_obj);
}

void remove_webui_section(const std::filesystem::path& config_path) {
    // load-modify-save: 读全量 json -> erase "webui" key -> 原子写回
    if (!std::filesystem::exists(config_path)) return;
    nlohmann::json full;
    {
        std::ifstream ifs(config_path);
        if (!ifs) return;
        try {
            ifs >> full;
        } catch (const std::exception& e) {
            throw std::runtime_error(std::format(
                "failed to parse {} for remove webui | error=\"{}\"",
                config_path.string(), e.what()));
        }
    }
    if (!full.is_object() || !full.contains("webui")) return;
    full.erase("webui");

    // 原子写回整个文件 (tmp+rename)，错误处理与 save_json_section 一致
    auto tmp = config_path;
    tmp += ".tmp";
    try {
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                throw std::runtime_error("open failed | path=" + tmp.string());
            }
            ofs << full.dump(2);
            ofs.flush();
            if (!ofs) {
                throw std::runtime_error("write failed | path=" + tmp.string());
            }
            ofs.close();
            if (!ofs) {
                throw std::runtime_error("close failed | path=" + tmp.string());
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, config_path, ec);
        if (ec) {
            throw std::runtime_error("rename failed | from=" + tmp.string() + " to=" + config_path.string());
        }
    } catch (const std::exception&) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        throw;
    }
}

void generate_default_config(const std::filesystem::path& json_path) {
    if (std::filesystem::exists(json_path)) return;

    std::error_code ec;
    std::filesystem::create_directories(json_path.parent_path(), ec);

    nlohmann::json shm_event = nlohmann::json::object();
    shm_event["page_size_mb"] = 32;
    shm_event["preload_points"] = nlohmann::json::object();
    shm_event["preload_points"]["08:45"] = {{"pages", 1}, {"bytes", 0}};
    shm_event["check_interval_min"] = 5;
    shm_event["check_pages"] = 1;
    shm_event["check_bytes"] = 0;

    nlohmann::json full = {
        {"log", {{"level", "info"}, {"flush_on", "warning"}}},
        {"master", {{"single_stop_timeout_sec", 3}}},
        {"shm", {
            {"meta_file_size", 1 * 1024 * 1024},
            {"event", shm_event}
        }}
    };

    std::ofstream ofs(json_path);
    if (!ofs) {
        throw std::runtime_error("无法创建配置文件: " + json_path.string());
    }
    ofs << full.dump(2);
}

// ---- MasterConfig ----

MasterConfig MasterConfig::load(const std::filesystem::path& path, const std::string& section) {
    return dztrader::core::load_json_section<MasterConfig>(path, section);
}

void MasterConfig::save(const std::filesystem::path& path, const std::string& section) const {
    // load-modify-save: 读现有 section -> 更新 single_stop_timeout_sec -> 原子写回 (保留其他字段)
    nlohmann::json section_obj = nlohmann::json::object();
    if (std::filesystem::exists(path)) {
        std::ifstream ifs(path);
        if (ifs) {
            try {
                nlohmann::json full;
                ifs >> full;
                if (full.is_object() && full.contains(section)) {
                    section_obj = full[section];
                }
            } catch (const std::exception&) {
                // 旧文件损坏, 用空 object 起步 (save_json_section 内部已处理)
            }
        }
    }
    // 只更新 single_stop_timeout_sec, 保留其他字段
    section_obj["single_stop_timeout_sec"] = single_stop_timeout_sec;
    // 用 save_json_section 写整个 section (原子 tmp+rename, 保留其他 section 不变)
    dztrader::core::save_json_section<nlohmann::json>(path, section, section_obj);
}

// ---- ShmGlobalConfig ----

ShmGlobalConfig ShmGlobalConfig::load(const std::filesystem::path& path, const std::string& section) {
    // NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT 只反序列化 meta_file_size 字段,
    // event 子段自动忽略
    return dztrader::core::load_json_section<ShmGlobalConfig>(path, section);
}

void ShmGlobalConfig::save(const std::filesystem::path& path, const std::string& section) const {
    // load-modify-save: 读现有 "shm" section -> 更新 meta_file_size -> 原子写回 (保留 event 子段)
    nlohmann::json shm_section = nlohmann::json::object();
    if (std::filesystem::exists(path)) {
        std::ifstream ifs(path);
        if (ifs) {
            try {
                nlohmann::json full;
                ifs >> full;
                if (full.is_object() && full.contains(section)) {
                    shm_section = full[section];
                }
            } catch (const std::exception&) {
                // 旧文件损坏, 用空 object 起步 (save_json_section 内部已处理)
            }
        }
    }
    // 只更新 meta_file_size, 保留其他字段 (event 子段等)
    shm_section["meta_file_size"] = meta_file_size;

    // 用 save_json_section 写整个 "shm" section (原子 tmp+rename)
    dztrader::core::save_json_section<nlohmann::json>(path, section, shm_section);
}

}  // namespace dztrader::master
