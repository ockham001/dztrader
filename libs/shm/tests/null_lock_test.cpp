#include <dztrader/shm/null_lock.h>
#include <gtest/gtest.h>
#include <thread>

using dztrader::shm::NullLock;

TEST(NullLockTest, LockUnlockNoop)
{
    NullLock lock;
    lock.lock();
    lock.unlock();
}

TEST(NullLockTest, TryLockAlwaysSucceeds)
{
    NullLock lock;
    EXPECT_TRUE(lock.try_lock());
    lock.unlock();
}

TEST(NullLockTest, CompatibleWithScopedLock)
{
    NullLock lock;
    {
        std::scoped_lock<NullLock> guard(lock);
    }
}

TEST(NullLockTest, MultiThreadNoContention)
{
    NullLock lock;
    std::atomic<int64_t> counter{0};
    constexpr int kThreads = 4;
    constexpr int kIncrements = 1000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < kIncrements; ++j) {
                std::scoped_lock<NullLock> guard(lock);
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(counter.load(), static_cast<int64_t>(kThreads) * kIncrements);
}

TEST(NullLockTest, InShmNamespace)
{
    dztrader::shm::NullLock lock;
    lock.lock();
    lock.unlock();
}

TEST(NullLockTest, SatisfiesLockable)
{
    static_assert(std::is_same_v<decltype(std::declval<NullLock>().lock()), void>);
    static_assert(std::is_same_v<decltype(std::declval<NullLock>().try_lock()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<NullLock>().unlock()), void>);
}
