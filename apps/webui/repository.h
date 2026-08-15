#ifndef DZTRADER_WEBUI_REPOSITORY_H_
#define DZTRADER_WEBUI_REPOSITORY_H_

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <mutex>
struct sqlite3;
struct sqlite3_stmt;

namespace dztrader::webui {

/// Wrapper for sqlite3_prepare_v2 that throws std::runtime_error on failure.
void dz_prepare_v2(sqlite3* db, const char* sql, int n_byte,
                   sqlite3_stmt** stmt, const char** tail);

/// Current UTC time in ISO 8601 format (e.g. "2026-07-12T08:30:00Z").
std::string now_iso();

// Data structure definitions

struct User {
    int64_t id = 0;
    std::string username;
    std::string display_name;
    std::string email;
    std::string role;       ///< "admin" | "user"
    std::string status;     ///< "online" | "offline" | "disabled" | "locked"
    int64_t locked_until = 0;
    int failed_login_count = 0;
    std::string last_login_at;
    std::string last_login_ip;
    std::string created_at;
    std::string updated_at;
    int default_password_acknowledged = 0;  ///< 默认密码告警确认标志(0=未确认,1=已确认)
};

struct Permission {
    std::string type;       ///< "account" | "strategy"
    std::string id;
    bool granted = false;
};

struct LoginHistoryEntry {
    int64_t id = 0;
    std::string username;
    std::string ip;
    std::string device;
    bool success = false;
    std::string created_at;
    std::string user_agent;
    std::string reason;
};

struct IpEntry {
    int64_t id = 0;
    std::string ip;
    std::string reason;
    std::string source;     ///< "auto" | "manual"
    std::string created_at;
};

struct MarketSource {
    int64_t id = 0;
    std::string source_type;
    std::string source_name;
    std::string display_name;
    bool is_added = true;
    std::string created_at;
    std::string updated_at;
};

struct SecurityConfig {
    bool login_lockout_enabled = true;
    std::string access_mode;        ///< "auto" | "whitelist" | "blacklist"
    int max_failed_attempts = 5;
    int lockout_duration_sec = 900;
};

/// Password hashing (PBKDF2-SHA256, format: pbkdf2_sha256$iterations$salt_hex$hash_hex).
std::string hash_password(const std::string& password);
bool verify_password(const std::string& password, const std::string& hash);

class Repository {
public:
    explicit Repository(const std::filesystem::path& db_path);
    ~Repository();

    // User CRUD
    std::optional<User> get_user_by_id(int64_t id);
    std::optional<User> get_user_by_username(const std::string& username);
    /// Returns just the password hash for verification. nullopt if user not found.
    std::optional<std::string> get_password_hash(const std::string& username);
    struct UserListResult { std::vector<User> users; int total = 0; };
    UserListResult list_users(const std::string& search, const std::string& role_filter,
                              const std::string& status_filter, int page, int page_size);
    int64_t create_user(const std::string& username, const std::string& display_name,
                        const std::string& email, const std::string& password,
                        const std::string& role);
    bool update_user(int64_t id, const std::string& display_name,
                     const std::string& email, const std::string& role);
    bool delete_user(int64_t id);
    bool update_user_status(int64_t id, const std::string& status);
    bool reset_password(int64_t id, const std::string& new_password);
    /// 标记用户已确认默认密码告警。返回是否成功更新。
    bool ack_default_password(int64_t user_id);
    void increment_failed_login(const std::string& username);
    void reset_failed_login(const std::string& username);
    void lock_user(int64_t id, int64_t locked_until);
    void update_last_login(int64_t id, const std::string& ip);

    // Permissions
    std::vector<Permission> get_user_permissions(int64_t user_id);
    bool set_user_permission(int64_t user_id, const std::string& type,
                             const std::string& perm_id, bool granted);

    // Login history
    void add_login_history(const std::string& username, const std::string& ip,
                           const std::string& device, bool success,
                           const std::string& user_agent = "",
                           const std::string& reason = "");
    struct LoginHistoryResult { std::vector<LoginHistoryEntry> entries; int total = 0; };
    LoginHistoryResult get_login_history(int page, int page_size, int days = 1,
                                         const std::string& start = "",
                                         const std::string& end = "");

    // Security config
    SecurityConfig get_security_config();
    bool set_security_config(const SecurityConfig& config);

    // IP blacklist / whitelist
    std::vector<IpEntry> list_blacklist();
    bool add_to_blacklist(const std::string& ip, const std::string& reason, const std::string& source);
    bool remove_from_blacklist(int64_t id);
    std::vector<IpEntry> list_whitelist();
    bool add_to_whitelist(const std::string& ip, const std::string& reason);
    bool remove_from_whitelist(int64_t id);
    bool is_ip_blacklisted(const std::string& ip);
    bool is_ip_whitelisted(const std::string& ip);

    // Market sources
    std::vector<MarketSource> list_market_sources();
    std::optional<MarketSource> get_market_source(int64_t id);
    /// 按 source_name 查找 (用于启动时与 dztraderd.json reconcile)
    std::optional<MarketSource> find_market_source_by_source_name(const std::string& source_name);
    int64_t create_market_source(const std::string& source_type, const std::string& source_name,
                                 const std::string& display_name);
    bool update_market_source(int64_t id, const std::string& display_name);
    bool delete_market_source(int64_t id);

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_REPOSITORY_H_
