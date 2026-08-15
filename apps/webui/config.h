#ifndef DZTRADER_WEBUI_CONFIG_H_
#define DZTRADER_WEBUI_CONFIG_H_

#include <filesystem>
#include <string>
#include <cstdint>

namespace dztrader::webui {

/// 默认 admin 密码（配置文件未显式指定时使用）。
/// 弱密码，部署后应立即修改。
constexpr const char* kDefaultAdminPassword = "88888888";

struct WebuiConfig {
    std::string server_listen = "0.0.0.0";
    uint16_t server_port = 8080;
    std::string log_level = "info";
    std::string jwt_secret;
    uint32_t token_ttl_sec = 3600;
    std::string admin_username = "admin";
    std::string admin_password = kDefaultAdminPassword;
    /// 是否使用了默认密码（配置文件未指定时为 true）
    bool admin_password_is_default = false;
    /// NOTIFY_UI 缓存条数，0 = 不缓存
    size_t notify_cache_size = 100;
};

/// 加载 webui.json 配置。文件不存在或解析失败抛 std::runtime_error。
WebuiConfig load_webui_config(const std::filesystem::path& json_path);

/// 生成默认 webui.json 配置文件(jwt_secret 随机生成)。
/// 文件已存在时不覆盖。失败抛 std::runtime_error。
void generate_default_config(const std::filesystem::path& json_path);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_CONFIG_H_
