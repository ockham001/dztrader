#include "config.h"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <fstream>
#include <random>

namespace dztrader::webui {

WebuiConfig load_webui_config(const std::filesystem::path& json_path) {
    if (!std::filesystem::exists(json_path)) {
        throw std::runtime_error("webui config not found: " + json_path.string());
    }
    std::ifstream ifs(json_path);
    if (!ifs) {
        throw std::runtime_error("cannot open webui config: " + json_path.string());
    }
    nlohmann::json data;
    try {
        ifs >> data;
    } catch (const std::exception& e) {
        throw std::runtime_error("webui config parse failed: " + json_path.string() +
                                 " error=\"" + e.what() + "\"");
    }
    if (!data.is_object()) {
        throw std::runtime_error("webui config root must be JSON object: " + json_path.string());
    }

    WebuiConfig cfg;

    if (data.contains("server") && data["server"].is_object()) {
        const auto& server = data["server"];
        cfg.server_listen = server.value("listen", std::string{"0.0.0.0"});
        cfg.server_port = static_cast<uint16_t>(server.value("port", 8080));
    }

    if (data.contains("log") && data["log"].is_object()) {
        cfg.log_level = data["log"].value("level", std::string{"info"});
    } else {
        cfg.log_level = "info";
    }

    if (data.contains("auth") && data["auth"].is_object()) {
        const auto& auth = data["auth"];
        cfg.jwt_secret = auth.value("jwt_secret", std::string{});
        cfg.token_ttl_sec = auth.value("token_ttl_sec", 3600u);
    }

    if (data.contains("admin") && data["admin"].is_object()) {
        const auto& admin = data["admin"];
        cfg.admin_username = admin.value("username", std::string{"admin"});
        // password 可选：缺失时使用默认密码（弱密码，main.cpp 会打 WARN 提醒修改）
        cfg.admin_password = admin.value("password", std::string{kDefaultAdminPassword});
        cfg.admin_password_is_default = (cfg.admin_password == kDefaultAdminPassword);
    }

    if (data.contains("notify") && data["notify"].is_object()) {
        cfg.notify_cache_size = data["notify"].value("cache_size", 100u);
    }

    return cfg;
}

namespace {

/// 生成 32 字符随机 hex 字符串(16 字节随机数据 → 32 hex)
std::string generate_random_hex_32() {
    static constexpr const char* HEX = "0123456789abcdef";
    std::random_device rd;
    std::string result;
    result.reserve(32);
    for (int i = 0; i < 16; ++i) {
        const auto byte = static_cast<unsigned char>(rd());
        result.push_back(HEX[(byte >> 4) & 0x0F]);
        result.push_back(HEX[byte & 0x0F]);
    }
    return result;
}

}  // namespace

void generate_default_config(const std::filesystem::path& json_path) {
    if (std::filesystem::exists(json_path)) {
        return;  // 已存在不覆盖
    }

    // 确保父目录存在
    std::error_code ec;
    std::filesystem::create_directories(json_path.parent_path(), ec);

    const nlohmann::json data = {
        {"server", {{"listen", "0.0.0.0"}, {"port", 8080}}},
        {"log", {{"level", "info"}, {"flush_on", "info"}}},
        {"auth", {{"jwt_secret", generate_random_hex_32()}, {"token_ttl_sec", 3600}}},
        {"admin",
         {
             {"username", "admin"}  // password 缺省: 使用默认密码 88888888, 请修改
         }},
        {"notify", {{"cache_size", 100}}}};

    std::ofstream ofs(json_path);
    if (!ofs) {
        throw std::runtime_error("无法创建配置文件: " + json_path.string());
    }
    ofs << data.dump(2);
}
}  // namespace dztrader::webui