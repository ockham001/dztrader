#include "test_util.h"

#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/subscriber_list.h>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::NamedSemaphore;
using dztrader::shm::SubscriberList;
using dztrader::shm::test::MB;
using dztrader::shm::test::cleanup_test_dir;
using dztrader::shm::test::test_shm_dir;
using dztrader::shm::test::unique_channel_name;

class SubscriberListTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;

    void SetUp() override
    {
        channel_name_ = unique_channel_name("dz_test_sublist");
        shm_dir_ = test_shm_dir(channel_name_);
    }

    void TearDown() override { cleanup_test_dir(shm_dir_); }

    ChannelConfig make_config()
    {
        return ChannelConfig{
            .channel_name = channel_name_,
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * MB,
            .page_size = 1 * MB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
    }
};

TEST_F(SubscriberListTest, DefaultConstruction)
{
    SubscriberList list;
}

TEST_F(SubscriberListTest, NotifyAllWithNoSubscribers)
{
    SubscriberList list;
    list.notify_all();
}

TEST_F(SubscriberListTest, SyncFromChannelMeta)
{
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    (void)meta->add_reader("sub_sync_1", /*pid=*/0);
    (void)meta->add_reader("sub_sync_2", /*pid=*/0);

    SubscriberList list;
    EXPECT_TRUE(list.sync_from_channel_meta(*meta));

    NamedSemaphore::remove("sub_sync_1");
    NamedSemaphore::remove("sub_sync_2");
}

TEST_F(SubscriberListTest, SyncFromChannelMetaEmpty)
{
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));

    SubscriberList list;
    EXPECT_TRUE(list.sync_from_channel_meta(*meta));
}

TEST_F(SubscriberListTest, NotifyAllWakesSubscribers)
{
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    (void)meta->add_reader("sub_notify_test", /*pid=*/0);

    SubscriberList list;
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));

    NamedSemaphore sem("sub_notify_test");

    std::atomic<bool> woke{false};
    std::thread waiter([&] {
        sem.wait();
        woke.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    list.notify_all();

    waiter.join();
    EXPECT_TRUE(woke.load());

    NamedSemaphore::remove("sub_notify_test");
}

TEST_F(SubscriberListTest, MoveConstruction)
{
    SubscriberList list1;
    SubscriberList list2 = std::move(list1);
}

TEST_F(SubscriberListTest, ReSyncUpdatesSubscribers)
{
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    (void)meta->add_reader("sub_first", /*pid=*/0);

    SubscriberList list;
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));

    (void)meta->add_reader("sub_second", /*pid=*/0);
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));

    NamedSemaphore::remove("sub_first");
    NamedSemaphore::remove("sub_second");
}

TEST_F(SubscriberListTest, SyncAfterClearPicksUpNewSubscribers)
{
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    (void)meta->add_reader("sub_old", /*pid=*/0);

    SubscriberList list;
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));

    meta->clear_readers();
    (void)meta->add_reader("sub_new", /*pid=*/0);
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));

    NamedSemaphore::remove("sub_new");
}
