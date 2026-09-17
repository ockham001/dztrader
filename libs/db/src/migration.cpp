#include "dztrader/db/legacy/migration.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <vector>

namespace dztrader::db::legacy {

void MigrationManager::add(int version, MigrationFn fn) {
    if (version < 1) {
        throw std::invalid_argument(std::format("migration version must be >= 1 | version={}", version));
    }
    if (!migrations_.emplace(version, std::move(fn)).second) {
        throw std::invalid_argument(std::format("migration version already registered | version={}", version));
    }
}

std::vector<int> MigrationManager::apply(SQLite::Database& db) {
    // 单一 IMMEDIATE 事务: 版本复查 + 迁移 + 记录 原子提交;
    // 多进程并发首开时由 SQLite 写锁串行化 (busy_timeout 吸收等待).
    SQLite::Transaction txn(db, SQLite::TransactionBehavior::IMMEDIATE);

    // 建表 (IF NOT EXISTS, 幂等; 随事务一并提交/回滚)
    db.exec(
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "    version INTEGER PRIMARY KEY,"
        "    applied_at TEXT NOT NULL"
        ")");
    db.exec("CREATE INDEX IF NOT EXISTS idx_schema_version ON schema_version(version)");

    // 事务内重读已应用版本: 并发的另一进程可能已抢先应用部分迁移
    std::vector<int> applied_versions;
    {
        SQLite::Statement q(db, "SELECT version FROM schema_version ORDER BY version");
        while (q.executeStep()) {
            applied_versions.push_back(q.getColumn(0).getInt());
        }
    }

    // 应用全部未应用迁移 (按 version 升序); 任一失败由 txn 析构整体回滚
    std::vector<int> newly_applied;
    for (const auto& [version, fn] : migrations_) {
        // 跳过已应用
        if (std::find(applied_versions.begin(), applied_versions.end(), version) != applied_versions.end()) {
            continue;
        }

        fn(db);
        // 记录到 schema_version (ISO 8601 UTC 时间戳)
        auto now = std::chrono::system_clock::now();
        auto ts = std::format("{:%FT%TZ}", std::chrono::zoned_time{
            std::chrono::locate_zone("UTC"), now});
        SQLite::Statement ins(db,
            "INSERT INTO schema_version (version, applied_at) VALUES (?, ?)");
        ins.bind(1, version);
        ins.bind(2, ts);
        ins.exec();
        newly_applied.push_back(version);
    }

    txn.commit();
    return newly_applied;
}

}  // namespace dztrader::db::legacy
