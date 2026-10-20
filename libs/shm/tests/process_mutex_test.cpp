#include "test_ipc_util.h"

#include <atomic>
#include <chrono>
#include <dztrader/error.h>
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

namespace {

template <typename Stop>
void expect_abandoned_recovery(const std::string& mutex_name, Stop stop) {
    ProcessMutex::remove(mutex_name);

    auto signal_file = std::filesystem::temp_directory_path() / ("dz_test_lock_" + mutex_name);

    boost::asio::io_context ctx;
    auto proc =
        spawn_helper(ctx, {"mutex_lock_and_hold", mutex_name, "60000", signal_file.string()});

    ASSERT_TRUE(wait_for_file(signal_file));
    std::filesystem::remove(signal_file);

    stop(proc);
    int status = wait_with_timeout(proc, std::chrono::seconds(5));
    ASSERT_NE(status, -1) << "helper still running after stop request";
    EXPECT_FALSE(proc.running()) << "helper still running after stop request";

    ProcessMutex mtx(mutex_name);
    const bool recovered = mtx.try_lock();
    if (recovered) {
        mtx.unlock();
    }
    EXPECT_TRUE(recovered) << "abandoned mutex was not recovered";
}

}  // namespace

class ProcessMutexTest : public ::testing::Test {
protected:
    std::string mutex_name_;

    void SetUp() override {
        mutex_name_ = "test_mtx_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    void TearDown() override { ProcessMutex::remove(mutex_name_); }
};

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
    expect_abandoned_recovery(mutex_name_, [](bpv::process& proc) { proc.terminate(); });
}

TEST_F(ProcessMutexTest, AbandonedMutexRecoveryAfterRequestExit) {
    expect_abandoned_recovery(mutex_name_, [](bpv::process& proc) { proc.request_exit(); });
}

TEST_F(ProcessMutexTest, AbandonedMutexRecoveryAfterInterrupt) {
    expect_abandoned_recovery(mutex_name_, [](bpv::process& proc) { proc.interrupt(); });
}

TEST_F(ProcessMutexTest, ConstructorThrowsForInvalidName) {
#ifdef _WIN32
    const std::string invalid_name = "bad\\name";
#else
    const std::string invalid_name(300, 'x');
#endif

    try {
        ProcessMutex mtx(invalid_name);
        FAIL() << "ProcessMutex should reject an invalid name";
    } catch (const dztrader::Exception& e) {
        EXPECT_EQ(e.code(), DZ_EC_SHM_LOCK_FAILED);
    }
}
