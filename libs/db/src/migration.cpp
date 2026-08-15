#include "dztrader/db/migration.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <vector>

namespace dztrader::db {

void MigrationManager::add(int version, MigrationFn fn) {
    if (version < 1) {
        throw std::invalid_argument(std::format("migration version must be >= 1 | version={}", version));
    }
    if (!migrations_.emplace(version, std::move(fn)).second) {
        throw std::invalid_argument(std::format("migration version already registered | version={}", version));
    }
}

std::vector<int> MigrationManager::apply(SQLite::Database& db) {
    // 1. 创建 schema_version 表 (IF NOT EXISTS, 幂等)
    db.exec(
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "    version INTEGER PRIMARY KEY,"
        "    applied_at TEXT NOT NULL"
        ")");
    db.exec("CREATE INDEX IF NOT EXISTS idx_schema_version ON schema_version(version)");

    // 2. 查询已应用的版本
    std::vector<int> applied_versions;
    {
        SQLite::Statement q(db, "SELECT version FROM schema_version ORDER BY version");
        while (q.executeStep()) {
            applied_versions.push_back(q.getColumn(0).getInt());
        }
    }

    // 3. 应用未注册的迁移 (按 version 升序)
    std::vector<int> newly_applied;
    for (const auto& [version, fn] : migrations_) {
        // 跳过已应用
        if (std::find(applied_versions.begin(), applied_versions.end(), version) != applied_versions.end()) {
            continue;
        }

        // 每个迁移在独立事务中执行
        SQLite::Transaction txn(db);
        try {
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
            txn.commit();
        } catch (const std::exception&) {
            throw;
        }
        newly_applied.push_back(version);
    }

    return newly_applied;
}

}  // namespace dztrader::db
