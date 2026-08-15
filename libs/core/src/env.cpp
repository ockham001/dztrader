/**
 * @file env.cpp
 * @brief 环境变量操作实现
 */

#ifdef _WIN32
#include <winsock2.h>
#endif

#include <dztrader/core/encoding.h>
#include <dztrader/core/env.h>
#include <dztrader/core/exception.h>
#include <dztrader/error.h>

#include <boost/process/v2/environment.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <array>
#include <pwd.h>
#include <unistd.h>
#endif

namespace dztrader::env {

namespace {

namespace bp2env = boost::process::v2::environment;
namespace bp2fs  = boost::process::v2::filesystem;

}  // namespace

std::optional<std::string> get(std::string_view key)
{
    boost::system::error_code ec;
    auto val = bp2env::get(bp2env::key(std::string(key)), ec);
    if (ec) {
        return std::nullopt;
    }
    return val.string();
}

std::string get_or(std::string_view key, std::string_view default_value)
{
    auto val = get(key);
    return val.has_value() ? std::move(*val) : std::string(default_value);
}

void set(std::string_view key, std::string_view value)
{
    boost::system::error_code ec;
    bp2env::set(bp2env::key(std::string(key)),
                bp2env::value(std::string(value)), ec);
    if (ec) {
        throw Exception(DZ_EC_SYSTEM,
            std::format("key=\"{}\" value=\"{}\" err=\"{}\"", key, value, dztrader::to_utf8_from_system(ec.message())));
    }
}

void unset(std::string_view key)
{
    boost::system::error_code ec;
    bp2env::unset(bp2env::key(std::string(key)), ec);
    if (ec) {
        throw Exception(DZ_EC_SYSTEM,
            std::format("key=\"{}\" err=\"{}\"", key, dztrader::to_utf8_from_system(ec.message())));
    }
}

std::optional<std::filesystem::path> find_executable(std::string_view name)
{
    auto result = bp2env::find_executable(
        bp2fs::path(std::string(name)), bp2env::current());
    if (result.empty()) {
        return std::nullopt;
    }
    return std::filesystem::path(result.string());
}

std::filesystem::path home_dir()
{
    boost::system::error_code ec;

#ifdef _WIN32
    // 优先 USERPROFILE（宽字符串避免代码页编码损失）
    auto user_profile = bp2env::get(bp2env::key("USERPROFILE"), ec);
    if (!ec && !user_profile.empty()) {
        auto p = std::filesystem::path(user_profile.wstring());
        return p.is_absolute() ? p : std::filesystem::absolute(p);
    }
    ec.clear();

    // 回退 HOMEDRIVE + HOMEPATH
    auto home_drive = bp2env::get(bp2env::key("HOMEDRIVE"), ec);
    if (ec) { ec.clear(); }
    auto home_path = bp2env::get(bp2env::key("HOMEPATH"), ec);
    if (!home_drive.empty() && !home_path.empty()) {
        auto p = std::filesystem::path(home_drive.wstring() + home_path.wstring());
        return p.is_absolute() ? p : std::filesystem::absolute(p);
    }
#else
    auto home = bp2env::get(bp2env::key("HOME"), ec);
    if (!ec && !home.empty()) {
        auto p = std::filesystem::path(home.string());
        return p.is_absolute() ? p : std::filesystem::absolute(p);
    }
    ec.clear();

    // getpwuid_r: 线程安全的 passwd 查询
    struct passwd pwd_buf {};
    struct passwd* pwd_result = nullptr;
    std::array<char, 4096> str_buf {};
    const int ret = getpwuid_r(getuid(), &pwd_buf, str_buf.data(),
                               str_buf.size(), &pwd_result);
    if (ret == 0 && pwd_result != nullptr
        && pwd_result->pw_dir != nullptr && pwd_result->pw_dir[0] != '\0') {
        auto p = std::filesystem::path(pwd_result->pw_dir);
        return p.is_absolute() ? p : std::filesystem::absolute(p);
    }
#endif
    throw Exception(DZ_EC_SYSTEM,
        std::string("msg=\"home directory unavailable\""));
}

}  // namespace dztrader::env
