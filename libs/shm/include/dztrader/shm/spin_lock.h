/**
 * @file spin_lock.h
 * @brief 自旋锁（单进程多线程）
 */

#ifndef DZTRADER_SHM_SPIN_LOCK_H_
#define DZTRADER_SHM_SPIN_LOCK_H_

#include <atomic>

namespace dztrader::shm {

class SpinLock {
public:
    SpinLock() noexcept = default;
    ~SpinLock() = default;

    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;
    SpinLock(SpinLock&&) = delete;
    SpinLock& operator=(SpinLock&&) = delete;

    void lock() noexcept;

    bool try_lock() noexcept { return !flag_.test_and_set(std::memory_order_acquire); }

    void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_;  // C++20 默认构造保证 clear
};

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_SPIN_LOCK_H_