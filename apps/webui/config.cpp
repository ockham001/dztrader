#include "config.h"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <cctype>
#include <fstream>
#include <random>
#include <string_view>

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
        throw std::runtime_error("webui config parse failed: " + json_path.string() + " error=\"" +
                                 e.what() + "\"");
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

/// IPv4 点分十进制字面量是否落在 127.0.0.0/8（严格四段，每段 0..255，不允许多余字符）
bool is_ipv4_loopback(std::string_view addr) {
    int part = 0;
    std::size_t i = 0;
    while (i <= addr.size()) {
        const std::size_t dot = addr.find('.', i);
        const std::string_view seg = addr.substr(i, dot == std::string_view::npos ? dot : dot - i);
        if (seg.empty() || seg.size() > 3) {
            return false;
        }
        int value = 0;
        for (const char c : seg) {
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + (c - '0');
        }
        if (value > 255) {
            return false;
        }
        if (part == 0 && value != 127) {
            return false;  // 只看 127.0.0.0/8
        }
        ++part;
        if (dot == std::string_view::npos) {
            break;
        }
        i = dot + 1;
    }
    return part == 4;
}

}  // namespace

bool is_loopback_listen(const std::string& listen) {
    // 首尾空白与大小写容忍：配置文件手写 " 127.0.0.1 " / "LocalHost" 仍应识别
    std::string_view addr(listen);
    while (!addr.empty() && (addr.front() == ' ' || addr.front() == '\t')) {
        addr.remove_prefix(1);
    }
    while (!addr.empty() && (addr.back() == ' ' || addr.back() == '\t')) {
        addr.remove_suffix(1);
    }
    std::string lowered(addr);
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    addr = lowered;

    // IPv6 的方括号写法语义上仍是回环地址，按回环处理
    // （注意：drogon 的 addListener 只接受裸 IP 字面量，"[::1]" 会在启动期 FATAL 退出，
    //  故配置文件应写 "::1"；本判定只回答「语义上是否仅本机可达」，不承诺 drogon 可接受）
    if (addr.size() > 2 && addr.front() == '[' && addr.back() == ']') {
        addr = addr.substr(1, addr.size() - 2);
    }
    // 注：drogon 拒绝主机名（含 localhost，启动期 FATAL），但它在语义上确实只解析到回环，
    // 故仍判为回环；配置文件应写 IP 字面量。
    if (addr == "localhost") {
        return true;
    }
    if (addr == "::1" || addr == "0:0:0:0:0:0:0:1") {
        return true;
    }
    return is_ipv4_loopback(addr);
}

void generate_default_config(const std::filesystem::path& json_path) {
    if (std::filesystem::exists(json_path)) {
        return;  // 已存在不覆盖
    }

    // 确保父目录存在
    std::error_code ec;
    std::filesystem::create_directories(json_path.parent_path(), ec);

    // JSON 无注释语法：server 段的说明以 `_comment` 键承载（加载器忽略未知键）。
    // 文案只讲风险与修改方式，不改默认值——默认 0.0.0.0 是维护者的决定（#201）。
    const nlohmann::json data = {
        {"server",
         {{"listen", "0.0.0.0"},
          {"port", 8080},
          {"_comment",
           "listen 默认为 0.0.0.0（全部网卡，局域网可访问）且无 TLS：管理面与口令、token 均为"
           "明文传输。仅在本机使用请改为 127.0.0.1；跨机访问请配合内网隔离、强口令或反向代理 "
           "TLS。listen 只接受 IP 字面量（写主机名如 localhost 会导致启动失败），且仅配置文件"
           "可改，重启生效。"}}},
        {"log", {{"level", "info"}, {"flush_on", "info"}}},
        {"auth", {{"jwt_secret", generate_random_hex_32()}, {"token_ttl_sec", 3600}}},
        {"admin",
         {
             {"username", "admin"}  // password 缺省: 使用默认密码 88888888, 请修改
         }},
        {"notify", {{"cache_size", 100}}}};

    std::ofstream ofs(json_path);
    if (!ofs) {
        throw std::runtime_error("create config file failed | path=\"" + json_path.string() + "\"");
    }
    ofs << data.dump(2);
}
}  // namespace dztrader::webui