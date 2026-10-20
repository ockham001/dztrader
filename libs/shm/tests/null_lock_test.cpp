#include <dztrader/shm/null_lock.h>
#include <gtest/gtest.h>
#include <type_traits>
#include <utility>

using dztrader::shm::NullLock;

TEST(NullLockTest, TryLockAlwaysSucceeds) {
    NullLock lock;
    EXPECT_TRUE(lock.try_lock());
    lock.unlock();
}

TEST(NullLockTest, SatisfiesLockable) {
    static_assert(std::is_same_v<decltype(std::declval<NullLock>().lock()), void>);
    static_assert(std::is_same_v<decltype(std::declval<NullLock>().try_lock()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<NullLock>().unlock()), void>);
}
