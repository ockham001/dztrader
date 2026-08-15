#include "db_init.h"
#include "repository.h"  // for hash_password

#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace dztrader::webui {

namespace {

constexpr const char* SCHEMA_SQL = R"(
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT UNIQUE NOT NULL,
    display_name TEXT NOT NULL,
    email TEXT,
    password_hash TEXT NOT NULL,
    role TEXT NOT NULL DEFAULT 'user',
    status TEXT NOT NULL DEFAULT 'offline',
    locked_until INTEGER,
    failed_login_count INTEGER DEFAULT 0,
    last_login_at TEXT,
    last_login_ip TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    default_password_acknowledged INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS user_permissions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    permission_type TEXT NOT NULL,
    permission_id TEXT NOT NULL,
    granted INTEGER NOT NULL DEFAULT 0,
    UNIQUE(user_id, permission_type, permission_id)
);

CREATE TABLE IF NOT EXISTS login_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL,
    ip TEXT NOT NULL,
    device TEXT,
    success INTEGER NOT NULL,
    created_at TEXT NOT NULL,
    user_agent TEXT,
    reason TEXT
);

CREATE TABLE IF NOT EXISTS security_config (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS ip_blacklist (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ip TEXT NOT NULL UNIQUE,
    reason TEXT,
    source TEXT NOT NULL DEFAULT 'manual',
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS ip_whitelist (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ip TEXT NOT NULL UNIQUE,
    reason TEXT,
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS market_sources (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_type TEXT NOT NULL,
    source_name TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL,
    is_added INTEGER NOT NULL DEFAULT 1,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
)";

constexpr const char* SEED_CONFIG_SQL = R"(
INSERT OR IGNORE INTO security_config (key, value) VALUES
    ('login_lockout_enabled', 'true'),
    ('access_mode', 'auto'),
    ('max_failed_attempts', '5'),
    ('lockout_duration_sec', '900');
)";

}  // anonymous namespace

void run_schema(sqlite3* db) {
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, SCHEMA_SQL, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        const std::string err = err_msg ? err_msg : "unknown";
        sqlite3_free(err_msg);
        throw std::runtime_error("failed to create schema: " + err);
    }
    rc = sqlite3_exec(db, SEED_CONFIG_SQL, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        const std::string err = err_msg ? err_msg : "unknown";
        sqlite3_free(err_msg);
        throw std::runtime_error("failed to seed config: " + err);
    }

    // Migration: add user_agent and reason columns to login_history for existing databases
    // where CREATE TABLE IF NOT EXISTS would not have added the new columns.
    {
        sqlite3_stmt* stmt = nullptr;
        dz_prepare_v2(db, "PRAGMA table_info(login_history)", -1, &stmt, nullptr);
        bool has_user_agent = false;
        bool has_reason = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(stmt, 1);
            if (name) {
                const std::string col_name(reinterpret_cast<const char*>(name));
                if (col_name == "user_agent") {
                    has_user_agent = true;
                }
                if (col_name == "reason") {
                    has_reason = true;
                }
            }
        }
        sqlite3_finalize(stmt);
        if (!has_user_agent) {
            rc = sqlite3_exec(db, "ALTER TABLE login_history ADD COLUMN user_agent TEXT;",
                              nullptr, nullptr, &err_msg);
            if (rc != SQLITE_OK) {
                const std::string err = err_msg ? err_msg : "unknown";
                sqlite3_free(err_msg);
                throw std::runtime_error("failed to add user_agent column: " + err);
            }
        }
        if (!has_reason) {
            rc = sqlite3_exec(db, "ALTER TABLE login_history ADD COLUMN reason TEXT;",
                              nullptr, nullptr, &err_msg);
            if (rc != SQLITE_OK) {
                const std::string err = err_msg ? err_msg : "unknown";
                sqlite3_free(err_msg);
                throw std::runtime_error("failed to add reason column: " + err);
            }
        }
    }

    // Migration: add default_password_acknowledged column to users for existing databases
    {
        sqlite3_stmt* stmt = nullptr;
        dz_prepare_v2(db, "PRAGMA table_info(users)", -1, &stmt, nullptr);
        bool has_default_password_ack = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(stmt, 1);
            if (name) {
                const std::string col_name(reinterpret_cast<const char*>(name));
                if (col_name == "default_password_acknowledged") {
                    has_default_password_ack = true;
                    break;
                }
            }
        }
        sqlite3_finalize(stmt);
        if (!has_default_password_ack) {
            rc = sqlite3_exec(db,
                "ALTER TABLE users ADD COLUMN default_password_acknowledged INTEGER NOT NULL DEFAULT 0;",
                nullptr, nullptr, &err_msg);
            if (rc != SQLITE_OK) {
                const std::string err = err_msg ? err_msg : "unknown";
                sqlite3_free(err_msg);
                throw std::runtime_error("failed to add default_password_acknowledged column: " + err);
            }
        }
    }

    // Migration: drop legacy broker/credential tables (Wave 2C).
    // 经纪商列表 + 当前选中 + 凭证 现在由子进程配置文件存储, 通过 SHM 镜像同步, 不再使用 DB.
    // 现有 DB 行将被遗弃, 用户需通过新 UI 重新输入经纪商信息.
    {
        const char* drop_statements[] = {
            "DROP TABLE IF EXISTS broker_frontends;",
            "DROP TABLE IF EXISTS brokers;",
            "DROP TABLE IF EXISTS market_source_credentials;",
        };
        for (const char* sql : drop_statements) {
            rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
            if (rc != SQLITE_OK) {
                const std::string err = err_msg ? err_msg : "unknown";
                sqlite3_free(err_msg);
                throw std::runtime_error(std::string("failed to drop legacy table: ") + err);
            }
        }
        spdlog::info("legacy broker/credential tables dropped if present");
    }

    // Migration: drop legacy market_source_schedules table and market_sources.auto_login column.
    // schedules 和 auto_login 现在由 dzmd_ctp.json 存储, 通过 SHM 镜像同步, 不再使用 DB.
    // SQLite 3.35+ 支持 ALTER TABLE DROP COLUMN; 旧版本降级为重建表.
    {
        // 1. 删除 market_source_schedules 表 (若存在)
        rc = sqlite3_exec(db, "DROP TABLE IF EXISTS market_source_schedules;",
                          nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            const std::string err = err_msg ? err_msg : "unknown";
            sqlite3_free(err_msg);
            throw std::runtime_error("failed to drop market_source_schedules: " + err);
        }

        // 2. 删除 market_sources.auto_login 列 (若存在)
        sqlite3_stmt* stmt = nullptr;
        dz_prepare_v2(db, "PRAGMA table_info(market_sources)", -1, &stmt, nullptr);
        bool has_auto_login_col = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(stmt, 1);
            if (name) {
                const std::string col_name(reinterpret_cast<const char*>(name));
                if (col_name == "auto_login") {
                    has_auto_login_col = true;
                    break;
                }
            }
        }
        sqlite3_finalize(stmt);
        if (has_auto_login_col) {
            rc = sqlite3_exec(db, "ALTER TABLE market_sources DROP COLUMN auto_login;",
                              nullptr, nullptr, &err_msg);
            if (rc != SQLITE_OK) {
                std::string err = err_msg ? err_msg : "unknown";
                sqlite3_free(err_msg);
                spdlog::warn("failed to drop auto_login column, ignoring | error={}", err);
            } else {
                spdlog::info("legacy column dropped | table=market_sources column=auto_login");
            }
        }
    }
}

void init_database(const std::filesystem::path& db_path, sqlite3*& out_db) {
    const int rc = sqlite3_open(db_path.string().c_str(), &out_db);
    if (rc != SQLITE_OK) {
        const std::string err = sqlite3_errmsg(out_db);
        sqlite3_close(out_db);
        out_db = nullptr;
        throw std::runtime_error("failed to open database: " + err);
    }
    run_schema(out_db);
    spdlog::info("database initialized | path={}", db_path.string());
}

void seed_admin_user(sqlite3* db, const std::string& username, const std::string& password) {
    // 检查是否已存在 admin 角色用户（而非仅查 users 表总行数）。
    // 这样即使有人绕过应用层删了 admin 但保留普通用户，重启也能恢复 admin。
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db, "SELECT COUNT(*) FROM users WHERE role = 'admin'", -1, &stmt, nullptr);
    sqlite3_step(stmt);
    const int admin_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (admin_count > 0) {
        return;
    }

    // 如果 admin 用户名已被普通用户占用（罕见但需防御），改为不创建并告警
    dz_prepare_v2(db, "SELECT COUNT(*) FROM users WHERE username = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    const int name_taken = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (name_taken > 0) {
        spdlog::warn("cannot seed admin | username={} existing_role=non-admin", username);
        return;
    }

    const std::string password_hash = hash_password(password);
    const std::string now = now_iso();

    const char* sql = "INSERT INTO users (username, display_name, password_hash, role, status, "
                      "created_at, updated_at) VALUES (?, ?, ?, 'admin', 'offline', ?, ?)";
    dz_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    spdlog::info("admin user seeded | username={}", username);
}

}  // namespace dztrader::webui
