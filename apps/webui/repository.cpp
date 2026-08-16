#include "repository.h"
#include "config.h"
#include "db_init.h"

#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace dztrader::webui {

// ---------------------------------------------------------------------------
// Password hashing (PBKDF2-SHA256)
// Format: pbkdf2_sha256$iterations$salt_hex$hash_hex
// ---------------------------------------------------------------------------

namespace {
constexpr int PBKDF2_ITERATIONS = 100000;
constexpr size_t SALT_LEN = 16;
constexpr size_t HASH_LEN = 32;  // SHA-256 output
constexpr const char* PREFIX = "pbkdf2_sha256$";
constexpr size_t PREFIX_LEN = 14;  // strlen("pbkdf2_sha256$")
}  // anonymous namespace

std::string hash_password(const std::string& password) {
    unsigned char salt[SALT_LEN];
    if (RAND_bytes(salt, SALT_LEN) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    char salt_hex[(SALT_LEN * 2) + 1];
    for (size_t i = 0; i < SALT_LEN; i++) {
        snprintf(salt_hex + (i * 2), 3, "%02x", salt[i]);
    }
    salt_hex[SALT_LEN * 2] = '\0';

    unsigned char hash[HASH_LEN];
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          salt, SALT_LEN, PBKDF2_ITERATIONS,
                          EVP_sha256(), HASH_LEN, hash) != 1) {
        throw std::runtime_error("PKCS5_PBKDF2_HMAC failed");
    }
    char hash_hex[(HASH_LEN * 2) + 1];
    for (size_t i = 0; i < HASH_LEN; i++) {
        snprintf(hash_hex + (i * 2), 3, "%02x", hash[i]);
    }
    hash_hex[HASH_LEN * 2] = '\0';

    return std::string(PREFIX) + std::to_string(PBKDF2_ITERATIONS) + "$" +
           salt_hex + "$" + hash_hex;
}

bool verify_password(const std::string& password, const std::string& hash) {
    // Format: pbkdf2_sha256$iterations$salt_hex$hash_hex
    if (hash.size() < PREFIX_LEN ||
        hash.compare(0, PREFIX_LEN, PREFIX) != 0) {
        return false;  // unsupported format
    }

    const size_t pos1 = PREFIX_LEN - 1;  // first '$'
    const size_t pos2 = hash.find('$', pos1 + 1);
    const size_t pos3 = hash.find('$', pos2 + 1);
    if (pos2 == std::string::npos || pos3 == std::string::npos) {
        return false;
    }

    int iterations = 0;
    try {
        iterations = std::stoi(hash.substr(pos1 + 1, pos2 - pos1 - 1));
    } catch (...) {
        return false;
    }
    if (iterations <= 0 || iterations > 10000000) {
        return false;  // sanity bound
    }

    const std::string salt_hex = hash.substr(pos2 + 1, pos3 - pos2 - 1);
    std::string stored_hash_hex = hash.substr(pos3 + 1);
    if (salt_hex.size() != SALT_LEN * 2 || stored_hash_hex.size() != HASH_LEN * 2) {
        return false;
    }

    // Convert salt_hex to bytes
    unsigned char salt[SALT_LEN];
    for (size_t i = 0; i < SALT_LEN; i++) {
        unsigned int val = 0;
        if (sscanf(salt_hex.c_str() + (i * 2), "%02x", &val) != 1) {
            return false;
        }
        salt[i] = static_cast<unsigned char>(val);
    }

    unsigned char computed[HASH_LEN];
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          salt, SALT_LEN, iterations,
                          EVP_sha256(), HASH_LEN, computed) != 1) {
        return false;
    }

    char computed_hex[(HASH_LEN * 2) + 1];
    for (size_t i = 0; i < HASH_LEN; i++) {
        snprintf(computed_hex + (i * 2), 3, "%02x", computed[i]);
    }
    computed_hex[HASH_LEN * 2] = '\0';

    // Constant-time comparison (CRYPTO_memcmp)
    return CRYPTO_memcmp(computed_hex, stored_hash_hex.data(), HASH_LEN * 2) == 0;
}

// ---------------------------------------------------------------------------
// Helper: extract a text column, returning empty string for NULL
// ---------------------------------------------------------------------------

namespace {

std::string col_text(sqlite3_stmt* stmt, int col) {
    const auto* ptr = sqlite3_column_text(stmt, col);
    if (ptr) {
        return reinterpret_cast<const char*>(ptr);
    }
    return {};
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Free functions declared in repository.h
// ---------------------------------------------------------------------------

void dz_prepare_v2(sqlite3* db, const char* sql, int n_byte,
                   sqlite3_stmt** stmt, const char** tail) {
    const int rc = sqlite3_prepare_v2(db, sql, n_byte, stmt, tail);
    if (rc != SQLITE_OK) {
        const std::string err = sqlite3_errmsg(db);
        if (*stmt) {
            sqlite3_finalize(*stmt);
            *stmt = nullptr;
        }
        throw std::runtime_error("sqlite3_prepare_v2 failed: " + err +
                                 " (sql: " + std::string(sql) + ")");
    }
}

std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

// ---------------------------------------------------------------------------
// Repository
// ---------------------------------------------------------------------------

Repository::Repository(const std::filesystem::path& db_path) {
    const int rc = sqlite3_open(db_path.string().c_str(), &db_);
    if (rc != SQLITE_OK) {
        const std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        throw std::runtime_error("failed to open database: " + err);
    }
    run_schema(db_);
}

Repository::~Repository() {
    if (db_) {
        sqlite3_close(db_);
    }
}

// ---------------------------------------------------------------------------
// User CRUD
// ---------------------------------------------------------------------------

std::optional<User> Repository::get_user_by_id(int64_t id) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, username, display_name, email, role, status, "
                      "locked_until, failed_login_count, last_login_at, last_login_ip, "
                      "created_at, updated_at, default_password_acknowledged FROM users WHERE id = ?";
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);

    std::optional<User> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        User u;
        u.id = sqlite3_column_int64(stmt, 0);
        u.username = col_text(stmt, 1);
        u.display_name = col_text(stmt, 2);
        u.email = col_text(stmt, 3);
        u.role = col_text(stmt, 4);
        u.status = col_text(stmt, 5);
        u.locked_until = sqlite3_column_int64(stmt, 6);
        u.failed_login_count = sqlite3_column_int(stmt, 7);
        u.last_login_at = col_text(stmt, 8);
        u.last_login_ip = col_text(stmt, 9);
        u.created_at = col_text(stmt, 10);
        u.updated_at = col_text(stmt, 11);
        u.default_password_acknowledged = sqlite3_column_int(stmt, 12);
        result = u;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<User> Repository::get_user_by_username(const std::string& username) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, username, display_name, email, role, status, "
                      "locked_until, failed_login_count, last_login_at, last_login_ip, "
                      "created_at, updated_at, default_password_acknowledged FROM users WHERE username = ?";
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<User> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        User u;
        u.id = sqlite3_column_int64(stmt, 0);
        u.username = col_text(stmt, 1);
        u.display_name = col_text(stmt, 2);
        u.email = col_text(stmt, 3);
        u.role = col_text(stmt, 4);
        u.status = col_text(stmt, 5);
        u.locked_until = sqlite3_column_int64(stmt, 6);
        u.failed_login_count = sqlite3_column_int(stmt, 7);
        u.last_login_at = col_text(stmt, 8);
        u.last_login_ip = col_text(stmt, 9);
        u.created_at = col_text(stmt, 10);
        u.updated_at = col_text(stmt, 11);
        u.default_password_acknowledged = sqlite3_column_int(stmt, 12);
        result = u;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<std::string> Repository::get_password_hash(const std::string& username) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT password_hash FROM users WHERE username = ?";
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* h = sqlite3_column_text(stmt, 0);
        if (h) {
            result = std::string(reinterpret_cast<const char*>(h));
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

Repository::UserListResult Repository::list_users(const std::string& search,
                                                   const std::string& role_filter,
                                                   const std::string& status_filter,
                                                   int page, int page_size) {
    std::scoped_lock lk(mutex_);

    std::string where = " WHERE 1=1";
    if (!search.empty()) {
        where += " AND (username LIKE ? OR display_name LIKE ?)";
    }
    if (!role_filter.empty()) {
        where += " AND role = ?";
    }
    if (!status_filter.empty()) {
        where += " AND status = ?";
    }

    // Count query
    const std::string count_sql = "SELECT COUNT(*) FROM users" + where;
    sqlite3_stmt* count_stmt = nullptr;
    dz_prepare_v2(db_, count_sql.c_str(), -1, &count_stmt, nullptr);
    int idx = 1;
    if (!search.empty()) {
        const std::string pattern = "%" + search + "%";
        sqlite3_bind_text(count_stmt, idx++, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(count_stmt, idx++, pattern.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!role_filter.empty()) {
        sqlite3_bind_text(count_stmt, idx++, role_filter.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!status_filter.empty()) {
        sqlite3_bind_text(count_stmt, idx++, status_filter.c_str(), -1, SQLITE_TRANSIENT);
    }

    int total = 0;
    if (sqlite3_step(count_stmt) == SQLITE_ROW) {
        total = sqlite3_column_int(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);

    // Data query
    const std::string sql =
        "SELECT id, username, display_name, email, role, status, "
        "locked_until, failed_login_count, last_login_at, last_login_ip, "
        "created_at, updated_at FROM users" +
        where + " ORDER BY id LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    idx = 1;
    if (!search.empty()) {
        const std::string pattern = "%" + search + "%";
        sqlite3_bind_text(stmt, idx++, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, idx++, pattern.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!role_filter.empty()) {
        sqlite3_bind_text(stmt, idx++, role_filter.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!status_filter.empty()) {
        sqlite3_bind_text(stmt, idx++, status_filter.c_str(), -1, SQLITE_TRANSIENT);
    }
    int offset = (page - 1) * page_size;
    offset = (std::max)(offset, 0);
    sqlite3_bind_int(stmt, idx++, page_size);
    sqlite3_bind_int(stmt, idx++, offset);

    UserListResult result;
    result.total = total;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        User u;
        u.id = sqlite3_column_int64(stmt, 0);
        u.username = col_text(stmt, 1);
        u.display_name = col_text(stmt, 2);
        u.email = col_text(stmt, 3);
        u.role = col_text(stmt, 4);
        u.status = col_text(stmt, 5);
        u.locked_until = sqlite3_column_int64(stmt, 6);
        u.failed_login_count = sqlite3_column_int(stmt, 7);
        u.last_login_at = col_text(stmt, 8);
        u.last_login_ip = col_text(stmt, 9);
        u.created_at = col_text(stmt, 10);
        u.updated_at = col_text(stmt, 11);
        result.users.push_back(u);
    }
    sqlite3_finalize(stmt);
    return result;
}

int64_t Repository::create_user(const std::string& username, const std::string& display_name,
                                const std::string& email, const std::string& password,
                                const std::string& role) {
    std::scoped_lock lk(mutex_);
    const std::string password_hash = hash_password(password);
    const std::string now = now_iso();

    const char* sql = "INSERT INTO users (username, display_name, email, password_hash, "
                      "role, status, created_at, updated_at) VALUES (?, ?, ?, ?, ?, 'offline', ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, display_name.c_str(), -1, SQLITE_TRANSIENT);
    if (email.empty()) {
        sqlite3_bind_null(stmt, 3);
    } else {
        sqlite3_bind_text(stmt, 3, email.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, 4, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int64_t const id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);
    return id;
}

bool Repository::update_user(int64_t id, const std::string& display_name,
                             const std::string& email, const std::string& role) {
    std::scoped_lock lk(mutex_);
    const std::string now = now_iso();
    const char* sql = "UPDATE users SET display_name = ?, email = ?, role = ?, updated_at = ? "
                      "WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, display_name.c_str(), -1, SQLITE_TRANSIENT);
    if (email.empty()) {
        sqlite3_bind_null(stmt, 2);
    } else {
        sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, id);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

bool Repository::delete_user(int64_t id) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, "DELETE FROM users WHERE id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

bool Repository::update_user_status(int64_t id, const std::string& status) {
    std::scoped_lock lk(mutex_);
    const std::string now = now_iso();
    const char* sql = "UPDATE users SET status = ?, updated_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, id);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

bool Repository::reset_password(int64_t id, const std::string& new_password) {
    std::scoped_lock lk(mutex_);
    const std::string password_hash = hash_password(new_password);
    const std::string now = now_iso();
    const char* sql = "UPDATE users SET password_hash = ?, updated_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, id);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    // 改回默认密码时重置确认标志(会再次告警);改为强密码时不重置(避免误报)
    if (changes > 0 && new_password == kDefaultAdminPassword) {
        sqlite3_stmt* reset_stmt = nullptr;
        dz_prepare_v2(db_, "UPDATE users SET default_password_acknowledged = 0 WHERE id = ?",
                      -1, &reset_stmt, nullptr);
        sqlite3_bind_int64(reset_stmt, 1, id);
        sqlite3_step(reset_stmt);
        sqlite3_finalize(reset_stmt);
    }
    return changes > 0;
}

bool Repository::ack_default_password(int64_t user_id) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, "UPDATE users SET default_password_acknowledged = 1 WHERE id = ?",
                  -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

void Repository::increment_failed_login(const std::string& username) {
    std::scoped_lock lk(mutex_);
    const char* sql = "UPDATE users SET failed_login_count = failed_login_count + 1 WHERE username = ?";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Repository::reset_failed_login(const std::string& username) {
    std::scoped_lock lk(mutex_);
    const char* sql = "UPDATE users SET failed_login_count = 0 WHERE username = ?";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Repository::lock_user(int64_t id, int64_t locked_until) {
    std::scoped_lock lk(mutex_);
    const std::string now = now_iso();
    const char* sql = "UPDATE users SET status = 'locked', locked_until = ?, updated_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, locked_until);
    sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Repository::update_last_login(int64_t id, const std::string& ip) {
    std::scoped_lock lk(mutex_);
    const std::string now = now_iso();
    const char* sql = "UPDATE users SET last_login_at = ?, last_login_ip = ?, status = 'online', "
                      "updated_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ---------------------------------------------------------------------------
// Permissions
// ---------------------------------------------------------------------------

std::vector<Permission> Repository::get_user_permissions(int64_t user_id) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT permission_type, permission_id, granted FROM user_permissions "
                      "WHERE user_id = ? ORDER BY permission_type, permission_id";
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, user_id);

    std::vector<Permission> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Permission p;
        p.type = col_text(stmt, 0);
        p.id = col_text(stmt, 1);
        p.granted = sqlite3_column_int(stmt, 2) != 0;
        result.push_back(p);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Repository::set_user_permission(int64_t user_id, const std::string& type,
                                     const std::string& perm_id, bool granted) {
    std::scoped_lock lk(mutex_);
    const char* sql = "INSERT INTO user_permissions (user_id, permission_type, permission_id, granted) "
                      "VALUES (?, ?, ?, ?) "
                      "ON CONFLICT(user_id, permission_type, permission_id) DO UPDATE SET granted = excluded.granted";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, perm_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, granted ? 1 : 0);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return true;
}

// ---------------------------------------------------------------------------
// Login history
// ---------------------------------------------------------------------------

void Repository::add_login_history(const std::string& username, const std::string& ip,
                                   const std::string& device, bool success,
                                   const std::string& user_agent,
                                   const std::string& reason) {
    std::scoped_lock lk(mutex_);
    const std::string now = now_iso();
    const char* sql = "INSERT INTO login_history (username, ip, device, success, created_at, user_agent, reason) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ip.c_str(), -1, SQLITE_TRANSIENT);
    if (device.empty()) {
        sqlite3_bind_null(stmt, 3);
    } else {
        sqlite3_bind_text(stmt, 3, device.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, 4, success ? 1 : 0);
    sqlite3_bind_text(stmt, 5, now.c_str(), -1, SQLITE_TRANSIENT);
    if (user_agent.empty()) {
        sqlite3_bind_null(stmt, 6);
    } else {
        sqlite3_bind_text(stmt, 6, user_agent.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (reason.empty()) {
        sqlite3_bind_null(stmt, 7);
    } else {
        sqlite3_bind_text(stmt, 7, reason.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

Repository::LoginHistoryResult Repository::get_login_history(int page, int page_size,
                                                             int days,
                                                             const std::string& start,
                                                             const std::string& end) {
    std::scoped_lock lk(mutex_);

    // Build the WHERE clause. When explicit start/end ISO timestamps are
    // provided they take precedence over the days-based filter; otherwise we
    // fall back to "last N days" using SQLite's datetime() modifier.
    std::string where;
    const bool use_range = !start.empty() || !end.empty();
    if (use_range) {
        where = "WHERE ";
        std::vector<std::string> conds;
        if (!start.empty()) {
            conds.emplace_back("created_at >= ?");
        }
        if (!end.empty()) {
            conds.emplace_back("created_at <= ?");
        }
        for (size_t i = 0; i < conds.size(); i++) {
            if (i > 0) {
                where += " AND ";
            }
            where += conds[i];
        }
    } else {
        const int safe_days = days > 0 ? days : 1;
        where = "WHERE created_at >= datetime('now', '-" + std::to_string(safe_days) + " days')";
    }

    // Count
    const std::string count_sql = "SELECT COUNT(*) FROM login_history " + where;
    sqlite3_stmt* count_stmt = nullptr;
    dz_prepare_v2(db_, count_sql.c_str(), -1, &count_stmt, nullptr);
    int bind_idx = 1;
    if (use_range && !start.empty()) {
        sqlite3_bind_text(count_stmt, bind_idx++, start.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (use_range && !end.empty()) {
        sqlite3_bind_text(count_stmt, bind_idx++, end.c_str(), -1, SQLITE_TRANSIENT);
    }
    int total = 0;
    if (sqlite3_step(count_stmt) == SQLITE_ROW) {
        total = sqlite3_column_int(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);

    // Data
    int offset = (page - 1) * page_size;
    offset = (std::max)(offset, 0);
    const std::string sql =
        "SELECT id, username, ip, device, success, created_at, user_agent, reason "
        "FROM login_history " +
        where + " ORDER BY id DESC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    bind_idx = 1;
    if (use_range && !start.empty()) {
        sqlite3_bind_text(stmt, bind_idx++, start.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (use_range && !end.empty()) {
        sqlite3_bind_text(stmt, bind_idx++, end.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, bind_idx++, page_size);
    sqlite3_bind_int(stmt, bind_idx++, offset);

    LoginHistoryResult result;
    result.total = total;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        LoginHistoryEntry e;
        e.id = sqlite3_column_int64(stmt, 0);
        e.username = col_text(stmt, 1);
        e.ip = col_text(stmt, 2);
        e.device = col_text(stmt, 3);
        e.success = sqlite3_column_int(stmt, 4) != 0;
        e.created_at = col_text(stmt, 5);
        e.user_agent = col_text(stmt, 6);
        e.reason = col_text(stmt, 7);
        result.entries.push_back(e);
    }
    sqlite3_finalize(stmt);
    return result;
}

// ---------------------------------------------------------------------------
// Security config
// ---------------------------------------------------------------------------

SecurityConfig Repository::get_security_config() {
    std::scoped_lock lk(mutex_);
    SecurityConfig config;
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, "SELECT value FROM security_config WHERE key = ?", -1, &stmt, nullptr);

    auto get_value = [&](const std::string& key) -> std::string {
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        std::string val;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            val = col_text(stmt, 0);
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        return val;
    };

    config.login_lockout_enabled = get_value("login_lockout_enabled") == "true";
    config.access_mode = get_value("access_mode");
    const std::string max_attempts = get_value("max_failed_attempts");
    config.max_failed_attempts = max_attempts.empty() ? 5 : std::stoi(max_attempts);
    const std::string lockout_dur = get_value("lockout_duration_sec");
    config.lockout_duration_sec = lockout_dur.empty() ? 900 : std::stoi(lockout_dur);

    sqlite3_finalize(stmt);
    return config;
}

bool Repository::set_security_config(const SecurityConfig& config) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, "INSERT OR REPLACE INTO security_config (key, value) VALUES (?, ?)",
                       -1, &stmt, nullptr);

    auto set_value = [&](const std::string& key, const std::string& value) {
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    };

    set_value("login_lockout_enabled", config.login_lockout_enabled ? "true" : "false");
    set_value("access_mode", config.access_mode);
    set_value("max_failed_attempts", std::to_string(config.max_failed_attempts));
    set_value("lockout_duration_sec", std::to_string(config.lockout_duration_sec));

    sqlite3_finalize(stmt);
    return true;
}

// ---------------------------------------------------------------------------
// IP blacklist / whitelist
// ---------------------------------------------------------------------------

std::vector<IpEntry> Repository::list_blacklist() {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, ip, reason, source, created_at FROM ip_blacklist ORDER BY id";
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);

    std::vector<IpEntry> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        IpEntry e;
        e.id = sqlite3_column_int64(stmt, 0);
        e.ip = col_text(stmt, 1);
        e.reason = col_text(stmt, 2);
        e.source = col_text(stmt, 3);
        e.created_at = col_text(stmt, 4);
        result.push_back(e);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Repository::add_to_blacklist(const std::string& ip, const std::string& reason,
                                  const std::string& source) {
    std::scoped_lock lk(mutex_);
    const std::string now = now_iso();
    const char* sql = "INSERT OR IGNORE INTO ip_blacklist (ip, reason, source, created_at) "
                      "VALUES (?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    if (reason.empty()) {
        sqlite3_bind_null(stmt, 2);
    } else {
        sqlite3_bind_text(stmt, 2, reason.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, 3, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

bool Repository::remove_from_blacklist(int64_t id) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, "DELETE FROM ip_blacklist WHERE id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

std::vector<IpEntry> Repository::list_whitelist() {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, ip, reason, '' AS source, created_at FROM ip_whitelist ORDER BY id";
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);

    std::vector<IpEntry> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        IpEntry e;
        e.id = sqlite3_column_int64(stmt, 0);
        e.ip = col_text(stmt, 1);
        e.reason = col_text(stmt, 2);
        e.created_at = col_text(stmt, 4);
        result.push_back(e);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Repository::add_to_whitelist(const std::string& ip, const std::string& reason) {
    std::scoped_lock lk(mutex_);
    const std::string now = now_iso();
    const char* sql = "INSERT OR IGNORE INTO ip_whitelist (ip, reason, created_at) VALUES (?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    if (reason.empty()) {
        sqlite3_bind_null(stmt, 2);
    } else {
        sqlite3_bind_text(stmt, 2, reason.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, 3, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

bool Repository::remove_from_whitelist(int64_t id) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, "DELETE FROM ip_whitelist WHERE id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

bool Repository::is_ip_blacklisted(const std::string& ip) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, "SELECT 1 FROM ip_blacklist WHERE ip = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    const bool result = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return result;
}

bool Repository::is_ip_whitelisted(const std::string& ip) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, "SELECT 1 FROM ip_whitelist WHERE ip = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    const bool result = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return result;
}

// ---------------------------------------------------------------------------
// Market sources
// ---------------------------------------------------------------------------

std::vector<MarketSource> Repository::list_market_sources() {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, source_type, source_name, display_name, is_added, "
                      "created_at, updated_at FROM market_sources ORDER BY id";
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);

    std::vector<MarketSource> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MarketSource s;
        s.id = sqlite3_column_int64(stmt, 0);
        s.source_type = col_text(stmt, 1);
        s.source_name = col_text(stmt, 2);
        s.display_name = col_text(stmt, 3);
        s.is_added = sqlite3_column_int(stmt, 4) != 0;
        s.created_at = col_text(stmt, 5);
        s.updated_at = col_text(stmt, 6);
        result.push_back(s);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<MarketSource> Repository::get_market_source(int64_t id) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, source_type, source_name, display_name, is_added, "
                      "created_at, updated_at FROM market_sources WHERE id = ?";
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);

    std::optional<MarketSource> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        MarketSource s;
        s.id = sqlite3_column_int64(stmt, 0);
        s.source_type = col_text(stmt, 1);
        s.source_name = col_text(stmt, 2);
        s.display_name = col_text(stmt, 3);
        s.is_added = sqlite3_column_int(stmt, 4) != 0;
        s.created_at = col_text(stmt, 5);
        s.updated_at = col_text(stmt, 6);
        result = s;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<MarketSource> Repository::find_market_source_by_source_name(
    const std::string& source_name) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, source_type, source_name, display_name, is_added, "
                      "created_at, updated_at FROM market_sources WHERE source_name = ?";
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, source_name.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<MarketSource> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        MarketSource s;
        s.id = sqlite3_column_int64(stmt, 0);
        s.source_type = col_text(stmt, 1);
        s.source_name = col_text(stmt, 2);
        s.display_name = col_text(stmt, 3);
        s.is_added = sqlite3_column_int(stmt, 4) != 0;
        s.created_at = col_text(stmt, 5);
        s.updated_at = col_text(stmt, 6);
        result = s;
    }
    sqlite3_finalize(stmt);
    return result;
}

int64_t Repository::create_market_source(const std::string& source_type,
                                         const std::string& source_name,
                                         const std::string& display_name) {
    std::scoped_lock lk(mutex_);
    const std::string now = now_iso();
    // 幂等创建/复用: source_name 已存在（UNIQUE 冲突，如 remove 后再次添加）时复位 is_added=1
    // 并刷新 updated_at；display_name 保留 DB 现值不覆盖（用户自定义名不因再添加丢失——
    // available 预填进程名，覆盖会静默丢名）
    const char* sql = "INSERT INTO market_sources (source_type, source_name, display_name, "
                      "is_added, created_at, updated_at) "
                      "VALUES (?, ?, ?, 1, ?, ?) "
                      "ON CONFLICT(source_name) DO UPDATE SET "
                      "is_added = 1, updated_at = excluded.updated_at";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, source_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, source_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // DO UPDATE 路径不产生新 rowid (last_insert_rowid 为陈旧值),
    // 统一按 source_name 回查 id (幂等复用行)
    int64_t id = 0;
    const char* sel = "SELECT id FROM market_sources WHERE source_name = ?";
    dz_prepare_v2(db_, sel, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, source_name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}

bool Repository::update_market_source(int64_t id, const std::string& display_name) {
    std::scoped_lock lk(mutex_);
    const std::string now = now_iso();
    const char* sql = "UPDATE market_sources SET display_name = ?, updated_at = ? "
                      "WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, id);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

bool Repository::set_market_source_added(int64_t id, bool added) {
    std::scoped_lock lk(mutex_);
    const char* sql = "UPDATE market_sources SET is_added = ?, updated_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, added ? 1 : 0);
    sqlite3_bind_text(stmt, 2, now_iso().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, id);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

bool Repository::delete_market_source(int64_t id) {
    std::scoped_lock lk(mutex_);
    sqlite3_stmt* stmt = nullptr;
    dz_prepare_v2(db_, "DELETE FROM market_sources WHERE id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

}  // namespace dztrader::webui
