#include <dztrader/platform/log_config.h>

#include <chrono>
#include <fstream>
#include <stdexcept>

namespace dztrader::platform {

LogConfig::LogConfig(std::string instance_id,
                     std::filesystem::path cfg_path,
                     nlohmann::json::json_pointer section)
    : instance_id_(std::move(instance_id)),
      cfg_path_(std::move(cfg_path)),
      section_(std::move(section)),
      cfg_(default_cfg()) {  // 初始化为默认值，load() 前也有合理状态
}

std::optional<spdlog::level::level_enum> LogConfig::parse_level(std::string_view s) noexcept {
    if (s.empty()) {
        return std::nullopt;  // 空串非法
    }
    const auto lvl = spdlog::level::from_str(std::string(s));
    // from_str 对未知串返回 off，与合法 "off" 歧义，需显式区分
    if (lvl == spdlog::level::off) {
        // from_str 大小写敏感（std::find 精确匹配）：仅精确匹配 "off"
        if (s == "off") {
            return spdlog::level::off;
        }
        return std::nullopt;  // 未知串
    }
    return lvl;
}

std::string LogConfig::canonicalize_level(std::string_view s) noexcept {
    auto lvl = parse_level(s);
    if (lvl) {
        return std::string(spdlog::level::to_string_view(*lvl));
    }
    return std::string(s);  // 未校验或非法，原样返回
}

void LogConfig::canonicalize(nlohmann::json& cfg) noexcept {
    if (cfg.contains("level") && cfg["level"].is_string()) {
        cfg["level"] = canonicalize_level(cfg["level"].get_ref<const std::string&>());
    }
    if (cfg.contains("flush_on") && cfg["flush_on"].is_string()) {
        cfg["flush_on"] = canonicalize_level(cfg["flush_on"].get_ref<const std::string&>());
    }
}

std::optional<std::string> LogConfig::validate(const nlohmann::json& cfg) {
    auto check_level = [](const nlohmann::json& v, const char* key) -> bool {
        if (!v.contains(key) || !v[key].is_string()) {
            return false;
        }
        const auto& s = v[key].get_ref<const std::string&>();
        return is_valid_level(s);
    };
    if (!check_level(cfg, "level")) {
        return "invalid log level";
    }
    if (!check_level(cfg, "flush_on")) {
        return "invalid flush_on level";
    }
    return std::nullopt;
}

void LogConfig::apply_to_spdlog(const nlohmann::json& cfg) noexcept {
    try {
        // 防御性：cfg.value() 对非 string 类型会抛 type_error，先检查 is_string
        auto get_lvl = [&cfg](const char* key,
                              spdlog::level::level_enum def) -> spdlog::level::level_enum {
            if (!cfg.contains(key) || !cfg[key].is_string()) {
                return def;
            }
            const auto& s = cfg[key].get_ref<const std::string&>();
            auto parsed = parse_level(s);
            return parsed.value_or(def);
        };
        spdlog::set_level(get_lvl("level", spdlog::level::debug));
        spdlog::default_logger()->flush_on(get_lvl("flush_on", spdlog::level::info));
    } catch (const std::exception& e) {
        // noexcept：防御性兜底（spdlog::set_level / flush_on 实际不会抛），记录后继续
        spdlog::error("log config apply to spdlog failed | error=\"{}\"", e.what());
    }
}

nlohmann::json LogConfig::load_cfg_from_file(bool& degraded) const {
    nlohmann::json raw;
    if (std::filesystem::exists(cfg_path_)) {
        std::ifstream ifs(cfg_path_);
        if (ifs) {
            nlohmann::json full;
            ifs >> full;  // 解析失败抛 std::exception
            if (section_.empty()) {
                raw = full.is_object() ? full : nlohmann::json::object();
                if (!full.is_object()) degraded = true;
            } else if (full.is_object() && full.contains(section_)) {
                raw = full.at(section_);
                if (!raw.is_object()) {
                    degraded = true;  // section 存在但非 object（如被手改成字符串），回退默认并修复
                }
            }
        } else {
            degraded = true;  // 文件存在但打不开:回退默认并尝试修复
        }
    }
    // 补齐缺失字段为默认值，校验类型 + 值有效性（契约第 56-57 行：RTN 的 level/flush_on
    // 必填且为有效 string）
    nlohmann::json result = default_cfg();
    if (raw.is_object()) {
        nlohmann::json candidate = result;
        bool field_degraded = false;
        // 只接受 string 类型且通过 validate 的有效值，否则用默认值并标记修复
        // （防御文件被手改成非法类型或非法级别）
        if (raw.contains("level")) {
            if (raw["level"].is_string()) {
                candidate["level"] = raw["level"];
            } else {
                field_degraded = true;
            }
        }
        if (raw.contains("flush_on")) {
            if (raw["flush_on"].is_string()) {
                candidate["flush_on"] = raw["flush_on"];
            } else {
                field_degraded = true;
            }
        }
        if (auto err = validate(candidate)) {
            field_degraded = true;  // 值非法（如大写 "INFO"）:整配置回退默认
        } else {
            // 规范化：warn/err -> warning/error（文件可能被手动编辑为简写）
            canonicalize(candidate);
            result = std::move(candidate);  // 校验通过，采用文件值
        }
        degraded = degraded || field_degraded;
    }
    return result;
}

void LogConfig::save_cfg_to_file(const nlohmann::json& cfg) const {
    // load-modify-save 保留其他 section，原子写（tmp + rename）
    nlohmann::json full = nlohmann::json::object();
    if (std::filesystem::exists(cfg_path_)) {
        std::ifstream ifs(cfg_path_);
        if (ifs) {
            // 损坏备份（解析失败或根非 object）：备份后用空 object 起步
            // （复用 core::save_json_section 模式，防单 section 写入丢失全部配置）
            auto backup_corrupt = [this]() {
                std::error_code ec;
                auto backup = cfg_path_;
                backup += ".corrupt." + std::to_string(std::chrono::system_clock::to_time_t(
                                            std::chrono::system_clock::now()));
                std::filesystem::rename(cfg_path_, backup, ec);
                if (ec) {
                    SPDLOG_ERROR("backup corrupt config failed | path={} error={}",
                                 cfg_path_.string(), ec.message());
                }
            };
            try {
                ifs >> full;
            } catch (const std::exception&) {
                full = nlohmann::json::object();
                backup_corrupt();
            }
            // 根为数组/字符串等非 object 且需写 section：full[section_] 会抛 type_error，
            // 无法自愈，同样备份后重置（load 侧用 is_object 守卫兜底读取）
            if (!section_.empty() && !full.is_object()) {
                full = nlohmann::json::object();
                backup_corrupt();
            }
        }
    }
    if (section_.empty()) {
        full = cfg;  // root：整个文件就是配置
    } else {
        full[section_] = cfg;
    }
    // tmp + rename 原子写，显式 close 检查（复用 core::save_json_section 模式）
    auto tmp = cfg_path_;
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
        std::filesystem::rename(tmp, cfg_path_, ec);
        if (ec) {
            throw std::runtime_error("rename failed | from=" + tmp.string() +
                                     " to=" + cfg_path_.string());
        }
    } catch (const std::exception&) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        throw;
    }
}

void LogConfig::load() {
    bool degraded = false;
    std::string reason;
    try {
        cfg_ = load_cfg_from_file(degraded);  // 文件不存在/section 缺失返回默认值不抛；JSON 解析失败抛
    } catch (const std::exception& e) {
        // 文件损坏（JSON 解析失败）：用默认值修复，不中断启动
        // 用 spdlog::error 而非 SPDLOG_ERROR：启动期项目 logger 可能尚未初始化
        degraded = true;
        reason = e.what();
        cfg_ = default_cfg();
    }
    if (degraded) {
        if (reason.empty()) {
            reason = "invalid values, using defaults";
        }
        spdlog::error("log config load degraded, using defaults | error=\"{}\"", reason);
        try {
            save_cfg_to_file(cfg_);  // 修复文件（内容与内存值同步）
        } catch (const std::exception& se) {
            // 修复失败：内存值已是默认/降级值，进程能跑，仅告警
            spdlog::error("log config repair save failed | error=\"{}\"", se.what());
        }
    }
    apply_to_spdlog(cfg_);  // 启动期与 spdlog 同步
}

void LogConfig::set_log_config(const nlohmann::json& patch) {
    // 1. patch 必须是 object（契约 payload 是 JSON object）
    if (!patch.is_object()) {
        throw std::runtime_error("patch must be a JSON object");
    }

    // 2. 手动合并：只取 level 和 flush_on，其他字段忽略（兼容性优先，不抛异常）
    //    契约第 28 行：出现的字段覆盖旧值，缺失字段保留
    nlohmann::json merged = cfg_;
    auto apply_field = [&patch](const char* key) -> std::optional<std::string> {
        if (!patch.contains(key)) {
            return std::nullopt;  // 缺失=不修改
        }
        const auto& v = patch[key];
        // 禁止 null（契约第 29 行）
        if (v.is_null()) {
            throw std::runtime_error(std::string("patch contains null field: ") + key);
        }
        // 必须是 string 类型（契约第 17-18 行）
        if (!v.is_string()) {
            throw std::runtime_error(std::string("field must be a string: ") + key);
        }
        std::string s = v.get<std::string>();
        // 禁止空字符串（契约第 17-18 行）
        if (s.empty()) {
            throw std::runtime_error(std::string("patch contains empty string field: ") + key);
        }
        return s;
    };
    auto new_level = apply_field("level");
    auto new_flush_on = apply_field("flush_on");
    if (new_level) {
        merged["level"] = *new_level;
    }
    if (new_flush_on) {
        merged["flush_on"] = *new_flush_on;
    }

    // 3. 校验 merged 值（level/flush_on 必须是有效 spdlog 级别）
    if (auto err = validate(merged)) {
        throw std::runtime_error(*err);  // cfg_ 不变
    }

    // 3b. 规范化：warn/err -> warning/error（契约：存储与 RTN 始终用规范全称）
    canonicalize(merged);

    // 3c. 契约第 33 行：空对象 {} 视为无操作。值无变化时跳过持久化，仍回 RTN（当前值）
    if (merged == cfg_) {
        return;
    }

    // 4. 持久化（失败抛，cfg_ 不变）
    save_cfg_to_file(merged);

    // 5. 应用到 spdlog（noexcept，不抛）
    apply_to_spdlog(merged);

    // 6. 提交
    cfg_ = std::move(merged);
}

}  // namespace dztrader::platform
