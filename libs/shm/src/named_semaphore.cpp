#include <dztrader/shm/named_semaphore.h>
#include <dztrader/error.h>

namespace dztrader::shm {

namespace named_semaphore_internal {

// ── Windows ──────────────────────────────────────────────────────────────

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class SemImpl {
public:
    explicit SemImpl(const std::string& name) {
        handle_ = CreateSemaphoreA(nullptr, 0, LONG_MAX, ("dz.sem." + name).c_str());
        if (!handle_) {
            throw Exception(DZ_EC_SHM_SEM_CREATE_FAILED, "semaphore create failed: name={} err={}",
                            name, GetLastError());
        }
    }

    ~SemImpl() {
        if (handle_) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    SemImpl(const SemImpl&) = delete;
    SemImpl& operator=(const SemImpl&) = delete;

    SemImpl(SemImpl&& other) noexcept
        : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    SemImpl& operator=(SemImpl&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                CloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    void wait() noexcept { WaitForSingleObject(handle_, INFINITE); }

    bool wait_for(uint32_t timeout_ms) noexcept {
        DWORD result = WaitForSingleObject(handle_, timeout_ms);
        if (result == WAIT_OBJECT_0) {
            return true;
        }
        if (result == WAIT_TIMEOUT) {
            return false;
        }
        LastError::set(DZ_EC_SHM_SEM_WAIT_FAILED, "WaitForSingleObject failed: err={}",
                       GetLastError());
        return false;
    }

    static bool uses_monotonic_clock() noexcept { return true; }

    void notify() noexcept { ReleaseSemaphore(handle_, 1, nullptr); }

    static void remove(const std::string& /*name*/) {}

private:
    HANDLE handle_ = nullptr;
};

// ── POSIX ────────────────────────────────────────────────────────────────

#elif defined(__unix__)

#include <cerrno>
#include <fcntl.h>
#include <semaphore.h>
#include <time.h>

class SemImpl {
public:
    explicit SemImpl(const std::string& name) {
        sem_ = sem_open(("/dz.sem." + name).c_str(), O_CREAT, 0666, 0);
        if (sem_ == SEM_FAILED) {
            throw Exception(DZ_EC_SHM_SEM_OPEN_FAILED, "semaphore open failed: name={} err={}",
                            name, errno);
        }
    }

    ~SemImpl() {
        if (sem_ != SEM_FAILED) {
            sem_close(sem_);
            sem_ = SEM_FAILED;
        }
    }

    SemImpl(const SemImpl&) = delete;
    SemImpl& operator=(const SemImpl&) = delete;

    SemImpl(SemImpl&& other) noexcept
        : sem_(other.sem_) {
        other.sem_ = SEM_FAILED;
    }

    SemImpl& operator=(SemImpl&& other) noexcept {
        if (this != &other) {
            if (sem_ != SEM_FAILED) {
                sem_close(sem_);
            }
            sem_ = other.sem_;
            other.sem_ = SEM_FAILED;
        }
        return *this;
    }

    void wait() noexcept { sem_wait(sem_); }

    bool wait_for(uint32_t timeout_ms) noexcept {
        struct timespec ts{};
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 30)
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += static_cast<long>(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        if (sem_clockwait(sem_, CLOCK_MONOTONIC, &ts) == 0) {
            return true;
        }
#elif defined(__linux__)
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += static_cast<long>(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        if (sem_timedwait(sem_, &ts) == 0) {
            return true;
        }
#else
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += static_cast<long>(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        if (sem_timedwait(sem_, &ts) == 0) {
            return true;
        }
#endif
        if (errno == ETIMEDOUT) {
            return false;
        }
        LastError::set(DZ_EC_SHM_SEM_WAIT_FAILED, "sem timed wait failed: errno={}", errno);
        return false;
    }

#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 30)
    static bool uses_monotonic_clock() noexcept { return true; }
#else
    static bool uses_monotonic_clock() noexcept { return false; }
#endif

    void notify() noexcept { sem_post(sem_); }

    static void remove(const std::string& name) { sem_unlink(("/dz.sem." + name).c_str()); }

private:
    sem_t* sem_ = SEM_FAILED;
};

#else
#error "unsupported platform: semaphore requires Windows or POSIX"
#endif

}  // namespace named_semaphore_internal

static_assert(noexcept(std::declval<named_semaphore_internal::SemImpl>().wait()));
static_assert(noexcept(std::declval<named_semaphore_internal::SemImpl>().wait_for(0)));
static_assert(noexcept(std::declval<named_semaphore_internal::SemImpl>().notify()));
static_assert(noexcept(std::declval<NamedSemaphore>().wait()));
static_assert(noexcept(std::declval<NamedSemaphore>().wait_for(0)));
static_assert(noexcept(std::declval<NamedSemaphore>().notify()));

NamedSemaphore::NamedSemaphore(const std::string& name)
    : impl_(new SemImpl(name)) {}

NamedSemaphore::NamedSemaphore(NamedSemaphore&& other) noexcept
    : impl_(other.impl_) {
    other.impl_ = nullptr;
}

NamedSemaphore& NamedSemaphore::operator=(NamedSemaphore&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

NamedSemaphore::~NamedSemaphore() {
    delete impl_;
    impl_ = nullptr;
}

void NamedSemaphore::wait() noexcept { impl_->wait(); }

bool NamedSemaphore::wait_for(uint32_t timeout_ms) noexcept { return impl_->wait_for(timeout_ms); }

void NamedSemaphore::notify() noexcept { impl_->notify(); }

bool NamedSemaphore::uses_monotonic_clock() noexcept { return SemImpl::uses_monotonic_clock(); }

void NamedSemaphore::remove(const std::string& name) { SemImpl::remove(name); }

}  // namespace dztrader::shm
