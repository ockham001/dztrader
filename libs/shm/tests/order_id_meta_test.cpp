#include "test_util.h"

#include <dztrader/core/exception.h>
#include <dztrader/shm/order_id_meta.h>
#include <gtest/gtest.h>

using dztrader::shm::OrderIdMeta;
using dztrader::shm::test::cleanup_test_dir;
using dztrader::shm::test::test_shm_dir;
using dztrader::shm::test::unique_channel_name;

class OrderIdMetaTest : public ::testing::Test {
protected:
    std::string name_;
    std::filesystem::path shm_dir_;

    void SetUp() override {
        name_ = unique_channel_name("dz_test_oid");
        shm_dir_ = test_shm_dir(name_);
    }

    void TearDown() override { cleanup_test_dir(shm_dir_); }
};

TEST_F(OrderIdMetaTest, OpenOrCreateInitializesOrderId) {
    auto meta = OrderIdMeta::open_or_create(name_, shm_dir_);
    EXPECT_EQ(meta.current(), 0);
    EXPECT_EQ(meta.generate(), 0);
    EXPECT_EQ(meta.current(), 1);
}

TEST_F(OrderIdMetaTest, OpenExistingPreservesOrderId) {
    DzOrderId val = 0;
    {
        auto meta = OrderIdMeta::open_or_create(name_, shm_dir_);
        (void)meta.generate();
        (void)meta.generate();
        val = meta.current();
        EXPECT_EQ(val, 2);
    }

    {
        auto meta = OrderIdMeta::open_only(name_, shm_dir_);
        EXPECT_EQ(meta.current(), val);
    }
}

TEST_F(OrderIdMetaTest, GenerateIsMonotonic) {
    auto meta = OrderIdMeta::open_or_create(name_, shm_dir_);
    DzOrderId prev = meta.generate();
    for (int i = 0; i < 100; ++i) {
        DzOrderId cur = meta.generate();
        EXPECT_GT(cur, prev);
        prev = cur;
    }
}

TEST_F(OrderIdMetaTest, OrderIdAccessCreatorOnly) {
    {
        auto meta = OrderIdMeta::open_or_create(name_, shm_dir_);
    }

    auto meta = OrderIdMeta::open_only(name_, shm_dir_);
    EXPECT_THROW((void)meta.order_id(), dztrader::Exception);
}

TEST_F(OrderIdMetaTest, OrderIdStoreViaCreator) {
    auto meta = OrderIdMeta::open_or_create(name_, shm_dir_);
    EXPECT_EQ(meta.current(), 0);

    auto* atomic = meta.order_id();
    ASSERT_NE(atomic, nullptr);
    atomic->store(1000, boost::memory_order_release);
    EXPECT_EQ(meta.current(), 1000);
    EXPECT_EQ(meta.generate(), 1000);
    EXPECT_EQ(meta.current(), 1001);
}

TEST_F(OrderIdMetaTest, EnsureAtLeastBumpsCounter) {
    // 启动自检路径: 库内 max 高于计数器时 fetch_max 上调
    auto meta = OrderIdMeta::open_or_create(name_, shm_dir_);
    EXPECT_EQ(meta.ensure_at_least(100), 0);
    EXPECT_EQ(meta.current(), 100);
    EXPECT_EQ(meta.generate(), 100);
    EXPECT_EQ(meta.current(), 101);
}

TEST_F(OrderIdMetaTest, EnsureAtLeastNeverDecreases) {
    // 单调不减: 小于当前值时 no-op, 等于当前值时无变化
    auto meta = OrderIdMeta::open_or_create(name_, shm_dir_);
    EXPECT_EQ(meta.ensure_at_least(50), 0);
    EXPECT_EQ(meta.current(), 50);
    EXPECT_EQ(meta.ensure_at_least(10), 50);
    EXPECT_EQ(meta.current(), 50);
    EXPECT_EQ(meta.ensure_at_least(50), 50);
    EXPECT_EQ(meta.current(), 50);
}
