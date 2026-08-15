/**
 * @file process_mutex.h
 * @brief 进程锁（跨进程互斥，崩溃安全）
 *
 * RAII：构造即有效，性能路径不判空。
 * Windows：CreateMutex 命名内核对象
 * Linux：  robust mutex（共享内存中，PTHREAD_MUTEX_ROBUST）
 *
 * 崩溃恢复：WAIT_ABANDONED / EOWNERDEAD 视为获取锁成功，不向上层报告。
 * 原因：帧完整性由原子写入位置保证，锁内不存在半写状态需上层处理。
 *
 * 可重入性差异（无法统一，有意为之）：
 *   Windows CreateMutex 天生递归；Linux robust mutex 不可递归。
 *   崩溃安全优先于递归，本锁调用路径不会递归加锁，故差异不影响正确性。
 */
#ifndef DZTRADER_SHM_PROCESS_MUTEX_H_
#define DZTRADER_SHM_PROCESS_MUTEX_H_

#include <dztrader/core/exception.h>
#include <dztrader/core/last_error.h>

namespace dztrader::shm {

namespace process_mutex_internal {
class LockImpl;
}

class ProcessMutex {
public:
    /// @param name 锁名称（自动加 dz.lock. 前缀）
    explicit ProcessMutex(const std::string& name);

    ProcessMutex(const ProcessMutex&) = delete;
    ProcessMutex& operator=(const ProcessMutex&) = delete;

    ProcessMutex(ProcessMutex&& other) noexcept;
    ProcessMutex& operator=(ProcessMutex&& other) noexcept;

    ~ProcessMutex();

    void lock() noexcept;
    bool try_lock() noexcept;
    void unlock() noexcept;

    /// 仅主进程调用，清理残留锁对象
    static void remove(const std::string& name);

private:
    using LockImpl = process_mutex_internal::LockImpl;
    LockImpl* impl_ = nullptr;
};

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_PROCESS_MUTEX_H_
