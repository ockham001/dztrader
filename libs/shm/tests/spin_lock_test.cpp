#include <atomic>
#include <dztrader/shm/spin_lock.h>
#include <gtest/gtest.h>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>

using dztrader::shm::SpinLock;

TEST(SpinLockTest, TryLock) {
    SpinLock lock;

    EXPECT_TRUE(lock.try_lock());
    lock.unlock();

    lock.lock();
    EXPECT_FALSE(lock.try_lock());
    lock.unlock();
}

TEST(SpinLockTest, MultiThreadContention) {
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
                // load/store 而非 fetch_add：失去互斥时会丢更新，锁坏掉不能继续全绿
                const int64_t current = counter.load(std::memory_order_relaxed);
                counter.store(current + 1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(counter.load(), static_cast<int64_t>(kThreads) * kIncrements);
}

TEST(SpinLockTest, TryLockContention) {
    SpinLock lock;
    std::latch holder_ready(1);
    std::latch contender_tried(1);
    std::atomic<bool> holder_acquired{false};
    std::atomic<bool> overlap{false};

    std::thread holder([&] {
        holder_acquired.store(lock.try_lock());
        holder_ready.count_down();
        contender_tried.wait();
        if (holder_acquired.load()) {
            lock.unlock();
        }
    });

    std::thread contender([&] {
        holder_ready.wait();
        if (lock.try_lock()) {
            overlap.store(true);
            lock.unlock();
        }
        contender_tried.count_down();
    });

    holder.join();
    contender.join();

    EXPECT_TRUE(holder_acquired.load());
    EXPECT_FALSE(overlap.load());

    // 正向对照：锁释放后 try_lock 仍能成功，排除「恒失败」的假实现
    EXPECT_TRUE(lock.try_lock());
    lock.unlock();
}
