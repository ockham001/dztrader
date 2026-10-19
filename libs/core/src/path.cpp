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
#include <mutex>
#include <optional>
#include <string_view>

namespace dztrader::paths {

namespace {

constexpr std::string_view ENV_DZTRADER_HOME = "DZTRADER_HOME";

std::filesystem::path compute_home() {
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
            throw Exception(DZ_EC_SYSTEM, std::format("path=\"{}\" err=\"{}\"", p.string(),
                                                      dztrader::to_utf8_from_system(ec.message())));
        }
        return abs_p;
    }

    // 2. 回退到 OS 用户主目录下的 .dztrader
    auto os_home = env::home_dir();
    return os_home / ".dztrader";
}

void ensure_exists(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        throw Exception(DZ_EC_SYSTEM, std::string("path=\"") + dir.string() + "\" err=\"" +
                                          dztrader::to_utf8_from_system(ec.message()) + "\"");
    }
}

/// 路径缓存。所有访问均在 g_cache_mutex 保护下进行。
struct PathCache {
    std::optional<std::filesystem::path> home;
    std::optional<std::filesystem::path> configs;
    std::optional<std::filesystem::path> shm;
    std::optional<std::filesystem::path> logs;
    std::optional<std::filesystem::path> cache;
    std::optional<std::filesystem::path> strategies;
    std::optional<std::filesystem::path> db;
    std::optional<std::filesystem::path> td_db;
};

std::mutex g_cache_mutex;
PathCache g_cache;

/// 取缓存的 home。调用者必须持有 g_cache_mutex。
const std::filesystem::path& home_locked() {
    if (!g_cache.home) {
        auto p = compute_home();
        ensure_exists(p);
        g_cache.home = std::move(p);
    }
    return *g_cache.home;
}

/// 取缓存的子目录。调用者必须持有 g_cache_mutex。
const std::filesystem::path& subdir_locked(std::optional<std::filesystem::path>& slot,
                                           const char* name) {
    if (!slot) {
        auto d = home_locked() / name;
        ensure_exists(d);
        slot = std::move(d);
    }
    return *slot;
}

}  // namespace

const std::filesystem::path& home() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    return home_locked();
}

const std::filesystem::path& configs() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    return subdir_locked(g_cache.configs, "configs");
}

const std::filesystem::path& shm() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    return subdir_locked(g_cache.shm, "shm");
}

const std::filesystem::path& logs() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    return subdir_locked(g_cache.logs, "logs");
}

const std::filesystem::path& cache() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    return subdir_locked(g_cache.cache, "cache");
}

const std::filesystem::path& strategies() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    return subdir_locked(g_cache.strategies, "strategies");
}

const std::filesystem::path& db() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    return subdir_locked(g_cache.db, "db");
}

const std::filesystem::path& td_db() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (!g_cache.td_db) {
        // db() 已保证目录存在, 此处不重复 ensure
        g_cache.td_db = subdir_locked(g_cache.db, "db") / "td.db";
    }
    return *g_cache.td_db;
}

void reset_for_test() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache = PathCache{};
}

}  // namespace dztrader::paths
