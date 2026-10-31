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
void dz_prepare_v2(
    sqlite3* db, const char* sql, int n_byte, sqlite3_stmt** stmt, const char** tail);

/// Current UTC time in ISO 8601 format (e.g. "2026-07-12T08:30:00Z").
std::string now_iso();

// Data structure definitions

struct User {
    int64_t id = 0;
    std::string username;
    std::string display_name;
    std::string email;
    std::string role;    ///< "admin" | "user"
    std::string status;  ///< "online" | "offline" | "disabled" | "locked"
    int64_t locked_until = 0;
    int failed_login_count = 0;
    std::string last_login_at;
    std::string last_login_ip;
    std::string created_at;
    std::string updated_at;
    int default_password_acknowledged = 0;  ///< 默认密码告警确认标志(0=未确认,1=已确认)
    /// JWT 吊销版本号（契约 rest §1「会话吊销」）：签发时写入 token 的 `ver` claim，
    /// 改密/重置/禁用时自增，使既有 token 与 WS 连接立即失效
    int64_t token_version = 0;
    /// 是否必须下次登录改密（0=否,1=是）：管理员重置口令后置位，
    /// 自助改密成功后清除；置位期间除改密与 /api/user/me 外全部拒绝
    int must_change_password = 0;
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
    std::string source;  ///< "auto" | "manual"
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
    std::string access_mode;  ///< "auto" | "whitelist" | "blacklist"
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
    struct UserListResult {
        std::vector<User> users;
        int total = 0;
    };
    UserListResult list_users(const std::string& search,
                              const std::string& role_filter,
                              const std::string& status_filter,
                              int page,
                              int page_size);
    /// 创建用户并返回新用户 id。
    /// @throws std::runtime_error 插入失败（如 username 唯一约束冲突）
    int64_t create_user(const std::string& username,
                        const std::string& display_name,
                        const std::string& email,
                        const std::string& password,
                        const std::string& role);
    bool update_user(int64_t id,
                     const std::string& display_name,
                     const std::string& email,
                     const std::string& role);
    bool delete_user(int64_t id);
    bool update_user_status(int64_t id, const std::string& status);
    /// 管理员重置口令（契约 rest §2.2）：改 hash + 自增 token_version（吊销既有会话）
    /// + 置 must_change_password=1（下次登录强制改密）。不需旧密码（admin 已鉴权）。
    bool admin_reset_password(int64_t id, const std::string& new_password);
    /// 自助改密（契约 rest §2.2）：改 hash + 自增 token_version（含当前会话失效）
    /// + 清除 must_change_password。不改 default_password_acknowledged。
    bool change_own_password(int64_t id, const std::string& new_password);
    /// 自增 token_version 以吊销该用户全部既有 JWT/WS 会话（禁用账户时调用）。
    /// 用户不存在返回 false。
    bool revoke_user_tokens(int64_t id);
    /// 该用户当前口令是否仍等于给定明文（用于「是否仍在使用系统默认口令」判定：
    /// 配置里的 admin_password_is_default 只说明**配置缺省**，用户改密后它仍是 true，
    /// 不能用来判断当前口令，否则默认口令告警会在改密后继续谎报）。
    /// 用户不存在或读 hash 失败返回 false。
    bool password_matches(const std::string& username, const std::string& plain);
    /// 若该用户当前口令**仍是** `default_password`，置 must_change_password=1
    /// （#202 决策：保留系统默认口令，但首次登录强制改密）。已改过口令则无操作。
    /// @return true=需要/已处于强制改密；false=口令非默认值或用户不存在
    bool force_password_change_if_default(int64_t id, const std::string& default_password);
    /// 标记用户已确认默认密码告警。返回是否成功更新。
    bool ack_default_password(int64_t user_id);
    void increment_failed_login(const std::string& username);
    void reset_failed_login(const std::string& username);
    void lock_user(int64_t id, int64_t locked_until);
    void update_last_login(int64_t id, const std::string& ip);

    // Login history
    void add_login_history(const std::string& username,
                           const std::string& ip,
                           const std::string& device,
                           bool success,
                           const std::string& user_agent = "",
                           const std::string& reason = "");
    struct LoginHistoryResult {
        std::vector<LoginHistoryEntry> entries;
        int total = 0;
    };
    LoginHistoryResult get_login_history(int page,
                                         int page_size,
                                         int days = 1,
                                         const std::string& start = "",
                                         const std::string& end = "");

    // Security config
    SecurityConfig get_security_config();
    bool set_security_config(const SecurityConfig& config);

    // IP blacklist / whitelist
    std::vector<IpEntry> list_blacklist();
    bool add_to_blacklist(const std::string& ip,
                          const std::string& reason,
                          const std::string& source);
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
    int64_t create_market_source(const std::string& source_type,
                                 const std::string& source_name,
                                 const std::string& display_name);
    bool update_market_source(int64_t id, const std::string& display_name);
    /// 标记行情源生命周期（契约 rest: remove 下发成功置 false，再次添加复位 true）
    bool set_market_source_added(int64_t id, bool added);
    bool delete_market_source(int64_t id);

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_REPOSITORY_H_
