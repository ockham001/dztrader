/**
 * @file config.cpp
 * @brief 后端配置解析单点实现（ADR 0016 决策 3）
 *
 * 读取顺序与合并规则见 config.h；配置文件缺失或 domain 段缺失时用
 * core::save_json_section 原子写生成默认段（不提交模板文件）。
 */
#include <dztrader/db/config.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <boost/process/v2/environment.hpp>
#include <nlohmann/json.hpp>

#include <dztrader/core/env.h>
#include <dztrader/core/exception.h>
#include <dztrader/core/json_section.h>
#include <dztrader/core/path.h>
#include <dztrader/error.h>

#include "driver_registry.h"

namespace dztrader::db {

namespace {

namespace bp2env = boost::process::v2::environment;

constexpr std::string_view kConfigFileName = "database.json";
constexpr std::string_view kEnvBackend = "DZTRADER_DB_BACKEND";
constexpr std::string_view kEnvOptPrefix = "DZTRADER_DB_OPT_";
constexpr std::string_view kKeyBackend = "backend";
constexpr std::string_view kKeyOptions = "options";
constexpr std::string_view kKeyPath = "path";
constexpr std::string_view kBackendSqlite = "sqlite";
constexpr std::string_view kBackendMysql = "mysql";
constexpr std::string_view kDefaultMysqlHost = "127.0.0.1";
constexpr std::string_view kDefaultMysqlPort = "3306";
constexpr std::string_view kDefaultMysqlDatabase = "dztrader";
constexpr std::string_view kDefaultConnectTimeoutMs = "5000";
constexpr std::string_view kDefaultBusyTimeoutMs = "5000";

using OptionsMap = std::map<std::string, std::string, std::less<>>;

/// 配置文件绝对路径：<DZTRADER_HOME>/configs/database.json
std::filesystem::path config_path() { return paths::configs() / kConfigFileName; }

/// `DZTRADER_DB_OPT_<KEY>` 的 KEY 允许字符集（spec §6.1 冻结为 [A-Z0-9_]）
bool is_env_key_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::string to_lower_ascii(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

/// 内置默认配置：backend 已确定，返回该后端的内置 options 默认（spec §6.3）
Config builtin_default(std::string_view backend) {
    Config config;
    config.backend = std::string(backend);
    if (backend == kBackendSqlite) {
        config.options[std::string(kKeyPath)] = paths::td_db().string();
        config.options[std::string("busy_timeout_ms")] = std::string(kDefaultBusyTimeoutMs);
    } else if (backend == kBackendMysql) {
        config.options[std::string("host")] = std::string(kDefaultMysqlHost);
        config.options[std::string("port")] = std::string(kDefaultMysqlPort);
        config.options[std::string("user")] = "";
        config.options[std::string("password")] = "";
        config.options[std::string("database")] = std::string(kDefaultMysqlDatabase);
        config.options[std::string("connect_timeout_ms")] = std::string(kDefaultConnectTimeoutMs);
    }
    return config;
}

/// options 值允许标量（字符串/数字/布尔），统一转字符串；容器/null 视为配置错误
std::string scalar_to_string(const nlohmann::json& value, const std::string& key) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
    }
    if (value.is_number_float()) {
        return value.dump();
    }
    throw Exception(DZ_EC_INVALID_PARAM,
                    "database config option must be scalar | key={} type={}", key,
                    value.type_name());
}

/// 已解析的 domain 段；found = 段存在（不存在则调用方生成默认）
struct FileSection {
    bool found = false;
    std::string backend;
    OptionsMap options;
};

/// 读配置文件并取 domain 段；JSON 解析失败/结构非法抛 DZ_EC_INVALID_PARAM（不静默回落）
FileSection load_file_section(const std::filesystem::path& path, std::string_view domain) {
    FileSection section;
    std::error_code exists_ec;
    if (!std::filesystem::exists(path, exists_ec) || exists_ec) {
        return section;  // 文件缺失: 调用方生成默认段
    }
    std::ifstream ifs(path);
    if (!ifs) {
        throw Exception(DZ_EC_SYSTEM, "database config unreadable | path={}", path.string());
    }
    nlohmann::json full;
    try {
        ifs >> full;
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_INVALID_PARAM,
                        "database config parse failed | path={} error=\"{}\"", path.string(),
                        e.what());
    }
    if (!full.is_object()) {
        throw Exception(DZ_EC_INVALID_PARAM, "database config root must be object | path={}",
                        path.string());
    }
    const std::string domain_key(domain);
    const auto section_it = full.find(domain_key);
    if (section_it == full.end()) {
        return section;  // 段缺失: 调用方生成默认段
    }
    if (!section_it->is_object()) {
        throw Exception(DZ_EC_INVALID_PARAM,
                        "database config section must be object | domain={}", domain_key);
    }
    section.found = true;

    const auto backend_it = section_it->find(std::string(kKeyBackend));
    if (backend_it != section_it->end()) {
        if (!backend_it->is_string()) {
            throw Exception(DZ_EC_INVALID_PARAM,
                            "database config backend must be string | domain={}", domain_key);
        }
        section.backend = backend_it->get<std::string>();
    }
    const auto options_it = section_it->find(std::string(kKeyOptions));
    if (options_it != section_it->end()) {
        if (!options_it->is_object()) {
            throw Exception(DZ_EC_INVALID_PARAM,
                            "database config options must be object | domain={}", domain_key);
        }
        for (auto opt = options_it->begin(); opt != options_it->end(); ++opt) {
            section.options[opt.key()] = scalar_to_string(opt.value(), opt.key());
        }
    }
    return section;
}

/// 段缺失时生成默认段（原子写；保留文件中其他段）
void generate_default_section(const std::filesystem::path& path, std::string_view domain) {
    // 生成内容与 spec §6.2 一致: path 不写入（解析期回落到 paths::td_db()），
    // 使文件与 DZTRADER_HOME 位置解耦。
    nlohmann::json options = nlohmann::json::object();
    options["busy_timeout_ms"] = std::string(kDefaultBusyTimeoutMs);
    nlohmann::json section = nlohmann::json::object();
    section[std::string(kKeyBackend)] = std::string(kBackendSqlite);
    section[std::string(kKeyOptions)] = std::move(options);
    try {
        core::save_json_section(path, std::string(domain), section);
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_SYSTEM,
                        "database config generate failed | path={} error=\"{}\"", path.string(),
                        e.what());
    }
}

/// 收集 DZTRADER_DB_OPT_<KEY>；非法 KEY（非 [A-Z0-9_]）与空值忽略（向前兼容新驱动选项）
OptionsMap env_options() {
    OptionsMap out;
    // key_value_pair_view 的成员为 private（结构化绑定取不到），只用 key()/value() 访问器
    for (const auto& entry : bp2env::current()) {
        const std::string key = entry.key().string();
        if (!key.starts_with(kEnvOptPrefix)) {
            continue;
        }
        const std::string_view suffix(key.data() + kEnvOptPrefix.size(),
                                      key.size() - kEnvOptPrefix.size());
        if (suffix.empty() ||
            !std::all_of(suffix.begin(), suffix.end(), is_env_key_char)) {
            continue;
        }
        const std::string value = entry.value().string();
        if (value.empty()) {
            continue;  // 空值视为未设置（避免用空串覆盖文件里的有效值）
        }
        out[to_lower_ascii(suffix)] = value;
    }
    return out;
}

}  // namespace

Config resolve_config(std::string_view domain) {
    const std::filesystem::path path = config_path();
    const FileSection section = load_file_section(path, domain);
    if (!section.found) {
        generate_default_section(path, domain);
    }

    // 优先级: 内置默认(按最终 backend) < 文件段 < 环境变量
    std::string backend =
        section.backend.empty() ? std::string(kBackendSqlite) : section.backend;
    if (const auto env_backend = env::get(kEnvBackend);
        env_backend.has_value() && !env_backend->empty()) {
        backend = *env_backend;
    }

    Config config = builtin_default(backend);
    for (const auto& [key, value] : section.options) {
        config.options[key] = value;
    }
    for (const auto& [key, value] : env_options()) {
        config.options[key] = value;
    }
    return config;
}

bool backend_available(std::string_view backend) noexcept {
    try {
        internal::ensure_builtin_drivers_registered();
        return internal::has_driver(backend);
    } catch (...) {
        return false;
    }
}

}  // namespace dztrader::db
