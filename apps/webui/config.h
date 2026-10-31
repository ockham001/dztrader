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
    /// 监听地址。默认 "0.0.0.0"（全部网卡，局域网使用），main.cpp 对非回环地址
    /// 打启动 WARN；仅配置文件可改，运行期不可热改。
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

/// 判断监听地址是否只对回环可达（#201：管理面无 TLS，非回环即在网络上明文可达）。
///
/// 判定对象是**语义上是否仅本机可达**（不是「drogon 是否接受该写法」）：IPv4 回环段
/// `127.0.0.0/8` 字面量、IPv6 `::1`（含 `0:0:0:0:0:0:0:1`，允许 `[...]` 形式），
/// 外加 `localhost` 主机名特例（大小写/首尾空白不敏感）。
/// 其余一律返回 false——通配地址（`0.0.0.0` / `::`）、本机其他网卡地址、其他主机名、
/// 空串，以及任何无法解析为上述形态的字符串（含带端口的写法）。
/// 方向刻意保守：误判为「暴露」只多一条启动告警，误判为「回环」会漏报管理面暴露。
///
/// 注意：drogon 的 `addListener` 只接受裸 IP 字面量，主机名与 `[...]` 写法会在启动期
/// FATAL 退出；本函数不做「该值能否启动」的保证（那是配置校验的事，不在本票范围）。
bool is_loopback_listen(const std::string& listen);

/// 加载 webui.json 配置。文件不存在或解析失败抛 std::runtime_error。
WebuiConfig load_webui_config(const std::filesystem::path& json_path);

/// 生成默认 webui.json 配置文件(jwt_secret 随机生成)。
/// 文件已存在时不覆盖。失败抛 std::runtime_error。
/// server 段带 `_comment` 键说明暴露面（JSON 无注释语法；加载器忽略未知键）。
void generate_default_config(const std::filesystem::path& json_path);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_CONFIG_H_
