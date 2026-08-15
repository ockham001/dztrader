/**
 * @file path.cpp
 * @brief DZTRADER_HOME 目录布局实现
 */

#include <dztrader/core/encoding.h>
#include <dztrader/core/env.h>
#include <dztrader/core/exception.h>
#include <dztrader/core/path.h>
#include <dztrader/error.h>

#include <filesystem>
#include <format>
#include <string_view>

namespace dztrader::paths {

namespace {

constexpr std::string_view ENV_DZTRADER_HOME = "DZTRADER_HOME";

std::filesystem::path compute_home()
{
    // 1. 优先读环境变量 DZTRADER_HOME
    auto env_home = env::get(ENV_DZTRADER_HOME);
    if (env_home.has_value() && !env_home->empty()) {
        auto p = std::filesystem::path(std::move(*env_home));
        if (p.is_absolute()) {
            return p;
        }
        std::error_code ec;
        auto abs_p = std::filesystem::absolute(p, ec);
        if (ec) {
            throw Exception(DZ_EC_SYSTEM,
                std::format("path=\"{}\" err=\"{}\"", p.string(), dztrader::to_utf8_from_system(ec.message())));
        }
        return abs_p;
    }

    // 2. 回退到 OS 用户主目录下的 .dztrader
    auto os_home = env::home_dir();
    return os_home / ".dztrader";
}

void ensure_exists(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        throw Exception(DZ_EC_SYSTEM,
            std::string("path=\"") + dir.string() + "\" err=\"" + dztrader::to_utf8_from_system(ec.message()) + "\"");
    }
}

}  // namespace

const std::filesystem::path& home()
{
    static const auto CACHED_HOME = [] {
        auto p = compute_home();
        ensure_exists(p);
        return p;
    }();
    return CACHED_HOME;
}

const std::filesystem::path& configs()
{
    static const auto CACHED = [] {
        auto d = home() / "configs";
        ensure_exists(d);
        return d;
    }();
    return CACHED;
}

const std::filesystem::path& shm()
{
    static const auto CACHED = [] {
        auto d = home() / "shm";
        ensure_exists(d);
        return d;
    }();
    return CACHED;
}

const std::filesystem::path& logs()
{
    static const auto CACHED = [] {
        auto d = home() / "logs";
        ensure_exists(d);
        return d;
    }();
    return CACHED;
}

const std::filesystem::path& cache()
{
    static const auto CACHED = [] {
        auto d = home() / "cache";
        ensure_exists(d);
        return d;
    }();
    return CACHED;
}

const std::filesystem::path& strategies()
{
    static const auto CACHED = [] {
        auto d = home() / "strategies";
        ensure_exists(d);
        return d;
    }();
    return CACHED;
}

const std::filesystem::path& db()
{
    static const auto CACHED = [] {
        auto d = home() / "db";
        ensure_exists(d);
        return d;
    }();
    return CACHED;
}

}  // namespace dztrader::paths
