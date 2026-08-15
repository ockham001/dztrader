#ifndef DZTRADER_SHM_NULL_LOCK_H_
#define DZTRADER_SHM_NULL_LOCK_H_

namespace dztrader::shm {

class NullLock {
public:
    NullLock() noexcept = default;
    void lock() noexcept {}
    bool try_lock() noexcept { return true; }
    void unlock() noexcept {}
};

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_NULL_LOCK_H_
