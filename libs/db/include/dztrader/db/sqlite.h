#ifndef DZTRADER_DB_SQLITE_H_
#define DZTRADER_DB_SQLITE_H_

// wrapper header: 统一 SQLiteCpp 引入方式
// 项目通过 Conan 引入 SQLiteCpp (find_package(SQLiteCpp CONFIG REQUIRED))
// 所有 dzdb 代码应 #include <dztrader/db/sqlite.h> 而非直接 #include <SQLiteCpp/Database.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>

#endif  // DZTRADER_DB_SQLITE_H_
