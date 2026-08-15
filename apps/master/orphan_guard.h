#ifndef DZTRADER_MASTER_ORPHAN_GUARD_H_
#define DZTRADER_MASTER_ORPHAN_GUARD_H_

/**
 * @file orphan_guard.h
 * @brief 孤儿/僵尸进程检测与清理 + file_lock 单实例保护。
 *
 * 使用 SQLite 追踪 PID（崩溃一致性），boost::interprocess::file_lock
 * 防止多个 master 实例，boost::process::v2::ext::exe() 检测进程存活。
 */

#include "config.h"

#include <SQLiteCpp/Database.h>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/process/v2/pid.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace dztrader::master {

namespace process_v2 = boost::process::v2;

struct PidRecord {
    process_v2::pid_type pid;
    std::string name;
    std::string exe_path;
};

class OrphanGuard {
public:
    /// 启动：获取 file_lock + 清理残留子进程。
    /// 若 file_lock 无法获取（另一实例运行中），抛出 std::runtime_error。
    void startup();

    /// 子进程启动后注册 PID。
    void register_child(process_v2::pid_type pid,
                        std::string_view name, const std::filesystem::path& exe);

    /// 子进程退出后注销 PID。
    void unregister_child(process_v2::pid_type pid);

    /// 清理 children 表（master 关闭时调用）。
    void cleanup();

private:
    /// 创建 children 表（如不存在）。
    void init_db();

    /// 读取 children 表所有记录。
    std::vector<PidRecord> read_children();

    /// 通过 PID 终止残留进程（Windows 上需 try-catch）。
    void terminate_orphan(process_v2::pid_type pid, const std::string& name);

    /// 清空 children 表。
    void clear_children();

    std::unique_ptr<boost::interprocess::file_lock> instance_lock_;
    std::filesystem::path lock_path_;    // $DZTRADER_HOME/cache/master.pid
    std::filesystem::path db_path_;      // $DZTRADER_HOME/cache/children.db
    std::unique_ptr<SQLite::Database> db_;
};

}  // namespace dztrader::master

#endif  // DZTRADER_MASTER_ORPHAN_GUARD_H_
