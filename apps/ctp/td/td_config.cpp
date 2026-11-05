#include "td/td_config.h"

#include <format>
#include <fstream>
#include <set>

#include <spdlog/spdlog.h>

#include <dztrader/core/json_section.h>

namespace dztrader::ctp {

// --- TdConfig JSON 序列化 ---
// to_json / from_json 由 NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT 宏生成 (头文件内)
// to_safe_json 需手写: password 脱敏

nlohmann::json TdConfig::to_safe_json() const {
    nlohmann::json j = *this;  // 调用宏生成的 to_json
    // 脱敏: accounts 数组中每个账户的 ext.password 替换为 "****"（契约 td-config §2.3）
    if (j.contains("accounts") && j["accounts"].is_array()) {
        for (auto& acct : j["accounts"]) {
            if (acct.contains("ext") && acct["ext"].contains("password")) {
                acct["ext"]["password"] = "****";
            }
        }
    }
    return j;
}

// --- 文件持久化 ---

TdConfig TdConfig::load(const std::filesystem::path& path, const std::string& section) {
    // 契约 td-config §6 两级语义：
    // - 文件缺失 / section 缺失 -> 用默认值 + 修复文件（自愈，不抛异常）
    // - 内容非法（JSON 解析失败 / 类型不符）-> 抛异常，由启动方致命退出
    //   （账户连接参数是运维资产，静默回落默认值会把账户配置清空且无法与"本来就空"区分）
    if (!std::filesystem::exists(path)) {
        SPDLOG_WARN("dztd_ctp config not found, using defaults | path={}", path.string());
        TdConfig defaults;
        repair_file(defaults, path, section);
        return defaults;
    }
    nlohmann::json full;
    try {
        std::ifstream ifs(path);
        if (!ifs) {
            SPDLOG_WARN("dztd_ctp config unreadable, using defaults | path={}", path.string());
            TdConfig defaults;
            repair_file(defaults, path, section);
            return defaults;
        }
        ifs >> full;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::format("dztd_ctp config parse failed | path={} error=\"{}\"",
                                             path.string(), e.what()));
    }
    if (!full.is_object() || !full.contains(section)) {
        SPDLOG_WARN("dztd_ctp config section missing, using defaults | path={} section={}",
                    path.string(), section);
        TdConfig defaults;
        repair_file(defaults, path, section);
        return defaults;
    }
    TdConfig cfg;
    try {
        cfg = full[section].get<TdConfig>();
    } catch (const std::exception& e) {
        // 类型不符（如 accounts 不是数组、数值字段给了字符串）= 内容非法，致命退出。
        // 注意：**旧扁平格式不走这条**——未知字段按总则 §8 忽略，解码成功但账户 ext 为空，
        // 由 validate() 的 `broker_id empty` 拒绝，同样致命（td_main 对两者一视同仁）。
        throw std::runtime_error(
            std::format("dztd_ctp config invalid | path={} error=\"{}\"", path.string(), e.what()));
    }
    SPDLOG_INFO("dztd_ctp config loaded | path={} accounts={}", path.string(), cfg.accounts.size());
    return cfg;
}

void TdConfig::repair_file(const TdConfig& defaults,
                           const std::filesystem::path& path,
                           const std::string& section) {
    // 自愈写文件失败不阻断启动（§6）：内存里已有可用默认值。
    try {
        defaults.save(path, section);
        SPDLOG_INFO("dztd_ctp config repaired | path={} section={}", path.string(), section);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("dztd_ctp config repair failed | path={} error=\"{}\"", path.string(),
                     e.what());
    }
}

void TdConfig::save(const std::filesystem::path& path, const std::string& section) const {
    dztrader::core::save_json_section<TdConfig>(path, section, *this);
}

// --- validate ---

std::optional<std::string> validate(const TdConfig& cfg) {
    // account_id 非空、唯一
    std::set<std::string> seen_ids;
    for (const auto& acct : cfg.accounts) {
        if (acct.account_id.empty()) {
            return "account_id empty";
        }
        if (!seen_ids.insert(acct.account_id).second) {
            return std::format("account_id duplicate | id=\"{}\"", acct.account_id);
        }
        // Global Constraints: 账户 ext 的连接参数非空
        if (acct.ext.broker_id.empty()) {
            return std::format("broker_id empty | account_id=\"{}\"", acct.account_id);
        }
        if (acct.ext.user_id.empty()) {
            return std::format("user_id empty | account_id=\"{}\"", acct.account_id);
        }
    }

    // 排程/自动登录校验已随契约 auto-login 迁移到 AutoLoginConfig（SET/RTN_AUTO_LOGIN 帧，
    // 持久化到 auto_login section）——td section 不再含排程字段

    if (cfg.qry_account_interval_s <= 0) {
        return "qry_account_interval_s must be > 0";
    }
    if (cfg.qry_position_interval_s <= 0) {
        return "qry_position_interval_s must be > 0";
    }
    if (cfg.qry_flush_interval_ms <= 0) {
        return "qry_flush_interval_ms must be > 0";
    }

    return std::nullopt;
}

}  // namespace dztrader::ctp
