#include <chrono>
#include <dztrader/shm/process_mutex.h>
#include <string>
#include <thread>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#elif defined(__linux__)

#include <boost/interprocess/managed_shared_memory.hpp>
#include <cerrno>
#include <pthread.h>
#else
#error "unsupported platform: process_mutex requires Windows or Linux with robust mutex (PTHREAD_MUTEX_ROBUST)"
#endif

namespace dztrader::shm {

namespace process_mutex_internal {
// ── Windows ──────────────────────────────────────────────────────────────

#ifdef _WIN32

class LockImpl {
public:
    explicit LockImpl(const std::string& name)
    {
        handle_ = CreateMutexA(nullptr, FALSE, ("dz.lock." + name).c_str());
        if (!handle_) {
            throw Exception(DZ_EC_SHM_LOCK_FAILED, "process lock create failed: name={} err={}", name, GetLastError());
        }
    }

    ~LockImpl()
    {
        if (handle_) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    LockImpl(const LockImpl&) = delete;
    LockImpl& operator=(const LockImpl&) = delete;

    LockImpl(LockImpl&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

    LockImpl& operator=(LockImpl&& other) noexcept
    {
        if (this != &other) {
            if (handle_) {
                CloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    /// 加锁（阻塞）。WAIT_ABANDONED 视为成功，不报告崩溃恢复——有意为之，勿加 abandoned 标志
    void lock() noexcept
    {
        for (;;) {
            switch (WaitForSingleObject(handle_, INFINITE)) {
            [[likely]] case WAIT_OBJECT_0:  // NOLINT
                return;
            [[unlikely]] case WAIT_ABANDONED:
                return;
            [[unlikely]] default:
                // WAIT_FAILED 等：盘中终止进程代价太大，sleep 后重试撑到盘后处理
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }
    }

    /// 非阻塞尝试加锁。WAIT_ABANDONED 视为成功，不报告崩溃恢复——有意为之，勿加 abandoned 标志
    bool try_lock() noexcept
    {
        switch (WaitForSingleObject(handle_, 0)) {
        [[likely]] case WAIT_OBJECT_0:
            return true;
        [[unlikely]] case WAIT_ABANDONED:
            return true;
        [[unlikely]] default:
            // 此处设置错误码无意义：错误码是 thread_local，调用方无法获取
            return false;
        }
    }

    /// 解锁
    void unlock() noexcept
    {
        // 不需要检查返回值，因为解锁失败对进程没有影响
        ReleaseMutex(handle_);
    }

    /// Windows 内核对象随句柄关闭自动销毁，此函数无操作
    static void remove(const std::string& /*name*/) {}

private:
    HANDLE handle_ = nullptr;
};

// ── Linux ─────────────────────────────────────────────────────────────────

#elif defined(__linux__)


namespace bip = boost::interprocess;

/// find_or_construct 构造回调：初始化 PTHREAD_PROCESS_SHARED + PTHREAD_MUTEX_ROBUST；不析构（mutex 在共享内存中，进程退出不 destroy）
struct PthreadMutexWrapper {
    pthread_mutex_t mutex{};

    PthreadMutexWrapper()
    {
        pthread_mutexattr_t attr;
        int rc = pthread_mutexattr_init(&attr);
        if (rc != 0) {
            throw Exception(DZ_EC_SHM_LOCK_FAILED, "pthread_mutexattr_init failed: err={}", rc);
        }
        rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (rc != 0) {
            pthread_mutexattr_destroy(&attr);
            throw Exception(DZ_EC_SHM_LOCK_FAILED, "pthread_mutexattr_setpshared failed: err={}", rc);
        }
        rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        if (rc != 0) {
            pthread_mutexattr_destroy(&attr);
            throw Exception(DZ_EC_SHM_LOCK_FAILED, "pthread_mutexattr_setrobust failed: err={}", rc);
        }
        rc = pthread_mutex_init(&mutex, &attr);
        if (rc != 0) {
            pthread_mutexattr_destroy(&attr);
            throw Exception(DZ_EC_SHM_LOCK_FAILED, "pthread_mutex_init failed: err={}", rc);
        }
        pthread_mutexattr_destroy(&attr);
    }

    PthreadMutexWrapper(const PthreadMutexWrapper&) = delete;
    PthreadMutexWrapper& operator=(const PthreadMutexWrapper&) = delete;
    PthreadMutexWrapper(PthreadMutexWrapper&&) = delete;
    PthreadMutexWrapper& operator=(PthreadMutexWrapper&&) = delete;
};

class LockImpl {
public:
    explicit LockImpl(const std::string& name)
    {
        const auto seg_name = "dz.lock." + name;
        segment_ = bip::managed_shared_memory(bip::open_or_create, seg_name.c_str(),
                                              4096  // 一页，segment header + 一个 mutex 绰绰有余
        );
        auto* wrapper = segment_.find_or_construct<PthreadMutexWrapper>("mutex")();
        if (!wrapper) {
            throw Exception(DZ_EC_SHM_LOCK_FAILED, "process lock construct failed: name={}", name);
        }
        mutex_ = &wrapper->mutex;
    }

    ~LockImpl() = default;

    LockImpl(const LockImpl&) = delete;
    LockImpl& operator=(const LockImpl&) = delete;

    LockImpl(LockImpl&& other) noexcept : mutex_(other.mutex_), segment_(std::move(other.segment_))
    {
        other.mutex_ = nullptr;
    }

    LockImpl& operator=(LockImpl&& other) noexcept
    {
        if (this != &other) {
            mutex_ = other.mutex_;
            segment_ = std::move(other.segment_);
            other.mutex_ = nullptr;
        }
        return *this;
    }

    /// 加锁（阻塞）。EOWNERDEAD 视为成功并自动 consistent，不报告崩溃恢复——有意为之，勿加 abandoned 标志
    void lock() noexcept
    {
        for (;;) {
            switch (pthread_mutex_lock(mutex_)) {
            [[likely]] case 0:
                return;
            [[unlikely]] case EOWNERDEAD:
                // consistent 唯一可能失败是 EINVAL（mutex 非 EOWNERDEAD），但刚收到 EOWNERDEAD 且已持有锁，不可能发生
                pthread_mutex_consistent(mutex_);
                return;
            [[unlikely]] default:
                // 其他错误（如 EAGAIN）：盘中终止进程代价太大，sleep 后重试撑到盘后处理
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }
    }

    /// 非阻塞尝试加锁。EOWNERDEAD 视为成功并自动 consistent，不报告崩溃恢复——有意为之，勿加 abandoned 标志
    bool try_lock() noexcept
    {
        switch (pthread_mutex_trylock(mutex_)) {
        [[likely]] case 0:
            return true;
        [[unlikely]] case EOWNERDEAD:
            // 同 lock() 中注释——有意忽略 consistent 返回值
            pthread_mutex_consistent(mutex_);
            return true;
        [[unlikely]] default:
            // 此处设置错误码无意义：错误码是 thread_local，调用方无法获取
            return false;
        }
    }

    /// 解锁
    void unlock() noexcept
    {
        // 不需要检查返回值，因为解锁失败对进程没有影响
        pthread_mutex_unlock(mutex_);
    }

    /// 删除共享内存段（仅主进程调用，清理残留）
    static void remove(const std::string& name)
    {
        bip::shared_memory_object::remove(("dz.lock." + name).c_str());
    }

private:
    pthread_mutex_t* mutex_ = nullptr;
    bip::managed_shared_memory segment_;
};

#else
#error "unsupported platform: process_mutex requires Windows or Linux with robust mutex (PTHREAD_MUTEX_ROBUST)"
#endif

}  // namespace process_mutex_internal

ProcessMutex::ProcessMutex(const std::string& name) : impl_(new LockImpl(name)) {}

ProcessMutex::ProcessMutex(ProcessMutex&& other) noexcept : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

ProcessMutex& ProcessMutex::operator=(ProcessMutex&& other) noexcept
{
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

ProcessMutex::~ProcessMutex()
{
    delete impl_;
    impl_ = nullptr;
}

void ProcessMutex::lock() noexcept
{
    impl_->lock();
}

bool ProcessMutex::try_lock() noexcept
{
    return impl_->try_lock();
}

void ProcessMutex::unlock() noexcept
{
    impl_->unlock();
}

void ProcessMutex::remove(const std::string& name)
{
    LockImpl::remove(name);
}

/// 编译期验证 noexcept 保证——防止未来修改意外破坏
static_assert(noexcept(std::declval<process_mutex_internal::LockImpl>().lock()));
static_assert(noexcept(std::declval<process_mutex_internal::LockImpl>().try_lock()));
static_assert(noexcept(std::declval<process_mutex_internal::LockImpl>().unlock()));
static_assert(noexcept(std::declval<ProcessMutex>().lock()));
static_assert(noexcept(std::declval<ProcessMutex>().try_lock()));
static_assert(noexcept(std::declval<ProcessMutex>().unlock()));

}  // namespace dztrader::shm
