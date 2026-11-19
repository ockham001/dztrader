#include <dztrader/platform/auto_login.h>

#include <chrono>
#include <fstream>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace dztrader::platform {

AutoLoginConfig::AutoLoginConfig(std::string instance_id,
                                 std::filesystem::path cfg_path,
                                 shm::MultiWriter& event_writer,
                                 nlohmann::json::json_pointer section,
                                 nlohmann::json default_schedules)
    : instance_id_(std::move(instance_id)),
      cfg_path_(std::move(cfg_path)),
      event_writer_(event_writer),
      section_(std::move(section)),
      default_schedules_(std::move(default_schedules)),
      cfg_(default_cfg()) {  // 初始化为默认值，load() 前也有合理状态
    // 默认排程与文件值/patch 走同一套校验：非法则退化为空数组（归一恒为 no-op，
    // 平台层行为等价于「未注入」），不抛异常——启动不因网关常量写错而中断。
    if (!default_schedules_.is_array()) {
        spdlog::error("auto-login default schedules rejected | reason=\"not an array\"");
        default_schedules_ = nlohmann::json::array();
        cfg_ = default_cfg();
        return;
    }
    for (const auto& s : default_schedules_) {
        if (auto err = validate_schedule(s)) {
            spdlog::error("auto-login default schedules rejected | reason=\"{}\"", *err);
            default_schedules_ = nlohmann::json::array();
            cfg_ = default_cfg();
            return;
        }
    }
}

bool AutoLoginConfig::is_hh_mm(const std::string& s) noexcept {
    if (s.size() != 5 || s[2] != ':') {
        return false;
    }
    for (const int i : {0, 1, 3, 4}) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    const int hh = ((s[0] - '0') * 10) + (s[1] - '0');
    const int mm = ((s[3] - '0') * 10) + (s[4] - '0');
    return hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59;
}

std::optional<std::string> AutoLoginConfig::validate_schedule(const nlohmann::json& s) {
    if (!s.is_object()) {
        return "schedule must be an object";
    }
    if (!s.contains("login_time") || !s["login_time"].is_string()) {
        return "schedule.login_time must be a string";
    }
    if (!s.contains("logout_time") || !s["logout_time"].is_string()) {
        return "schedule.logout_time must be a string";
    }
    const auto& lo = s["login_time"].get_ref<const std::string&>();
    const auto& out = s["logout_time"].get_ref<const std::string&>();
    if (!is_hh_mm(lo)) {
        return "schedule.login_time must be HH:MM (00:00-23:59)";
    }
    if (!is_hh_mm(out)) {
        return "schedule.logout_time must be HH:MM (00:00-23:59)";
    }
    if (lo == out) {
        return "schedule.login_time must not equal logout_time";
    }
    return std::nullopt;
}

std::optional<std::string> AutoLoginConfig::validate(const nlohmann::json& cfg) {
    if (!cfg.is_object()) {
        return "auto-login config must be a JSON object";
    }
    if (!cfg.contains("enabled")) {
        return "missing required field: enabled";
    }
    if (!cfg.contains("schedules")) {
        return "missing required field: schedules";
    }
    // enabled 必须为 bool（契约：任何位置 null 均非法）
    const auto& e = cfg["enabled"];
    if (e.is_null() || !e.is_boolean()) {
        return "enabled must be a boolean";
    }
    // schedules 必须为数组
    const auto& sch = cfg["schedules"];
    if (sch.is_null() || !sch.is_array()) {
        return "schedules must be an array";
    }
    for (const auto& s : sch) {
        if (auto err = validate_schedule(s)) {
            return *err;
        }
    }
    return std::nullopt;
}

nlohmann::json AutoLoginConfig::load_cfg_from_file(bool& degraded,
                                                   bool& schedules_normalized) const {
    nlohmann::json raw;
    if (std::filesystem::exists(cfg_path_)) {
        std::ifstream ifs(cfg_path_);
        if (ifs) {
            nlohmann::json full;
            ifs >> full;  // 解析失败抛 std::exception
            if (section_.empty()) {
                raw = full.is_object() ? full : nlohmann::json::object();
                if (!full.is_object()) {
                    degraded = true;
                }
            } else if (full.is_object() && full.contains(section_)) {
                raw = full.at(section_);
            }
        } else {
            degraded = true;  // 文件存在但打不开:回退默认并尝试修复
        }
    }
    // 补齐缺失字段为默认值，校验类型 + 值有效性
    nlohmann::json result = default_cfg();
    // 文件是否表达了非空排程（用于判断是否需要在加载后材料化内置默认排程）
    bool file_expresses_schedules = false;
    if (raw.is_object()) {
        nlohmann::json candidate = result;
        bool field_degraded = false;
        // 只接受类型正确的值，否则用默认值并标记修复（防御文件被手改成非法类型）
        if (raw.contains("enabled")) {
            if (raw["enabled"].is_boolean()) {
                candidate["enabled"] = raw["enabled"];
            } else {
                field_degraded = true;
            }
        }
        if (raw.contains("schedules")) {
            if (raw["schedules"].is_array()) {
                candidate["schedules"] = raw["schedules"];
                file_expresses_schedules = !candidate["schedules"].empty();
            } else {
                field_degraded = true;
            }
        }
        if (auto err = validate(candidate)) {
            field_degraded = true;  // 值非法（如 login==logout）:整配置回退默认
        } else {
            result = std::move(candidate);  // 校验通过，采用文件值
        }
        degraded = degraded || field_degraded;
    }
    // 空表归一：文件/section 缺失、或 schedules 为空数组/缺失时回落到内置默认排程。
    // 归一实际改变了文件所表达的值才标记材料化；内置默认排程为空时归一恒为 no-op。
    normalize_schedules(result);
    schedules_normalized = !default_schedules_.empty() && !file_expresses_schedules;
    return result;
}

void AutoLoginConfig::normalize_schedules(nlohmann::json& cfg) const {
    if (default_schedules_.empty()) {
        return;  // 无内置默认排程：空表归一不适用（平台层行为不变）
    }
    auto it = cfg.find("schedules");
    if (it == cfg.end() || !it->is_array() || !it->empty()) {
        return;  // 已是非空排程或非法类型（后者由 validate 处理）
    }
    *it = default_schedules_;
}

void AutoLoginConfig::save_cfg_to_file(const nlohmann::json& cfg) const {
    // load-modify-save 保留其他 section，原子写（tmp + rename）
    nlohmann::json full = nlohmann::json::object();
    if (std::filesystem::exists(cfg_path_)) {
        std::ifstream ifs(cfg_path_);
        if (ifs) {
            // 损坏备份（解析失败或根非 object）：备份后用空 object 起步
            auto backup_corrupt = [this]() {
                std::error_code ec;
                auto backup = cfg_path_;
                backup += ".corrupt." + std::to_string(std::chrono::system_clock::to_time_t(
                                            std::chrono::system_clock::now()));
                std::filesystem::rename(cfg_path_, backup, ec);
            };
            try {
                ifs >> full;
            } catch (const std::exception&) {
                full = nlohmann::json::object();
                backup_corrupt();
            }
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
    // tmp + rename 原子写
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

void AutoLoginConfig::load() {
    bool degraded = false;
    bool schedules_normalized = false;
    std::string reason;
    try {
        cfg_ = load_cfg_from_file(degraded, schedules_normalized);
    } catch (const std::exception& e) {
        // 文件损坏（JSON 解析失败）：用默认值修复，不中断启动
        degraded = true;
        reason = e.what();
        cfg_ = default_cfg();
    }
    // 内容非法 → 修复写回；空表归一改变了文件所表达的值 → 材料化默认表；值未变 → 不写。
    // 两条原因分别记日志：材料化单独成立时也必须可审计（文件内容变了要有解释）。
    if (degraded) {
        if (reason.empty()) {
            reason = "invalid values, using defaults";
        }
        spdlog::error("auto-login config load degraded, using defaults | error=\"{}\"", reason);
    } else if (schedules_normalized) {
        spdlog::info("auto-login default schedules materialized | path={} schedules={}",
                     cfg_path_.string(), cfg_["schedules"].size());
    }
    if (degraded || schedules_normalized) {
        try {
            save_cfg_to_file(cfg_);  // 修复/材料化文件（内容与内存值同步）
        } catch (const std::exception& se) {
            // 修复失败：内存值已是默认/降级值，进程能跑，仅告警
            spdlog::error("auto-login config repair save failed | error=\"{}\"", se.what());
        }
    }
}

void AutoLoginConfig::set_auto_login(const nlohmann::json& patch) {
    // 1. patch 必须是 object（契约 payload 是 JSON object）
    if (!patch.is_object()) {
        throw std::runtime_error("patch must be a JSON object");
    }

    // 2. 合并：enabled 出现则覆盖；schedules 出现则整体覆盖（契约 auto-login）
    //    其他字段忽略（前向兼容）。null 任何位置非法（契约 auto-login）。
    nlohmann::json merged = cfg_;
    if (patch.contains("enabled")) {
        const auto& v = patch["enabled"];
        if (v.is_null()) {
            throw std::runtime_error("patch contains null field: enabled");
        }
        if (!v.is_boolean()) {
            throw std::runtime_error("field must be a boolean: enabled");
        }
        merged["enabled"] = v;
    }
    if (patch.contains("schedules")) {
        const auto& v = patch["schedules"];
        if (v.is_null()) {
            throw std::runtime_error("patch contains null field: schedules");
        }
        if (!v.is_array()) {
            throw std::runtime_error("field must be an array: schedules");
        }
        // 逐条校验排程元素（login_time/logout_time HH:MM、login != logout）
        for (const auto& s : v) {
            if (auto err = validate_schedule(s)) {
                throw std::runtime_error(*err);
            }
        }
        merged["schedules"] = v;  // 整体覆盖（非递归合并）
    }

    // 2b. 空表归一：空数组表示「回落到网关内置默认排程」，不是清空。
    //     放在幂等判定之前，使「当前值已等于默认表时再提交 []」不产生磁盘写入。
    normalize_schedules(merged);

    // 3. 校验 merged 完整配置（safety net：cfg_ 已合法，patch 字段已在上面校验）
    if (auto err = validate(merged)) {
        throw std::runtime_error(*err);  // cfg_ 不变
    }

    // 3b. 契约 auto-login：空对象 {} 视为无操作。值无变化时跳过持久化，仍回 RTN（当前值）
    if (merged == cfg_) {
        return;
    }

    // 4. 持久化（失败抛，cfg_ 不变）
    save_cfg_to_file(merged);

    // 5. 提交
    cfg_ = std::move(merged);
}

}  // namespace dztrader::platform
