#include "td/td_config.h"

#include <cstddef>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>

#include <spdlog/spdlog.h>

#include <dztrader/core/json_section.h>
#include <dztrader/core/utf8.h>

namespace dztrader::ctp {

namespace {

/// CTP `TThostFtdcInvestorIDType[13]` 的可用长度：12 字节 + NUL。超长必须报错而不是
/// 静默截断——截断会把订单打到不存在的账户（ADR 0056 / 契约 td-config §2.3）。
constexpr std::size_t kCtpInvestorIdMaxBytes = 12;
/// 契约 td-config §2.2：`account_name` trim 后 ≤64 字节 UTF-8。
constexpr std::size_t kAccountNameMaxBytes = 64;
/// 契约 td-config §2.2 / §7：`account_id` = ASCII 可见字符 `0x21`–`0x7E`，禁 `:`。
constexpr unsigned char kAccountIdMinByte = 0x21;
constexpr unsigned char kAccountIdMaxByte = 0x7E;
/// ASCII 空白（契约 §2.2「trim 后」的口径）；控制字符另行拒绝（§7）。
constexpr std::string_view kAsciiWhitespace = " \t\r\n\f\v";

std::string_view trim_ascii_whitespace(std::string_view text) {
    const auto first = text.find_first_not_of(kAsciiWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(kAsciiWhitespace);
    return text.substr(first, last - first + 1);
}

bool is_valid_account_id_charset(std::string_view account_id) {
    for (const char c : account_id) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < kAccountIdMinByte || byte > kAccountIdMaxByte || c == ':') {
            return false;
        }
    }
    return true;
}

/// 控制字符 = ASCII < 0x20（含 `\t\r\n`）、DEL 0x7F 与 C1 控制段 U+0080–U+009F。
/// C1 段在 UTF-8 里只能是 `0xC2` + `0x80`–`0x9F` 两字节序列。
bool contains_control_chars(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x20 || byte == 0x7F) {
            return true;
        }
        if (byte == 0xC2 && i + 1 < text.size()) {
            const auto next = static_cast<unsigned char>(text[i + 1]);
            if (next >= 0x80 && next <= 0x9F) {
                return true;
            }
        }
    }
    return false;
}

/// `account_name` 校验结果：成功给唯一性比较键，失败给原因（错误串形态见契约 §7）。
struct AccountNameValidation {
    std::optional<std::string> key{};  ///< 成功 = NFC(trim(name))；留空时回落 account_id
    std::string error{};               ///< key 为空时的原因（超长 / 控制字符 / 非 UTF-8）
};

/// 校验 `account_name` 并生成唯一性比较键（契约 §2.2 / ADR 0056 决策 4）。
AccountNameValidation validate_account_name(const AccountConfig& acct) {
    const std::string_view trimmed = trim_ascii_whitespace(acct.account_name);
    if (trimmed.empty()) {
        return {.key = acct.account_id};  // 留空按 account_id 参与唯一性判定
    }
    if (trimmed.size() > kAccountNameMaxBytes) {
        return {.error = std::format("account_name too long | account_id=\"{}\" max_bytes={} "
                                     "actual_bytes={}",
                                     acct.account_id, kAccountNameMaxBytes, trimmed.size())};
    }
    if (contains_control_chars(trimmed)) {
        return {.error = std::format("account_name invalid | account_id=\"{}\" reason=control_char",
                                     acct.account_id)};
    }
    auto normalized = utf8_nfc(trimmed);
    if (!normalized.has_value()) {
        return {.error = std::format("account_name invalid | account_id=\"{}\" reason=invalid_utf8",
                                     acct.account_id)};
    }
    return {.key = std::move(*normalized)};
}

/// 检测旧配置里已删除的 `ext.user_id`（ADR 0056 硬切）：返回首个命中账户的 account_id。
std::optional<std::string> find_removed_user_id(const nlohmann::json& td_json) {
    if (!td_json.is_object()) {
        return std::nullopt;
    }
    const auto accounts = td_json.find("accounts");
    if (accounts == td_json.end() || !accounts->is_array()) {
        return std::nullopt;
    }
    for (const auto& acct : *accounts) {
        if (!acct.is_object()) {
            continue;
        }
        const auto ext = acct.find("ext");
        if (ext != acct.end() && ext->is_object() && ext->contains("user_id")) {
            return acct.value("account_id", std::string{});
        }
    }
    return std::nullopt;
}

}  // namespace

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
    if (const auto legacy_account = find_removed_user_id(full[section]);
        legacy_account.has_value()) {
        // ADR 0056 硬切：`ext.user_id` 已删除（CTP 的 UserID / InvestorID 都取 account_id）。
        // 按契约 §6「内容非法」致命退出——未知字段本会按总则 §8 静默忽略，但那样旧账户会以
        // 自造 id 启动、登录必失败且无从区分「本来就没配」；删掉 td section 可自愈重生成。
        throw std::runtime_error(
            std::format("dztd_ctp config invalid | path={} reason=\"removed field ext.user_id\" "
                        "account_id=\"{}\"",
                        path.string(), *legacy_account));
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
    // 每个账户的唯一性比较键（NFC(trim(account_name))；留空回落 account_id）
    std::vector<std::string> name_keys;
    name_keys.reserve(cfg.accounts.size());

    for (const auto& acct : cfg.accounts) {
        // account_id = 柜台真实投资者代码（ADR 0056）：非空、CTP 侧 ≤12 字节、ASCII 可见
        // 字符且禁 `:`（契约 td-config §2.2 / §7）；超长必须报错，禁止静默截断。
        if (acct.account_id.empty()) {
            return "account_id empty";
        }
        if (acct.account_id.size() > kCtpInvestorIdMaxBytes) {
            return std::format(
                "account_id too long | account_id=\"{}\" max_bytes={} actual_bytes={}",
                acct.account_id, kCtpInvestorIdMaxBytes, acct.account_id.size());
        }
        if (!is_valid_account_id_charset(acct.account_id)) {
            return std::format("account_id charset invalid | account_id=\"{}\"", acct.account_id);
        }
        auto name = validate_account_name(acct);
        if (!name.key.has_value()) {
            return name.error;
        }
        name_keys.push_back(std::move(*name.key));

        // Global Constraints: 账户 ext 的连接参数非空
        if (acct.ext.broker_id.empty()) {
            return std::format("broker_id empty | account_id=\"{}\"", acct.account_id);
        }
    }

    // 契约 td-config §5 / §7：account_id 本配置内唯一；account_name 与 account_id 展平判定
    // ——任意两个账户的 id / name 两两不等（同一账户允许 name == 自己的 id，§2.2 不做写入
    // 时规范化，因此「未设置」与「设置成和 id 一样」是两件事）。
    for (std::size_t i = 0; i < cfg.accounts.size(); ++i) {
        for (std::size_t j = i + 1; j < cfg.accounts.size(); ++j) {
            const auto& id_i = cfg.accounts[i].account_id;
            const auto& id_j = cfg.accounts[j].account_id;
            if (id_i == id_j) {
                return std::format("account_id duplicate | id=\"{}\"", id_i);
            }
            if (name_keys[i] == id_j || id_i == name_keys[j] || name_keys[i] == name_keys[j]) {
                return std::format(
                    "account_name conflict | account_id=\"{}\" other_account_id=\"{}\"", id_i,
                    id_j);
            }
        }
    }

    // 排程/自动登录校验已随契约 auto-login 迁移到 AutoLoginConfig（SET/RTN_AUTO_LOGIN 帧，
    // 持久化到 auto_login section）——td section 不再含排程字段

    if (cfg.qry_min_interval_ms <= 0) {
        return "qry_min_interval_ms must be > 0";
    }

    return std::nullopt;
}

}  // namespace dztrader::ctp
