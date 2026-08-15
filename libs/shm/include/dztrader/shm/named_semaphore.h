/**
 * @file named_semaphore.h
 * @brief 命名信号量（跨进程，仅用于唤醒）
 *
 * 信号量不用于精确计数同步，仅用于唤醒策略进程。
 * 虚假唤醒和多余的 notify 均无害——调用方被唤醒后自行判断是否有新数据。
 */
#ifndef DZTRADER_SHM_NAMED_SEMAPHORE_H_
#define DZTRADER_SHM_NAMED_SEMAPHORE_H_

#include <string>

#include <dztrader/core/exception.h>
#include <dztrader/core/last_error.h>

namespace dztrader::shm {

namespace named_semaphore_internal {
class SemImpl;
}

class NamedSemaphore {
public:
    /// @param name 信号量名称（自动加 dz.sem. 前缀）
    explicit NamedSemaphore(const std::string& name);

    NamedSemaphore(const NamedSemaphore&) = delete;
    NamedSemaphore& operator=(const NamedSemaphore&) = delete;
    NamedSemaphore(NamedSemaphore&& other) noexcept;
    NamedSemaphore& operator=(NamedSemaphore&& other) noexcept;

    ~NamedSemaphore();

    void wait() noexcept;

    bool wait_for(uint32_t timeout_ms) noexcept;

    void notify() noexcept;

    [[nodiscard]] static bool uses_monotonic_clock() noexcept;

    static void remove(const std::string& name);

private:
    using SemImpl = named_semaphore_internal::SemImpl;
    SemImpl* impl_ = nullptr;
};

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_NAMED_SEMAPHORE_H_
