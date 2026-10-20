#include "test_ipc_util.h"

#include <atomic>
#include <chrono>
#include <dztrader/error.h>
#include <dztrader/shm/named_semaphore.h>
#include <gtest/gtest.h>
#include <string>
#include <thread>

#if defined(__linux__)
#include <features.h>
#endif

using dztrader::shm::NamedSemaphore;
using dztrader::shm::test::spawn_helper;
using dztrader::shm::test::wait_with_timeout;

class NamedSemaphoreTest : public ::testing::Test {
protected:
    std::string sem_name_;

    void SetUp() override {
        sem_name_ = "test_sem_" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    void TearDown() override { NamedSemaphore::remove(sem_name_); }
};

TEST_F(NamedSemaphoreTest, BasicWaitNotify) {
    NamedSemaphore sem(sem_name_);

    std::thread notifier([&] { sem.notify(); });

    EXPECT_TRUE(sem.wait_for(5000));
    notifier.join();
}

TEST_F(NamedSemaphoreTest, NotifyBeforeWait) {
    NamedSemaphore sem(sem_name_);

    sem.notify();
    EXPECT_TRUE(sem.wait_for(200));
}

TEST_F(NamedSemaphoreTest, WaitForTimeout) {
    NamedSemaphore sem(sem_name_);

    auto start = std::chrono::steady_clock::now();
    bool rc = sem.wait_for(200);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(rc);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 180);
}

TEST_F(NamedSemaphoreTest, CrossProcessNotify) {
    NamedSemaphore sem(sem_name_);

    std::atomic<bool> wait_result{false};
    std::thread waiter([&] { wait_result.store(sem.wait_for(5000)); });

    boost::asio::io_context ctx;
    auto proc = spawn_helper(ctx, {"sem_notify", sem_name_});

    waiter.join();
    EXPECT_TRUE(wait_result.load());

    EXPECT_EQ(wait_with_timeout(proc, std::chrono::seconds(5)), 0);
}

TEST_F(NamedSemaphoreTest, CrossProcessWait) {
    NamedSemaphore sem(sem_name_);

    boost::asio::io_context ctx;
    auto proc = spawn_helper(ctx, {"sem_wait", sem_name_});

    sem.notify();

    EXPECT_EQ(wait_with_timeout(proc, std::chrono::seconds(5)), 0);
}

TEST_F(NamedSemaphoreTest, CrossProcessWaitFor) {
    NamedSemaphore sem(sem_name_);

    boost::asio::io_context ctx;
    auto proc = spawn_helper(ctx, {"sem_wait_for_and_notify", sem_name_, "5000"});

    sem.notify();

    EXPECT_EQ(wait_with_timeout(proc, std::chrono::seconds(6)), 0);
}

TEST_F(NamedSemaphoreTest, UsesMonotonicClockMatchesPlatform) {
#if defined(_WIN32)
    EXPECT_TRUE(NamedSemaphore::uses_monotonic_clock());
#elif defined(__linux__) && defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 30)
    EXPECT_TRUE(NamedSemaphore::uses_monotonic_clock());
#else
    EXPECT_FALSE(NamedSemaphore::uses_monotonic_clock());
#endif
#else
    EXPECT_FALSE(NamedSemaphore::uses_monotonic_clock());
#endif
}

TEST_F(NamedSemaphoreTest, ConstructorThrowsForInvalidName) {
#ifdef _WIN32
    const std::string invalid_name = "bad\\name";
    constexpr auto expected_code = DZ_EC_SHM_SEM_CREATE_FAILED;
#else
    const std::string invalid_name = "/";
    constexpr auto expected_code = DZ_EC_SHM_SEM_OPEN_FAILED;
#endif

    try {
        NamedSemaphore sem(invalid_name);
        FAIL() << "NamedSemaphore should reject an invalid name";
    } catch (const dztrader::Exception& e) {
        EXPECT_EQ(e.code(), expected_code);
    }
}
