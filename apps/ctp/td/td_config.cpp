#include "td/td_config.h"

#include <format>
#include <set>

#include <spdlog/spdlog.h>

#include <dztrader/core/json_section.h>

namespace dztrader::ctp {

// --- TdConfig JSON 序列化 ---
// to_json / from_json 由 NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT 宏生成 (头文件内)
// to_safe_json 需手写: password 脱敏

nlohmann::json TdConfig::to_safe_json() const {
    nlohmann::json j = *this;  // 调用宏生成的 to_json
    // 脱敏: accounts 数组中每个 account 的 broker.password 替换为 "****"
    if (j.contains("accounts") && j["accounts"].is_array()) {
        for (auto& acct : j["accounts"]) {
            if (acct.contains("broker") && acct["broker"].contains("password")) {
                acct["broker"]["password"] = "****";
            }
        }
    }
    return j;
}

// --- 文件持久化 ---

TdConfig TdConfig::load(const std::filesystem::path& path, const std::string& section) {
    if (!std::filesystem::exists(path)) {
        SPDLOG_WARN("dztd_ctp config not found, using defaults | path={}", path.string());
        return TdConfig{};
    }
    try {
        auto cfg = dztrader::core::load_json_section<TdConfig>(path, section);
        SPDLOG_INFO("dztd_ctp config loaded | path={} accounts={}",
                    path.string(), cfg.accounts.size());
        return cfg;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::format("dztd_ctp config parse failed | path={} error=\"{}\"",
                                             path.string(), e.what()));
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
        // Global Constraints: broker_id / user_id 非空
        if (acct.broker.broker_id.empty()) {
            return std::format("broker_id empty | account_id=\"{}\"", acct.account_id);
        }
        if (acct.broker.user_id.empty()) {
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
