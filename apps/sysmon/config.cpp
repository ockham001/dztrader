#include "config.h"

#include <dztrader/platform/sysmon.h>

#include <spdlog/spdlog.h>

#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

using nlohmann::json;

/// 只保留六个业务配置段，剔除 log 与其它未知 section。
json extract_sections(const json& full) {
    json cfg = json::object();
    for (const char* section : platform::SYSMON_CONFIG_SECTIONS) {
        if (full.contains(section)) {
            cfg[section] = full.at(section);
        }
    }
    return cfg;
}

/// 原子写 JSON 文件（tmp + rename）：失败清理 tmp，不留下半截文件。
/// @throws std::runtime_error 目录创建/写入/close/rename 失败
void write_json_atomic(const std::filesystem::path& path, const json& full) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw std::runtime_error("config dir create failed | path=" + parent.string() +
                                     " error=\"" + ec.message() + "\"");
        }
    }
    auto tmp = path;
    tmp += ".tmp";
    try {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            throw std::runtime_error("config write failed | path=" + tmp.string());
        }
        ofs << full.dump(2);
        ofs.flush();
        if (!ofs) {
            throw std::runtime_error("config write failed | path=" + tmp.string());
        }
        // 显式 close 并检查：磁盘满/配额耗尽时 ofstream 析构会丢弃错误，
        // 直接 rename 会把截断文件提升为正式配置文件。
        ofs.close();
        if (!ofs) {
            throw std::runtime_error("config write failed | path=" + tmp.string());
        }
        std::error_code rename_ec;
        std::filesystem::rename(tmp, path, rename_ec);
        if (rename_ec) {
            throw std::runtime_error("config rename failed | from=" + tmp.string() + " to=" +
                                     path.string() + " error=\"" + rename_ec.message() + "\"");
        }
    } catch (const std::exception&) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        throw;
    }
}

/// 读取并解析 JSON 对象文件（不检查存在性；调用方负责）。
/// @throws std::runtime_error 打开/解析失败或根不是 object（英文小写无句号）
json read_json_object(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("config read failed | path=" + path.string());
    }
    json value;
    try {
        ifs >> value;
    } catch (const std::exception& e) {
        throw std::runtime_error("config parse failed | path=" + path.string() + " error=\"" +
                                 e.what() + "\"");
    }
    if (!value.is_object()) {
        throw std::runtime_error("config parse failed | path=" + path.string() +
                                 " reason=root_not_object");
    }
    return value;
}

/// `thresholds` 段里可从旧 `alerts` 段映射过来的键（契约 sysmon §3.1）。
/// 顺序即输出顺序，也即日志里 `mapped_keys` 的展示顺序。
constexpr std::array<const char*, 5> kMigratableThresholdKeys = {
    "cpu_sustain_used_pct", "cpu_sustain_samples", "memory_used_pct", "swap_used_pct",
    "volume_free_pct"};

/// 旧 `alerts` 段里因能力删除而丢弃的键（`volume_free_bytes` 绝对容量形态、
/// `cooldown_sec` 通知冷却），日志里如实报告丢弃，便于用户核对。
constexpr std::array<const char*, 2> kDroppedLegacyKeys = {"volume_free_bytes", "cooldown_sec"};

/// 把字符串列表拼成日志里的一段（逗号分隔；空列表返回 `-`，避免 `key=` 后空值难辨）。
std::string join_keys(const std::vector<std::string>& keys) {
    if (keys.empty()) {
        return "-";
    }
    std::string out;
    for (const auto& key : keys) {
        if (!out.empty()) {
            out += ',';
        }
        out += key;
    }
    return out;
}

}  // namespace

ConfigMigration migrate_legacy_alerts(json& full) {
    ConfigMigration migration;
    const auto legacy_it = full.find(std::string(platform::SYSMON_LEGACY_ALERTS_SECTION));
    if (legacy_it == full.end()) {
        return migration;
    }
    migration.migrated = true;
    // 旧段不是对象（被手工改坏）：丢弃它并让后续校验按"缺 thresholds 段"处理——
    // 迁移不是校验，不在这里判断合法性。
    if (!legacy_it->is_object()) {
        full.erase(legacy_it);
        return migration;
    }
    json migrated = json::object();
    for (const char* key : kMigratableThresholdKeys) {
        if (legacy_it->contains(key)) {
            migrated[key] = (*legacy_it)[key];
            migration.mapped_keys.emplace_back(key);
        }
    }
    for (const char* key : kDroppedLegacyKeys) {
        if (legacy_it->contains(key)) {
            migration.dropped_keys.emplace_back(key);
        }
    }
    // 已存在的新段优先：用户可能已经手工写好新形状，不能被旧值覆盖。
    if (full.contains(platform::SYSMON_THRESHOLDS_SECTION)) {
        const auto& existing = full.at(platform::SYSMON_THRESHOLDS_SECTION);
        if (existing.is_object()) {
            json merged = migrated;
            for (const auto& [key, value] : existing.items()) {
                merged[key] = value;
            }
            migrated = std::move(merged);
        }
    }
    full[platform::SYSMON_THRESHOLDS_SECTION] = std::move(migrated);
    // 旧段必须删除：留着会让下次启动重复迁移，也会让「文件里有两份阈值」长期存在。
    full.erase(legacy_it);
    return migration;
}

json default_config_json() {
    json cfg = platform::SysmonConfig::default_config();
    cfg["log"] = {{"level", "info"}, {"flush_on", "warning"}};
    return cfg;
}

void generate_default_config(const std::filesystem::path& path) {
    std::error_code exists_ec;
    if (std::filesystem::exists(path, exists_ec)) {
        return;  // 已存在不覆盖（用户手工编辑优先）
    }
    write_json_atomic(path, default_config_json());
}

json load_config(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        generate_default_config(path);  // 缺失自愈：先生成默认文件再按正常路径加载
    }
    json user = read_json_object(path);

    // 旧配置迁移（契约 sysmon §3.1）：先就地迁移再合并，命中时写回文件并记一条日志。
    // 迁移失败不影响启动（写回失败只记日志）：内存里已完成迁移，进程照常运行，
    // 下次启动会再试一次——比"因为写不了文件就起不来"更符合 7x24 进程的要求。
    if (const auto migration = migrate_legacy_alerts(user); migration.migrated) {
        SPDLOG_INFO(
            "legacy config section migrated | from={} to={} mapped_keys={} "
            "dropped_keys={}",
            platform::SYSMON_LEGACY_ALERTS_SECTION, platform::SYSMON_THRESHOLDS_SECTION,
            join_keys(migration.mapped_keys), join_keys(migration.dropped_keys));
        try {
            write_json_atomic(path, user);
        } catch (const std::exception& e) {
            SPDLOG_WARN("legacy config write-back failed | path={} error=\"{}\"", path.string(),
                        e.what());
        }
    }

    // 以默认配置为基底 merge patch：用户文件只需写差异项即可。
    json merged = default_config_json();
    merged.merge_patch(user);
    json cfg = extract_sections(merged);
    if (auto err = platform::SysmonConfig::validate(cfg)) {
        throw std::runtime_error("sysmon config invalid | " + *err);
    }
    // 结构体往返归一化：剔除未知字段、补齐类型层默认值，保证 RTN 与落盘形状稳定。
    const platform::SysmonConfig typed = cfg.get<platform::SysmonConfig>();
    return json(typed);
}

SysmonConfigStore::SysmonConfigStore(std::filesystem::path path, json current)
    : path_(std::move(path)),
      current_(std::move(current)) {}

void SysmonConfigStore::set_patch(const json& patch) {
    // apply_patch 内部校验 patch 类型/空值并 merge + validate（失败抛，current_ 不变）。
    json merged = platform::SysmonConfig::apply_patch(current_, patch);
    // 结构体往返归一化：未知字段忽略（契约 sysmon §3 / general §8）。
    const platform::SysmonConfig typed = merged.get<platform::SysmonConfig>();
    json normalized = typed;
    if (auto err = platform::SysmonConfig::validate(normalized)) {
        throw std::runtime_error("sysmon config invalid | " + *err);
    }
    // 先持久化后更新内存：持久化失败时内存镜像保持旧值（强异常安全）。
    save(normalized);
    current_ = std::move(normalized);
}

void SysmonConfigStore::save(const json& cfg) const {
    // load-modify-save：读现有整文件，只替换六个业务配置段，log 与未知 section 原样保留。
    json full = std::filesystem::exists(path_) ? read_json_object(path_) : default_config_json();
    for (const char* section : platform::SYSMON_CONFIG_SECTIONS) {
        if (cfg.contains(section)) {
            full[section] = cfg.at(section);
        }
    }
    write_json_atomic(path_, full);
}

}  // namespace dztrader::sysmon
