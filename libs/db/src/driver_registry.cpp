/**
 * @file driver_registry.cpp
 * @brief 驱动注册缝实现（ADR 0016 决策 1）+ Database::open 查表入口
 *
 * 注册表用 std::mutex 保护：注册一次性、查找低频，性能无关。
 * 注册表状态为函数内 static（懒初始化），不用全局对象静态初始化，
 * 避免静态库对象被链接器丢弃（--gc-sections / 未引用 TU）。
 */
#include "driver_registry.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>

namespace dztrader::db::internal {

namespace {

struct Registry {
    std::mutex mutex;
    std::map<std::string, DriverFactory, std::less<>> drivers;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

}  // namespace

void register_driver(std::string_view backend, DriverFactory factory) {
    if (backend.empty()) {
        throw Exception(DZ_EC_INVALID_PARAM, "driver backend name must not be empty");
    }
    if (factory == nullptr) {
        throw Exception(DZ_EC_INVALID_PARAM, "driver factory must not be null | backend={}",
                        backend);
    }
    Registry& reg = registry();
    const std::lock_guard<std::mutex> lock(reg.mutex);
    const auto inserted = reg.drivers.emplace(std::string(backend), factory);
    if (!inserted.second) {
        throw Exception(DZ_EC_INVALID_PARAM, "driver already registered | backend={}", backend);
    }
}

bool has_driver(std::string_view backend) noexcept {
    // 纯查表：不触发懒注册（内置驱动由 ensure_builtin_drivers_registered 显式注册）
    Registry& reg = registry();
    const std::lock_guard<std::mutex> lock(reg.mutex);
    return reg.drivers.find(backend) != reg.drivers.end();
}

std::unique_ptr<Database> create_database(const Config& config,
                                          std::span<const ResourceSchema> schemas) {
    DriverFactory factory = nullptr;
    {
        Registry& reg = registry();
        const std::lock_guard<std::mutex> lock(reg.mutex);
        const auto it = reg.drivers.find(config.backend);
        if (it != reg.drivers.end()) {
            factory = it->second;
        }
    }
    if (factory == nullptr) {
        // 文案与错误码是既有测试断言依赖的现状，不因注册缝引入而改变
        throw Exception(DZ_EC_DB_UNSUPPORTED_BACKEND, "unsupported backend: backend={}",
                        config.backend);
    }
    return factory(config, schemas);  // 不持锁调用工厂：工厂内可能抛错/做 IO
}

}  // namespace dztrader::db::internal

namespace dztrader::db {

std::unique_ptr<Database> Database::open(const Config& config,
                                         std::span<const ResourceSchema> schemas) {
    // ADR 0016 §4：先懒注册内置驱动，再查表建库；sqlite_driver.cpp 只提供 open_sqlite 工厂
    internal::ensure_builtin_drivers_registered();
    return internal::create_database(config, schemas);
}

}  // namespace dztrader::db
