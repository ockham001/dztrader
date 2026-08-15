#include <atomic>
#include <dztrader/shm/spin_lock.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using dztrader::shm::SpinLock;

TEST(SpinLockTest, BasicLockUnlock)
{
    SpinLock lock;
    lock.lock();
    lock.unlock();
}

TEST(SpinLockTest, TryLock)
{
    SpinLock lock;

    EXPECT_TRUE(lock.try_lock());
    lock.unlock();

    lock.lock();
    EXPECT_FALSE(lock.try_lock());
    lock.unlock();
}

TEST(SpinLockTest, MultiThreadContention)
{
    constexpr int kThreads = 8;
    constexpr int kIncrements = 10000;

    SpinLock lock;
    std::atomic<int64_t> counter{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < kIncrements; ++j) {
                std::scoped_lock<SpinLock> guard(lock);
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(counter.load(), static_cast<int64_t>(kThreads) * kIncrements);
}

TEST(SpinLockTest, TryLockContention)
{
    constexpr int kThreads = 8;
    constexpr int kAttempts = 10000;

    SpinLock lock;
    std::atomic<int64_t> counter{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < kAttempts; ++j) {
                if (lock.try_lock()) {
                    counter.fetch_add(1, std::memory_order_relaxed);
                    lock.unlock();
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_GT(counter.load(), 0);
    EXPECT_LE(counter.load(), static_cast<int64_t>(kThreads) * kAttempts);
}

TEST(SpinLockTest, InShmNamespace)
{
    dztrader::shm::SpinLock lock;
    lock.lock();
    lock.unlock();
}
