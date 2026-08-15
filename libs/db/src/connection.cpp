#include "dztrader/db/connection.h"

#include <stdexcept>

namespace dztrader::db {

Connection::Connection(const std::string& path) {
    // SQLiteCpp Database 构造: SQLITE_OPEN_READWRITE|CREATE, 默认 0 timeout
    db_ = std::make_unique<SQLite::Database>(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    // PRAGMA 配置 (dztd_ctp 数据安全优先)
    // journal_mode 用默认 DELETE (最安全), 内存库自动转 memory
    db_->exec("PRAGMA synchronous=FULL");        // 每次 commit 都 fsync
    db_->exec("PRAGMA busy_timeout=5000");       // 5s
    db_->exec("PRAGMA cache_size=-8000");        // 8MB page cache
    db_->exec("PRAGMA temp_store=MEMORY");       // 临时表在内存
}

Connection::~Connection() = default;

Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;

void Connection::exec(const std::string& sql) {
    db_->exec(sql);
}

SQLite::Transaction Connection::begin_transaction() {
    return SQLite::Transaction(*db_);
}

}  // namespace dztrader::db
