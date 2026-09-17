#ifndef DZTRADER_DB_SQLITE_H_
#define DZTRADER_DB_SQLITE_H_

// 驱动私有 wrapper: 统一 SQLiteCpp 引入方式（公开面不暴露 SQLiteCpp）
// 项目通过 Conan 引入 SQLiteCpp (find_package(SQLiteCpp CONFIG REQUIRED))
// libs/db/src 内代码 #include "sqlite.h" 而非直接 #include <SQLiteCpp/Database.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>

#endif  // DZTRADER_DB_SQLITE_H_
