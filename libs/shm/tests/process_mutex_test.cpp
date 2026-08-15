#include "test_ipc_util.h"

#include <atomic>
#include <chrono>
#include <dztrader/shm/process_mutex.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

using dztrader::shm::ProcessMutex;
using dztrader::shm::test::spawn_helper;
using dztrader::shm::test::wait_for_file;
using dztrader::shm::test::wait_with_timeout;

class ProcessMutexTest : public ::testing::Test {
protected:
    std::string mutex_name_;

    void SetUp() override {
        mutex_name_ = "test_mtx_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    void TearDown() override { ProcessMutex::remove(mutex_name_); }
};

TEST_F(ProcessMutexTest, BasicLockUnlock) {
    ProcessMutex mtx(mutex_name_);
    mtx.lock();
    mtx.unlock();
}

TEST_F(ProcessMutexTest, TryLock) {
    ProcessMutex mtx(mutex_name_);

    EXPECT_TRUE(mtx.try_lock());
    mtx.unlock();

    mtx.lock();
    std::atomic<bool> try_lock_result{true};
    std::thread t([&] {
        try_lock_result = mtx.try_lock();
        if (try_lock_result) {
            mtx.unlock();
        }
    });
    t.join();
    EXPECT_FALSE(try_lock_result);
    mtx.unlock();
}

TEST_F(ProcessMutexTest, MultiThreadContention) {
    constexpr int kThreads = 8;
    constexpr int kIncrements = 10000;

    ProcessMutex mtx(mutex_name_);
    std::atomic<int64_t> counter{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < kIncrements; ++j) {
                std::scoped_lock<ProcessMutex> guard(mtx);
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(counter.load(), static_cast<int64_t>(kThreads) * kIncrements);
}

TEST_F(ProcessMutexTest, CrossProcessMutualExclusion) {
    ProcessMutex mtx(mutex_name_);
    std::atomic<int64_t> counter{0};

    auto signal_file = std::filesystem::temp_directory_path() / ("dz_test_lock_" + mutex_name_);
    boost::asio::io_context ctx;
    auto proc =
        spawn_helper(ctx, {"mutex_lock_and_hold", mutex_name_, "2000", signal_file.string()});

    ASSERT_TRUE(wait_for_file(signal_file));
    std::filesystem::remove(signal_file);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        std::lock_guard<ProcessMutex> guard(mtx);
        counter.fetch_add(1, std::memory_order_relaxed);
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();

    EXPECT_GE(elapsed_ms, 1500);
    EXPECT_EQ(counter.load(), 100);

    EXPECT_EQ(wait_with_timeout(proc, std::chrono::seconds(10)), 0);
}

TEST_F(ProcessMutexTest, AbandonedMutexRecovery) {
    ProcessMutex::remove(mutex_name_);

    auto signal_file = std::filesystem::temp_directory_path() / ("dz_test_lock_" + mutex_name_);

    {
        boost::asio::io_context ctx;
        auto proc =
            spawn_helper(ctx, {"mutex_lock_and_hold", mutex_name_, "60000", signal_file.string()});

        ASSERT_TRUE(wait_for_file(signal_file));
        std::filesystem::remove(signal_file);

        proc.terminate();
        wait_with_timeout(proc, std::chrono::seconds(2));
    }

    ProcessMutex mtx(mutex_name_);
    mtx.lock();
    mtx.unlock();
}

TEST_F(ProcessMutexTest, AbandonedMutexRecoveryAfterRequestExit) {
    ProcessMutex::remove(mutex_name_);

    auto signal_file = std::filesystem::temp_directory_path() / ("dz_test_lock_" + mutex_name_);

    {
        boost::asio::io_context ctx;
        auto proc =
            spawn_helper(ctx, {"mutex_lock_and_hold", mutex_name_, "60000", signal_file.string()});

        ASSERT_TRUE(wait_for_file(signal_file));
        std::filesystem::remove(signal_file);

        proc.request_exit();
        wait_with_timeout(proc, std::chrono::seconds(5));
    }

    ProcessMutex mtx(mutex_name_);
    mtx.lock();
    mtx.unlock();
}

TEST_F(ProcessMutexTest, AbandonedMutexRecoveryAfterInterrupt) {
    ProcessMutex::remove(mutex_name_);

    auto signal_file = std::filesystem::temp_directory_path() / ("dz_test_lock_" + mutex_name_);

    {
        boost::asio::io_context ctx;
        auto proc =
            spawn_helper(ctx, {"mutex_lock_and_hold", mutex_name_, "60000", signal_file.string()});

        ASSERT_TRUE(wait_for_file(signal_file));
        std::filesystem::remove(signal_file);

        proc.interrupt();
        wait_with_timeout(proc, std::chrono::seconds(5));
    }

    ProcessMutex mtx(mutex_name_);
    mtx.lock();
    mtx.unlock();
}
