#include "test_fixture.h"

#include <atomic>
#include <chrono>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/subscriber_list.h>
#include <gtest/gtest.h>
#include <thread>
#include <utility>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::NamedSemaphore;
using dztrader::shm::SubscriberList;
using dztrader::shm::test::cleanup_channel;
using dztrader::shm::test::MB;

class SubscriberListTest : public dztrader::shm::test::ShmTestBase {
protected:
    [[nodiscard]] std::string channel_prefix() const override { return "dz_test_sublist"; }

    void TearDown() override {
        cleanup_channel(channel_name_, shm_dir_,
                        {"sub_sync_1", "sub_sync_2", "sub_notify_test", "sub_first", "sub_second",
                         "sub_old", "sub_new", channel_name_ + ".late", channel_name_ + ".move"});
    }

    ChannelConfig make_config() {
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

TEST_F(SubscriberListTest, SyncFromChannelMetaNotifiesRegisteredReaders) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    (void)meta->add_reader("sub_sync_1", /*pid=*/0);
    (void)meta->add_reader("sub_sync_2", /*pid=*/0);

    SubscriberList list;
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));
    list.notify_all();

    NamedSemaphore sem1("sub_sync_1");
    NamedSemaphore sem2("sub_sync_2");
    EXPECT_TRUE(sem1.wait_for(1000));
    EXPECT_TRUE(sem2.wait_for(1000));
}

TEST_F(SubscriberListTest, SyncFromChannelMetaEmptyThenPicksUpLateReader) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));

    SubscriberList list;
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));

    const auto late_name = channel_name_ + ".late";
    (void)meta->add_reader(late_name, /*pid=*/0);
    list.notify_all();
    NamedSemaphore late(late_name);
    EXPECT_FALSE(late.wait_for(0));

    ASSERT_TRUE(list.sync_from_channel_meta(*meta));
    list.notify_all();
    EXPECT_TRUE(late.wait_for(1000));
}

TEST_F(SubscriberListTest, NotifyAllWakesSubscribers) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    (void)meta->add_reader("sub_notify_test", /*pid=*/0);

    SubscriberList list;
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));

    NamedSemaphore sem("sub_notify_test");
    std::atomic<bool> woke{false};
    std::thread waiter([&] { woke.store(sem.wait_for(5000)); });

    list.notify_all();

    waiter.join();
    EXPECT_TRUE(woke.load());
}

TEST_F(SubscriberListTest, MoveConstructionPreservesSubscribers) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    const auto move_name = channel_name_ + ".move";
    (void)meta->add_reader(move_name, /*pid=*/0);

    SubscriberList list1;
    ASSERT_TRUE(list1.sync_from_channel_meta(*meta));

    SubscriberList list2 = std::move(list1);
    list2.notify_all();

    NamedSemaphore sem(move_name);
    EXPECT_TRUE(sem.wait_for(1000));
}

TEST_F(SubscriberListTest, ReSyncUpdatesSubscribers) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    (void)meta->add_reader("sub_first", /*pid=*/0);

    SubscriberList list;
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));

    (void)meta->add_reader("sub_second", /*pid=*/0);
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));

    list.notify_all();
    NamedSemaphore first("sub_first");
    NamedSemaphore second("sub_second");
    EXPECT_TRUE(first.wait_for(1000));
    EXPECT_TRUE(second.wait_for(1000));

    meta->remove_reader("sub_first");
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));
    list.notify_all();
    EXPECT_FALSE(first.wait_for(0));
    EXPECT_TRUE(second.wait_for(1000));
}

TEST_F(SubscriberListTest, SyncAfterClearPicksUpNewSubscribers) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    (void)meta->add_reader("sub_old", /*pid=*/0);

    SubscriberList list;
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));

    meta->clear_readers();
    (void)meta->add_reader("sub_new", /*pid=*/0);
    ASSERT_TRUE(list.sync_from_channel_meta(*meta));

    list.notify_all();
    NamedSemaphore old_sem("sub_old");
    NamedSemaphore new_sem("sub_new");
    EXPECT_FALSE(old_sem.wait_for(0));
    EXPECT_TRUE(new_sem.wait_for(1000));
}
