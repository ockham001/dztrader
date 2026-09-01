#include "test_util.h"

#include <cstring>
#include <dztrader/core/core_data_type.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/struct.h>
#include <gtest/gtest.h>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::Reader;
using dztrader::shm::SingleWriter;
using dztrader::shm::test::cleanup_test_dir;
using dztrader::shm::test::MB;
using dztrader::shm::test::test_shm_dir;
using dztrader::shm::test::unique_channel_name;

class WriteFrameTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;

    void SetUp() override {
        channel_name_ = unique_channel_name("dz_test_write_frame");
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

    static const std::byte* to_bytes(const char* str) {
        return reinterpret_cast<const std::byte*>(str);
    }

    static const char* to_chars(const std::byte* ptr) { return reinterpret_cast<const char*>(ptr); }
};

TEST_F(WriteFrameTest, WriteFrameAlignedStruct) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    DzShmPreload preload{};
    preload.bytes = 4096;
    preload.pages = 2;
    ASSERT_TRUE(writer.write_frame(DZ_FRAME_PRELOAD_EVENT_SHM, preload));

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_PRELOAD_EVENT_SHM);
    EXPECT_EQ(hdr->frame_size, sizeof(DzFrameHeader) + sizeof(DzShmPreload));

    auto* payload = reinterpret_cast<const DzShmPreload*>(static_cast<const std::byte*>(frame) +
                                                           sizeof(DzFrameHeader));
    EXPECT_EQ(payload->bytes, 4096u);
    EXPECT_EQ(payload->pages, 2u);
}

TEST_F(WriteFrameTest, WriteFrameMultipleFrames) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    for (int i = 0; i < 5; ++i) {
        DzShmPreload preload{};
        preload.bytes = 4096;
        preload.pages = 2;
        ASSERT_TRUE(writer.write_frame(DZ_FRAME_PRELOAD_EVENT_SHM, preload));
    }

    for (int i = 0; i < 5; ++i) {
        auto* frame = reader.next_frame();
        ASSERT_NE(frame, nullptr);
        auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
        EXPECT_EQ(hdr->frame_type, DZ_FRAME_PRELOAD_EVENT_SHM);
    }
    EXPECT_EQ(reader.next_frame(), nullptr);
}

TEST_F(WriteFrameTest, WriteExtFrameBinaryAligned) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    const std::byte data[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
                              std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
    ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, "inst001", data, sizeof(data)));

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_UI_INPUT);
    EXPECT_EQ(hdr->frame_size, sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader) + sizeof(data));

    auto* ext_hdr = reinterpret_cast<const DzExtInstFrameHeader*>(static_cast<const std::byte*>(frame) +
                                                              sizeof(DzFrameHeader));
    EXPECT_STREQ(ext_hdr->instance_id, "inst001");
    EXPECT_EQ(ext_hdr->data_size, sizeof(data));

    auto* payload =
        static_cast<const std::byte*>(frame) + sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader);
    for (uint32_t i = 0; i < sizeof(data); ++i) {
        EXPECT_EQ(payload[i], data[i]);
    }
}

TEST_F(WriteFrameTest, WriteExtFrameBinaryUnaligned) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    const std::byte data[] = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
    ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, "inst002", data, sizeof(data)));

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_UI_INPUT);

    uint32_t padded_size = (sizeof(data) + 7u) & ~7u;
    EXPECT_EQ(hdr->frame_size, sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader) + padded_size);

    auto* ext_hdr = reinterpret_cast<const DzExtInstFrameHeader*>(static_cast<const std::byte*>(frame) +
                                                              sizeof(DzFrameHeader));
    EXPECT_STREQ(ext_hdr->instance_id, "inst002");
    EXPECT_EQ(ext_hdr->data_size, sizeof(data));

    auto* payload =
        static_cast<const std::byte*>(frame) + sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader);
    for (uint32_t i = 0; i < sizeof(data); ++i) {
        EXPECT_EQ(payload[i], data[i]);
    }
    for (uint32_t i = sizeof(data); i < padded_size; ++i) {
        EXPECT_EQ(payload[i], std::byte{0});
    }
}

TEST_F(WriteFrameTest, WriteExtFrameBinaryZeroSize) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, "inst003", nullptr, 0));

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_UI_INPUT);
    EXPECT_EQ(hdr->frame_size, sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader));

    auto* ext_hdr = reinterpret_cast<const DzExtInstFrameHeader*>(static_cast<const std::byte*>(frame) +
                                                              sizeof(DzFrameHeader));
    EXPECT_STREQ(ext_hdr->instance_id, "inst003");
    EXPECT_EQ(ext_hdr->data_size, 0u);
}

TEST_F(WriteFrameTest, WriteExtFrameStringAligned) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    const char* str = "hello!";
    uint32_t str_len = 6;
    ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, "inst004", to_bytes(str), str_len));

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_UI_INPUT);

    uint32_t padded_size = (str_len + 7u) & ~7u;
    EXPECT_EQ(hdr->frame_size, sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader) + padded_size);

    auto* ext_hdr = reinterpret_cast<const DzExtInstFrameHeader*>(static_cast<const std::byte*>(frame) +
                                                              sizeof(DzFrameHeader));
    EXPECT_STREQ(ext_hdr->instance_id, "inst004");
    EXPECT_EQ(ext_hdr->data_size, str_len);

    auto* payload =
        static_cast<const std::byte*>(frame) + sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader);
    EXPECT_EQ(memcmp(payload, str, str_len), 0);
}

TEST_F(WriteFrameTest, WriteExtFrameStringUnaligned) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    const char* str = "abc";
    uint32_t str_len = 3;
    ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, "inst005", to_bytes(str), str_len));

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);

    uint32_t padded_size = (str_len + 7u) & ~7u;

    auto* ext_hdr = reinterpret_cast<const DzExtInstFrameHeader*>(static_cast<const std::byte*>(frame) +
                                                              sizeof(DzFrameHeader));
    EXPECT_EQ(ext_hdr->data_size, str_len);

    auto* payload =
        static_cast<const std::byte*>(frame) + sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader);
    EXPECT_EQ(memcmp(payload, str, str_len), 0);
    for (uint32_t i = str_len; i < padded_size; ++i) {
        EXPECT_EQ(payload[i], std::byte{0});
    }
}

TEST_F(WriteFrameTest, WriteExtFrameStdString) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    std::string msg = "test message from std::string";
    ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, "inst006", to_bytes(msg.c_str()),
                                       static_cast<uint32_t>(msg.size())));

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);

    auto* ext_hdr = reinterpret_cast<const DzExtInstFrameHeader*>(static_cast<const std::byte*>(frame) +
                                                              sizeof(DzFrameHeader));
    EXPECT_STREQ(ext_hdr->instance_id, "inst006");
    EXPECT_EQ(ext_hdr->data_size, static_cast<uint32_t>(msg.size()));

    auto* payload =
        static_cast<const std::byte*>(frame) + sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader);
    EXPECT_EQ(memcmp(payload, msg.c_str(), msg.size()), 0);
}

TEST_F(WriteFrameTest, WriteExtFrameInstanceIdTruncation) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    std::string long_id(100, 'X');
    const std::byte dummy = std::byte{0x42};
    ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, long_id.c_str(), &dummy, 1));

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);

    auto* ext_hdr = reinterpret_cast<const DzExtInstFrameHeader*>(static_cast<const std::byte*>(frame) +
                                                              sizeof(DzFrameHeader));
    EXPECT_EQ(std::strlen(ext_hdr->instance_id), sizeof(DzStrategyId) - 1);
    EXPECT_EQ(ext_hdr->instance_id[sizeof(DzStrategyId) - 1], '\0');
}

TEST_F(WriteFrameTest, WriteExtFrameMixedTypes) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    DzShmPreload preload{};
    preload.bytes = 4096;
    preload.pages = 2;
    ASSERT_TRUE(writer.write_frame(DZ_FRAME_PRELOAD_EVENT_SHM, preload));

    const std::byte bin_data[] = {std::byte{0xDE}, std::byte{0xAD}};
    ASSERT_TRUE(
        writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, "inst_bin", bin_data, sizeof(bin_data)));

    const char* str = "payload text";
    ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, "inst_str", to_bytes(str), 12));

    auto* f1 = reader.next_frame();
    ASSERT_NE(f1, nullptr);
    auto* hdr1 = reinterpret_cast<const DzFrameHeader*>(f1);
    EXPECT_EQ(hdr1->frame_type, DZ_FRAME_PRELOAD_EVENT_SHM);

    auto* f2 = reader.next_frame();
    ASSERT_NE(f2, nullptr);
    auto* hdr2 = reinterpret_cast<const DzFrameHeader*>(f2);
    EXPECT_EQ(hdr2->frame_type, DZ_FRAME_UI_INPUT);
    auto* ext2 = reinterpret_cast<const DzExtInstFrameHeader*>(static_cast<const std::byte*>(f2) +
                                                           sizeof(DzFrameHeader));
    EXPECT_STREQ(ext2->instance_id, "inst_bin");

    auto* f3 = reader.next_frame();
    ASSERT_NE(f3, nullptr);
    auto* hdr3 = reinterpret_cast<const DzFrameHeader*>(f3);
    EXPECT_EQ(hdr3->frame_type, DZ_FRAME_UI_INPUT);
    auto* ext3 = reinterpret_cast<const DzExtInstFrameHeader*>(static_cast<const std::byte*>(f3) +
                                                           sizeof(DzFrameHeader));
    EXPECT_STREQ(ext3->instance_id, "inst_str");
    auto* payload3 =
        static_cast<const std::byte*>(f3) + sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader);
    EXPECT_EQ(memcmp(payload3, str, 12), 0);

    EXPECT_EQ(reader.next_frame(), nullptr);
}

TEST_F(WriteFrameTest, WriteExtFrameReservedZeroed) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    const std::byte dummy = std::byte{0x01};
    ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, "inst_res", &dummy, sizeof(dummy)));

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    auto* ext_hdr = reinterpret_cast<const DzExtInstFrameHeader*>(static_cast<const std::byte*>(frame) +
                                                              sizeof(DzFrameHeader));
    for (char c : ext_hdr->reserved) {
        EXPECT_EQ(c, '\0');
    }
}
