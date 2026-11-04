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
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>

#include <dztrader/tdstore/schema_catalog.h>

namespace dztrader::tdstore {

namespace {

using dztrader::tdstore::TradeDbInitReport;

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

/// 探测既有库版本；返回 false = 打不开/结构不可读（按损坏库处理）。
bool probe_existing(const std::filesystem::path& path, int& version) {
    try {
        SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
        version = db.execAndGet("PRAGMA user_version").getInt();
        // 只碰 schema 页的最轻查询：能成功说明库结构可读（不是损坏/截断/非 SQLite）。
        (void)db.execAndGet("SELECT COUNT(*) FROM sqlite_master").getInt();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// 旧库改名保留（主文件 + WAL/SHM 伴生文件一起改名；不删除旧文件）。
std::string move_aside(const std::filesystem::path& path) {
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

}  // namespace

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
    report.backup_path = move_aside(path);
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
