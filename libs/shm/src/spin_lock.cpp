#include <dztrader/shm/spin_lock.h>

namespace dztrader::shm {

// ── 跨平台 pause 指令（仅支持 64 位 x86_64）──
#if defined(_WIN32) && defined(_M_X64)
#include <intrin.h>
#define DZ_SPIN_PAUSE() _mm_pause()
#elif defined(__x86_64__)
#define DZ_SPIN_PAUSE() __asm__ __volatile__("pause" :::)
#else
#error "unsupported: dztrader requires Windows x86_64 (MSVC) or Linux x86_64 (GCC)"
#endif

void SpinLock::lock() noexcept
{
    while (!try_lock()) {
        DZ_SPIN_PAUSE();
    }
}

/// 编译期验证 noexcept 保证——防止未来修改意外破坏
static_assert(noexcept(std::declval<SpinLock>().lock()));
static_assert(noexcept(std::declval<SpinLock>().try_lock()));
static_assert(noexcept(std::declval<SpinLock>().unlock()));

}  // namespace dztrader::shm
