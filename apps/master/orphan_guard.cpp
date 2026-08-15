#include "orphan_guard.h"

#include <dztrader/core/encoding.h>
#include <dztrader/core/path.h>
#include <dztrader/core/exception.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/error.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>
#include <boost/process/v2/ext/exe.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/asio/io_context.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>
#include <thread>

namespace dztrader::master {

namespace {

/// 获取当前时间戳（秒，Unix epoch）。
int64_t current_timestamp() {
    return DateTime::system_now().timestamp<int64_t>();
}

}  // namespace

void OrphanGuard::startup() {
    lock_path_ = dztrader::paths::cache() / "master.pid";
    db_path_   = dztrader::paths::cache() / "children.db";

    // 1. 获取 file_lock 防止多实例
    // 确保锁文件的父目录存在
    std::filesystem::create_directories(lock_path_.parent_path());

    // 确保锁文件存在
    if (!std::filesystem::exists(lock_path_)) {
        std::ofstream ofs(lock_path_);
        if (!ofs) {
            throw Exception(DZ_EC_MASTER_LOCK_FAILED, "lock file create failed | path={}", lock_path_.string());
        }
    }

    try {
        instance_lock_ = std::make_unique<boost::interprocess::file_lock>(
            lock_path_.string().c_str());
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_MASTER_LOCK_FAILED, "file_lock create failed | path={} error=\"{}\"",
                    lock_path_.string(), e.what());
    }

    if (!instance_lock_->try_lock()) {
        throw Exception(DZ_EC_MASTER_ALREADY_RUNNING, "another master instance is running");
    }
    SPDLOG_INFO("instance lock acquired | path={}", lock_path_.string());

    // 2. 打开/创建 SQLite 数据库，启用 WAL 模式和忙等待
    try {
        db_ = std::make_unique<SQLite::Database>(db_path_.string(),
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db_->exec("PRAGMA journal_mode=WAL");
        db_->exec("PRAGMA busy_timeout=5000");
        init_db();
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_MASTER_DB_FAILED, "children db open failed | path={} error=\"{}\"",
                    db_path_.string(), e.what());
    }

    // 3. 清理残留子进程
    auto records = read_children();
    for (const auto& rec : records) {
        try {
            // 使用 ext::exe(pid) 检查进程是否存活
            // ext::exe 成功 = 进程存活，失败 = 进程已死
            boost::system::error_code ec;
            auto exe_result = process_v2::ext::exe(rec.pid, ec);

            if (ec) {
                // 进程已死，跳过
                SPDLOG_INFO("orphan process dead | pid={} name={}", rec.pid, rec.name);
                continue;
            }

            // 将 boost::filesystem::path 转为字符串进行比较
            auto exe_str = exe_result.string();

            // 规范化两个路径后比较（避免相对路径/混合分隔符导致误判）
            std::error_code canon_ec;
            auto canon_exe = std::filesystem::weakly_canonical(
                std::filesystem::path(exe_str), canon_ec);
            auto canon_stored = std::filesystem::weakly_canonical(
                std::filesystem::path(rec.exe_path), canon_ec);

            // 进程存活。检查 exe_path 是否匹配（PID 复用检测）
            if (canon_exe == canon_stored) {
                // 相同可执行文件 — 这是我们的孤儿进程，终止它
                SPDLOG_WARN("terminating orphan | pid={} name={} exe={}",
                            rec.pid, rec.name, rec.exe_path);
                terminate_orphan(rec.pid, rec.name);
            } else {
                // PID 被不同进程复用 — 跳过
                SPDLOG_INFO("pid reused by other process, skipping | pid={} old_exe={} new_exe={}",
                            rec.pid, rec.exe_path, exe_str);
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("orphan check failed, skipping | pid={} name={} error=\"{}\"",
                         rec.pid, rec.name, e.what());
        }
    }

    // 4. 清空 children 表（所有孤儿已处理）
    clear_children();
    SPDLOG_INFO("orphan cleanup done");
}

void OrphanGuard::register_child(process_v2::pid_type pid,
                                  std::string_view name,
                                  const std::filesystem::path& exe) {
    if (!db_) { return; }
    try {
        SQLite::Statement insert(*db_,
            "INSERT OR REPLACE INTO children (pid, name, exe, ts) VALUES (?, ?, ?, ?)");
        insert.bind(1, static_cast<int64_t>(pid));
        insert.bind(2, std::string(name));
        insert.bind(3, exe.string());
        insert.bind(4, current_timestamp());
        insert.exec();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("register child failed | pid={} name={} error=\"{}\"", pid, name, e.what());
    }
}

void OrphanGuard::unregister_child(process_v2::pid_type pid) {
    if (!db_) { return; }
    try {
        SQLite::Statement del(*db_, "DELETE FROM children WHERE pid = ?");
        del.bind(1, static_cast<int64_t>(pid));
        del.exec();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("unregister child failed | pid={} error=\"{}\"", pid, e.what());
    }
}

void OrphanGuard::cleanup() {
    clear_children();

    // 释放 file_lock
    if (instance_lock_) {
        instance_lock_->unlock();
        instance_lock_.reset();  // 防止重复 unlock (cleanup 幂等)
        SPDLOG_INFO("instance lock released");
    }

    // 关闭数据库
    db_.reset();

    // 删除锁文件
    std::error_code ec;
    std::filesystem::remove(lock_path_, ec);
}

void OrphanGuard::init_db() {
    if (!db_) { return; }
    db_->exec(
        "CREATE TABLE IF NOT EXISTS children ("
        "  pid   INTEGER PRIMARY KEY,"
        "  name  TEXT NOT NULL,"
        "  exe   TEXT NOT NULL,"
        "  ts    INTEGER NOT NULL"
        ")"
    );
}

std::vector<PidRecord> OrphanGuard::read_children() {
    std::vector<PidRecord> records;
    if (!db_) { return records; }

    try {
        SQLite::Statement query(*db_, "SELECT pid, name, exe FROM children");
        while (query.executeStep()) {
            PidRecord rec;
            rec.pid = static_cast<process_v2::pid_type>(query.getColumn(0).getInt64());
            rec.name = query.getColumn(1).getString();
            rec.exe_path = query.getColumn(2).getString();
            records.push_back(std::move(rec));
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("read children table failed | error=\"{}\"", e.what());
    }

    return records;
}

void OrphanGuard::terminate_orphan(process_v2::pid_type pid, const std::string& name) {
    // Windows 上 process(executor, pid) 在 PID 不存在时抛异常，必须 try-catch
    try {
        boost::asio::io_context ioc;
        process_v2::process orphan(ioc.get_executor(), pid);
        boost::system::error_code ec;
        orphan.terminate(ec);
        if (!ec) {
            SPDLOG_INFO("orphan terminated | pid={} name={}", pid, name);
            return;
        }

        // terminate 失败 — 常见于父进程被强杀后子进程处于退出过渡状态，
        // Windows 对退出中进程的 TerminateProcess 返回 ERROR_ACCESS_DENIED。
        // 等待短时间让进程完成退出，再用 ext::exe 确认进程是否真的死亡。
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        boost::system::error_code check_ec;
        process_v2::ext::exe(pid, check_ec);
        if (check_ec) {
            // 进程已死 — 视为成功（在 terminate 调用过程中退出）
            SPDLOG_INFO("orphan exited during terminate | pid={} name={}", pid, name);
        } else {
            SPDLOG_WARN("terminate orphan failed | pid={} name={} error=\"{}\"",
                        pid, name, dztrader::to_utf8_from_system(ec.message()));
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("attach to orphan failed | pid={} name={} error=\"{}\"", pid, name, e.what());
    }
}

void OrphanGuard::clear_children() {
    if (!db_) { return; }
    try {
        db_->exec("DELETE FROM children");
    } catch (const std::exception& e) {
        SPDLOG_ERROR("clear children table failed | error=\"{}\"", e.what());
    }
}

}  // namespace dztrader::master
