/**
 * @file driver_registry.h
 * @brief 驱动注册缝：backend 名 -> 工厂（内嵌驱动 / 将来的动态库插件 / 代理适配器共用入口）
 *
 * ADR 0016 决策 1：Database::open 只做"懒注册 + 查表"，未注册 backend 抛
 * DZ_EC_DB_UNSUPPORTED_BACKEND；内置驱动由 builtin_drivers.cpp 显式注册，
 * 不使用全局对象静态初始化（避免链接器丢弃与初始化顺序问题）。
 *
 * 本缝是**进程内**驱动入口：注册/查找都发生在同一进程的静态库内。将来的动态库插件
 * （dlopen）不能直接复用本接口——跨 so 边界需要另加 C ABI 层（abi_version / size /
 * destroy 等握手），本轮不做（ADR 0016 决策 4 只留缝）。
 */
#ifndef DZTRADER_DB_SRC_DRIVER_REGISTRY_H_
#define DZTRADER_DB_SRC_DRIVER_REGISTRY_H_

#include <memory>
#include <span>
#include <string_view>

#include <dztrader/db/database.h>

namespace dztrader::db::internal {

/// 驱动工厂：backend 名在查找时已确定，工厂只负责按 config 建库
using DriverFactory = std::unique_ptr<Database> (*)(const Config& config,
                                                    std::span<const ResourceSchema> schemas);

/// 注册驱动；backend 为空或重名抛 Exception(DZ_EC_INVALID_PARAM)
void register_driver(std::string_view backend, DriverFactory factory);

/// 是否已注册（不打开连接；供诊断与将来插件加载失败降级判断）
[[nodiscard]] bool has_driver(std::string_view backend) noexcept;

/// 懒注册内置驱动（std::once_flag；sqlite 恒有，mysql 由 DZ_WITH_MYSQL 决定）
void ensure_builtin_drivers_registered();

/// 查表建库；未注册抛 Exception(DZ_EC_DB_UNSUPPORTED_BACKEND, "unsupported backend: backend={}")
[[nodiscard]] std::unique_ptr<Database> create_database(const Config& config,
                                                        std::span<const ResourceSchema> schemas);

}  // namespace dztrader::db::internal

#endif  // DZTRADER_DB_SRC_DRIVER_REGISTRY_H_
