#include "test_util.h"

#include <dztrader/error.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/struct.h>
#include <gtest/gtest.h>
#include <cstring>
#include <thread>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::Reader;
using dztrader::shm::NamedSemaphore;
using dztrader::shm::SingleWriter;
using dztrader::shm::test::cleanup_test_dir;
using dztrader::shm::test::MB;
using dztrader::shm::test::test_shm_dir;
using dztrader::shm::test::unique_channel_name;

class ReaderTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;

    void SetUp() override {
        channel_name_ = unique_channel_name("dz_test_reader");
        shm_dir_ = test_shm_dir(channel_name_);
    }

    void TearDown() override { cleanup_test_dir(shm_dir_); }

    ChannelConfig make_config(uint64_t page_size = 1 * MB) {
        return ChannelConfig{
            .channel_name = channel_name_,
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * MB,
            .page_size = page_size,
            .lock_memory = false,
            .prefetch_memory = false,
        };
    }

    static constexpr uint32_t test_data_size = 56;

    static void write_test_frame(SingleWriter& writer, DzFrameType frame_type) {
        std::byte* payload = writer.open_frame(frame_type, test_data_size);
        ASSERT_NE(payload, nullptr);
        std::memset(payload, 0xAB, test_data_size);
        writer.close_frame();
    }
};

TEST_F(ReaderTest, ReaderReturnsNullWhenNoNewData) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    Reader reader = Reader::create(meta, "stg.test");

    EXPECT_EQ(reader.next_frame(), nullptr);
}

TEST_F(ReaderTest, ReaderReadsWrittenFrame) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    write_test_frame(writer, DZ_FRAME_TICK);

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);

    const auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_TICK);
    EXPECT_EQ(hdr->frame_size, sizeof(DzFrameHeader) + test_data_size);
}

TEST_F(ReaderTest, ReaderReadsMultipleFrames) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    const int num_frames = 5;
    for (int i = 0; i < num_frames; i++) {
        write_test_frame(writer, DZ_FRAME_TICK);
    }

    int count = 0;
    while (auto* frame = reader.next_frame()) {
        const auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
        EXPECT_EQ(hdr->frame_type, DZ_FRAME_TICK);
        count++;
    }
    EXPECT_EQ(count, num_frames);
}

TEST_F(ReaderTest, ReaderOnlyReadsNewDataAfterCreation) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    write_test_frame(writer, DZ_FRAME_TICK);

    Reader reader = Reader::create(meta, "stg.test");
    EXPECT_EQ(reader.next_frame(), nullptr);

    write_test_frame(writer, DZ_FRAME_TICK);

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    const auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_TICK);
}

TEST_F(ReaderTest, ReaderCrossPageBoundary) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    int frames_written = 0;
    uint64_t page_size = 1 * MB;
    while (meta->next_write_pos()->load() <= page_size) {
        std::byte* payload = writer.open_frame(DZ_FRAME_TICK, test_data_size);
        if (!payload) break;
        std::memset(payload, 0xAB, test_data_size);
        writer.close_frame();
        frames_written++;
        if (frames_written > 20000) break;
    }

    int count = 0;
    while (auto* frame = reader.next_frame()) {
        const auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
        EXPECT_EQ(hdr->frame_type, DZ_FRAME_TICK);
        count++;
    }
    EXPECT_EQ(count, frames_written);
    EXPECT_GT(meta->next_write_pos()->load(), page_size);
}

TEST_F(ReaderTest, WaitAndNotify) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    Reader reader = Reader::create(meta, "stg.test");
    NamedSemaphore sem(channel_name_);

    std::thread notifier([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        sem.notify();
    });

    sem.wait();
    notifier.join();
}

TEST_F(ReaderTest, NextFrameBlockingRead) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    (void)meta->add_reader("test_sub", 0);
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");
    NamedSemaphore sem(channel_name_);

    std::thread delayed_write([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::byte* payload = writer.open_frame(DZ_FRAME_TICK, test_data_size);
        ASSERT_NE(payload, nullptr);
        std::memset(payload, 0xAB, test_data_size);
        writer.close_frame();
        sem.notify();
    });

    auto* frame = reader.next_frame();
    if (!frame) {
        sem.wait();
        frame = reader.next_frame();
    }
    ASSERT_NE(frame, nullptr);
    const auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_TICK);

    delayed_write.join();
}

TEST_F(ReaderTest, ReaderSkipsInitialInvalidFill) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    Reader reader = Reader::create(meta, "stg.test");

    write_test_frame(writer, DZ_FRAME_TICK);

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    const auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_NE(hdr->frame_type, DZ_FRAME_INVALID_FILL);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_TICK);
}

TEST_F(ReaderTest, MultipleSubscribersIndependentReadPosition) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    write_test_frame(writer, DZ_FRAME_TICK);

    Reader late_reader = Reader::create(meta, "stg.test");
    EXPECT_EQ(late_reader.next_frame(), nullptr);

    write_test_frame(writer, DZ_FRAME_TICK);
    auto* frame = late_reader.next_frame();
    ASSERT_NE(frame, nullptr);
    const auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_TICK);
}

TEST_F(ReaderTest, CrossPageInvalidFillSkipped) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    int frames_written = 0;
    uint64_t page_size = 1 * MB;
    while (meta->next_write_pos()->load() <= page_size) {
        std::byte* payload = writer.open_frame(DZ_FRAME_TICK, test_data_size);
        if (!payload) break;
        std::memset(payload, 0xAB, test_data_size);
        writer.close_frame();
        frames_written++;
        if (frames_written > 20000) break;
    }

    int valid_frames = 0;
    while (auto* frame = reader.next_frame()) {
        const auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
        EXPECT_NE(hdr->frame_type, DZ_FRAME_INVALID_FILL);
        valid_frames++;
    }
    EXPECT_EQ(valid_frames, frames_written);
}

TEST_F(ReaderTest, ReadPositionInitializesAtWritePosition) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    write_test_frame(writer, DZ_FRAME_TICK);

    uint64_t write_pos = meta->next_write_pos()->load();
    Reader reader = Reader::create(meta, "stg.test");
    EXPECT_EQ(reader.read_position(), write_pos);
}

TEST_F(ReaderTest, ReadPositionAdvancesAfterRead) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    write_test_frame(writer, DZ_FRAME_TICK);

    uint64_t pos_before = reader.read_position();
    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    EXPECT_GT(reader.read_position(), pos_before);
}

TEST_F(ReaderTest, CurrentPageIdUpdatesAfterCrossPage) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    EXPECT_EQ(reader.current_page_id(), 0u);

    int frames_written = 0;
    uint64_t page_size = 1 * MB;
    while (meta->next_write_pos()->load() <= page_size) {
        std::byte* payload = writer.open_frame(DZ_FRAME_TICK, test_data_size);
        if (!payload) break;
        std::memset(payload, 0xAB, test_data_size);
        writer.close_frame();
        frames_written++;
        if (frames_written > 20000) break;
    }

    while (auto* frame = reader.next_frame()) {
        (void)frame;
    }
    EXPECT_GT(reader.current_page_id(), 0u);
}

TEST_F(ReaderTest, NextFrameRawPointerParseable) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    constexpr uint32_t data_size = 56;
    std::byte* payload = writer.open_frame(DZ_FRAME_TICK, data_size);
    ASSERT_NE(payload, nullptr);
    std::memset(payload, 0xEF, data_size);
    writer.close_frame();

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);

    const auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_TICK);
    EXPECT_EQ(hdr->frame_size, sizeof(DzFrameHeader) + data_size);

    auto* payload_ptr = frame + sizeof(DzFrameHeader);
    for (uint32_t i = 0; i < data_size; i++) {
        EXPECT_EQ(payload_ptr[i], static_cast<std::byte>(0xEF));
    }
}

TEST_F(ReaderTest, ReadPositionUnchangedWhenNoNewData) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    Reader reader = Reader::create(meta, "stg.test");

    uint64_t pos = reader.read_position();
    EXPECT_EQ(reader.next_frame(), nullptr);
    EXPECT_EQ(reader.read_position(), pos);
}

TEST_F(ReaderTest, WaitForTimeout) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    Reader reader = Reader::create(meta, "stg.test");
    NamedSemaphore sem(channel_name_);

    bool rc = sem.wait_for(100);
    EXPECT_FALSE(rc);
}

TEST_F(ReaderTest, WaitForNotified) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    Reader reader = Reader::create(meta, "stg.test");
    NamedSemaphore sem(channel_name_);

    std::thread notifier([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        sem.notify();
    });

    EXPECT_TRUE(sem.wait_for(5000));
    notifier.join();
}

TEST(ReaderReleaseTest, ReleaseOldPagesUpdatesIndex) {
    using namespace dztrader::shm;
    using namespace dztrader::shm::test;
    auto channel = unique_channel_name("dz_test_reader_rel");
    auto dir = test_shm_dir(channel);
    ChannelConfig config{.channel_name=channel, .shm_dir=dir, .meta_file_size=4*MB,
                         .page_size=1*MB, .lock_memory=false, .prefetch_memory=false};
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(config));
    meta->clear_readers();
    (void)meta->add_reader("stg.t", 1234);

    // 先推进 write position 到 page 2，使 reader 起始于 page 3。
    // 只读 reader (P2) 不创建页文件: 页 3 必须先存在 (写者推进 nwp 前必已创建,
    // 此处直接模拟该前提)。
    create_page_file(meta->page_dir(), 3, 1 * MB);
    meta->next_write_pos()->store(3 * 1 * MB, boost::memory_order_release);
    auto reader = Reader::create(meta, "stg.t");
    reader.prefetch_pages(3);
    // 此时 reader index 仍为 0（未 release）
    EXPECT_EQ(meta->min_reader_page_index(), 0u);
    reader.release_old_pages();
    // release 后 index 抬高到 current_page_id
    EXPECT_GE(meta->min_reader_page_index(), 1u);
    cleanup_test_dir(dir);
}
