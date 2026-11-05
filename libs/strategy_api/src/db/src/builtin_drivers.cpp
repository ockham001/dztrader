/**
 * @file builtin_drivers.cpp
 * @brief 内置驱动显式注册（sqlite 恒有；mysql 由 DZ_WITH_MYSQL 决定）
 *
 * ADR 0016 决策 1：不使用全局对象静态初始化（避免静态库对象被链接器丢弃与初始化顺序问题），
 * 由 Database::open 经 std::once_flag 懒注册；mysql 未编入（DZ_WITH_MYSQL=OFF）时该 backend
 * 即为未注册，Database::open 抛 DZ_EC_DB_UNSUPPORTED_BACKEND。
 */
#include <mutex>

#include "driver_registry.h"
#include "sqlite_driver.h"

#if defined(DZ_WITH_MYSQL)
#include "mysql_driver.h"
#endif

namespace dztrader::db::internal {

void ensure_builtin_drivers_registered() {
    // std::call_once：并发首调时只有一个线程执行注册体，其余线程阻塞至其完成，不会重复注册；
    // 注册体只注册唯一名（sqlite/mysql）且工厂非空。调用方若先用同名（sqlite/mysql）注册，
    // 属编程错误：register_driver 响亮抛 DZ_EC_INVALID_PARAM，不静默覆盖。
    static std::once_flag registered;
    std::call_once(registered, [] {
        register_driver("sqlite", &open_sqlite);
#if defined(DZ_WITH_MYSQL)
        register_driver("mysql", &open_mysql);
#endif
    });
}

}  // namespace dztrader::db::internal
