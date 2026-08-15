#include "test_ipc_util.h"

#include <chrono>
#include <dztrader/shm/named_semaphore.h>
#include <gtest/gtest.h>
#include <string>
#include <thread>

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

    sem.wait();
    notifier.join();
}

TEST_F(NamedSemaphoreTest, NotifyBeforeWait) {
    NamedSemaphore sem(sem_name_);

    sem.notify();
    sem.wait();
}

TEST_F(NamedSemaphoreTest, WaitForTimeout) {
    NamedSemaphore sem(sem_name_);

    auto start = std::chrono::steady_clock::now();
    bool rc = sem.wait_for(200);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(rc);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 180);
}

TEST_F(NamedSemaphoreTest, WaitForNotifiedWithinTimeout) {
    NamedSemaphore sem(sem_name_);

    std::thread notifier([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        sem.notify();
    });

    EXPECT_TRUE(sem.wait_for(5000));
    notifier.join();
}

TEST_F(NamedSemaphoreTest, CrossProcessNotify) {
    NamedSemaphore sem(sem_name_);

    std::atomic<bool> wait_done{false};
    std::thread waiter([&] {
        sem.wait();
        wait_done.store(true);
    });

    boost::asio::io_context ctx;
    auto proc = spawn_helper(ctx, {"sem_notify", sem_name_});

    waiter.join();
    EXPECT_TRUE(wait_done.load());

    EXPECT_EQ(wait_with_timeout(proc, std::chrono::seconds(5)), 0);
}

TEST_F(NamedSemaphoreTest, CrossProcessWait) {
    NamedSemaphore sem(sem_name_);

    boost::asio::io_context ctx;
    auto proc = spawn_helper(ctx, {"sem_wait", sem_name_});

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sem.notify();

    EXPECT_EQ(wait_with_timeout(proc, std::chrono::seconds(5)), 0);
}

TEST_F(NamedSemaphoreTest, CrossProcessWaitFor) {
    NamedSemaphore sem(sem_name_);

    boost::asio::io_context ctx;
    auto proc = spawn_helper(ctx, {"sem_wait_for_and_notify", sem_name_, "5000"});

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sem.notify();

    EXPECT_EQ(wait_with_timeout(proc, std::chrono::seconds(6)), 0);
}