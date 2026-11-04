#include <dztrader/tdstore/trade_db.h>

#include <chrono>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Exception.h>
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>

#include <dztrader/tdstore/schema_catalog.h>

namespace dztrader::tdstore {

namespace {

using dztrader::tdstore::TradeDbInitReport;

// SQLite 主 result code（取 sqlite3.h 的稳定编号）：SQLiteCpp 的 conan 包不在公共 include
// 暴露 sqlite3.h（其头文件只用前置声明），故在此显式声明。probe 分类用：
// 只有 kSqliteCorrupt / kSqliteNotADatabase 判定库损坏重建；kSqliteBusy / kSqliteLocked（并发
// 读写锁竞争）及其它（如 SQLITE_CANTOPEN=14 打开失败、IOERR、权限）一律按可重试处理。
constexpr int kSqliteBusy = 5;
constexpr int kSqliteLocked = 6;
constexpr int kSqliteCorrupt = 11;
constexpr int kSqliteNotADatabase = 26;

/// 本机时间戳（备份文件名用；master 启动期单线程，无并发要求）
std::string timestamp_now() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return std::format("{:04d}{:02d}{:02d}-{:02d}{:02d}{:02d}", local.tm_year + 1900,
                       local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

std::string error_text(const std::filesystem::path& path,
                       std::string_view action,
                       const std::string& error) {
    return std::format("trade db {} | path={} error=\"{}\"", action, path.string(), error);
}

void ensure_parent_dir(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    if (ec) {
        throw std::runtime_error(error_text(path, "parent dir create failed", ec.message()));
    }
}

/// 连接级 PRAGMA（建库/自愈连接与只读查询连接共用；WAL 仅写连接需要）
void apply_write_pragmas(SQLite::Database& db) {
    db.exec("PRAGMA busy_timeout=5000");
    // 交易数据抗断电（ADR 0041 A4 保留 WAL）：低频批量写入 + 多读者只读。
    db.exec("PRAGMA synchronous=FULL");
    const std::string mode = db.execAndGet("PRAGMA journal_mode=WAL").getString();
    if (mode != "wal") {
        throw std::runtime_error(std::format("trade db wal conversion failed | mode={}", mode));
    }
}

/// 按声明建表/建索引并写版本（幂等；调用方负责 PRAGMA 与事务边界）
void apply_schema_ddl(SQLite::Database& db) {
    db.exec(build_schema_ddl());
    db.exec(std::format("PRAGMA user_version={}", kTdSchemaVersion));
}

void create_new(const std::filesystem::path& path) {
    ensure_parent_dir(path);
    try {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        apply_write_pragmas(db);
        SQLite::Transaction txn(db);
        apply_schema_ddl(db);
        txn.commit();
    } catch (const SQLite::Exception& e) {
        throw std::runtime_error(error_text(path, "create failed", e.what()));
    } catch (const std::exception& e) {
        throw std::runtime_error(error_text(path, "create failed", e.what()));
    }
}

/// 版本一致的既有库：幂等 DDL 自愈（补缺失表/索引）并校正 user_version。
void ensure_existing(const std::filesystem::path& path) {
    try {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE);
        apply_write_pragmas(db);
        SQLite::Transaction txn(db);
        apply_schema_ddl(db);
        txn.commit();
    } catch (const SQLite::Exception& e) {
        throw std::runtime_error(error_text(path, "schema ensure failed", e.what()));
    } catch (const std::exception& e) {
        throw std::runtime_error(error_text(path, "schema ensure failed", e.what()));
    }
}

/// 探测失败按主 result code 分类：只有明确损坏才判重建，其余一律按可重试抛出。
/// busy/locked = 并发读写锁竞争；cantopen/ioerr/权限等 = 打开或读取失败。
/// 两类都不得改名（健康库可能只是暂时不可用，必须交给调用方退避重连）。
[[noreturn]] void throw_retryable_probe_error(const std::filesystem::path& path,
                                              int sqlite_result_code,
                                              std::string_view error) {
    const bool lock_contention =
        sqlite_result_code == kSqliteBusy || sqlite_result_code == kSqliteLocked;
    throw std::runtime_error(
        std::format("trade db probe {} retryable | path={} code={} error=\"{}\"",
                    lock_contention ? "busy or locked" : "open failed", path.string(),
                    sqlite_result_code, error));
}

/// 探测既有库版本。返回 false = 明确损坏（SQLITE_CORRUPT / SQLITE_NOTADB，按损坏库
/// 改名重建）；返回 true 时写 version。
/// 其余失败（busy/locked/cantopen/ioerr 等）不判定损坏：抛 std::runtime_error
/// （可重试，含 code，调用方退避重连），不得把健康库改名。
bool probe_existing(const std::filesystem::path& path, int& version) {
    try {
        SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
        // 并发写者持锁时先等 5s 再判定（避免把暂时不可读的健康库当损坏）
        db.exec("PRAGMA busy_timeout=5000");
        version = db.execAndGet("PRAGMA user_version").getInt();
        // 只碰 schema 页的最轻查询：能成功说明库结构可读（不是损坏/截断/非 SQLite）。
        (void)db.execAndGet("SELECT COUNT(*) FROM sqlite_master").getInt();
        return true;
    } catch (const SQLite::Exception& e) {
        // getErrorCode 可能是扩展码（primary | subcode << 8）：取低 8 位主码
        const int error_code = e.getErrorCode();
        const int primary_code = error_code > 0 ? (error_code & 0xff) : error_code;
        if (primary_code == kSqliteCorrupt || primary_code == kSqliteNotADatabase) {
            return false;
        }
        throw_retryable_probe_error(path, primary_code, e.what());
    } catch (const std::exception& e) {
        throw std::runtime_error(error_text(path, "probe failed retryable", e.what()));
    }
}

}  // namespace

std::string quarantine_trade_db(const std::filesystem::path& path) {
    const std::string main = path.string();
    const std::string stamp = timestamp_now();
    std::string target = main + ".old-" + stamp;
    for (int index = 1;
         std::filesystem::exists(target) || std::filesystem::exists(target + "-wal") ||
         std::filesystem::exists(target + "-shm");
         ++index) {
        target = main + ".old-" + stamp + "-" + std::to_string(index);
    }
    std::error_code ec;
    std::filesystem::rename(main, target, ec);
    if (ec) {
        throw std::runtime_error(error_text(path, "backup rename failed", ec.message()));
    }
    for (const std::string_view suffix : {"-wal", "-shm"}) {
        const std::string sidecar = main + std::string(suffix);
        if (!std::filesystem::exists(sidecar)) {
            continue;
        }
        std::filesystem::rename(sidecar, target + std::string(suffix), ec);
        if (ec) {
            throw std::runtime_error(
                error_text(path, "backup sidecar rename failed", ec.message()));
        }
    }
    return target;
}

TradeDbInitReport ensure_trade_db(const std::filesystem::path& path) {
    TradeDbInitReport report;
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        throw std::runtime_error(error_text(path, "exists check failed", ec.message()));
    }

    if (!exists) {
        create_new(path);
        report.status = TradeDbInitReport::Status::Created;
        return report;
    }

    // 目录/设备/管道等非普通文件不是可恢复的交易库：可重试错误（td 写端退避重连），
    // 不改名、不建库（避免把目录改名后再建文件这种破坏行为）。
    const bool regular = std::filesystem::is_regular_file(path, ec);
    if (ec) {
        throw std::runtime_error(error_text(path, "file type check failed", ec.message()));
    }
    if (!regular) {
        throw std::runtime_error(
            std::format("trade db path is not a regular file | path={}", path.string()));
    }

    int version = 0;
    const bool readable = probe_existing(path, version);
    if (readable && version == kTdSchemaVersion) {
        ensure_existing(path);
        report.status = TradeDbInitReport::Status::Existing;
        report.previous_version = version;
        return report;
    }

    // 旧版本 / 损坏 / 非 SQLite：改名保留 + 新建（开发期无兼容义务，ADR 0041）
    report.status = TradeDbInitReport::Status::Rebuilt;
    report.previous_version = readable ? version : -1;
    report.rebuild_reason = readable ? "schema_version_mismatch" : "corrupt_or_unreadable";
    report.backup_path = quarantine_trade_db(path);
    create_new(path);
    return report;
}

int64_t read_max_order_id(const std::filesystem::path& path) {
    try {
        SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
        db.exec("PRAGMA busy_timeout=5000");
        SQLite::Statement query(db, "SELECT COALESCE(MAX(order_id), 0) FROM orders");
        if (!query.executeStep()) {
            return 0;
        }
        return query.getColumn(0).getInt64();
    } catch (const SQLite::Exception& e) {
        throw std::runtime_error(error_text(path, "max order id read failed", e.what()));
    } catch (const std::exception& e) {
        throw std::runtime_error(error_text(path, "max order id read failed", e.what()));
    }
}

}  // namespace dztrader::tdstore
