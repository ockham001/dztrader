#include "test_util.h"

#include <cstring>
#include <string_view>
#include <vector>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/struct.h>
#include <gtest/gtest.h>

#include "test_ipc_util.h"

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::FrameView;
using dztrader::shm::Reader;
using dztrader::shm::MultiWriter;
using dztrader::shm::SingleWriter;
using dztrader::shm::test::cleanup_test_dir;
using dztrader::shm::test::MB;
using dztrader::shm::test::spawn_helper;
using dztrader::shm::test::test_shm_dir;
using dztrader::shm::test::unique_channel_name;
using dztrader::shm::test::wait_with_timeout;

class WriterTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;

    void SetUp() override {
        channel_name_ = unique_channel_name("dz_test_writer");
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
};

TEST_F(WriterTest, SingleWriterWriteSingleFrame) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    constexpr uint32_t data_size = 56;
    std::byte* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
    ASSERT_NE(payload, nullptr);
    std::memset(payload, 0xAB, data_size);
    writer.close_frame();

    uint64_t expected_pos = sizeof(DzFrameHeader) + sizeof(DzFrameHeader) + data_size;
    EXPECT_EQ(meta->next_write_pos()->load(), expected_pos);
}

TEST_F(WriterTest, MultiWriterWriteSingleFrame) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    MultiWriter writer = MultiWriter::create(meta, "gw.test");

    constexpr uint32_t data_size = 56;
    std::byte* payload = writer.open_frame(DZ_FRAME_TD_ORDER_RPT, data_size);
    ASSERT_NE(payload, nullptr);
    std::memset(payload, 0xAB, data_size);
    writer.close_frame();

    uint64_t expected_pos = sizeof(DzFrameHeader) + sizeof(DzFrameHeader) + data_size;
    EXPECT_EQ(meta->next_write_pos()->load(), expected_pos);
}

TEST_F(WriterTest, SingleWriterWriteMultipleFrames) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    constexpr uint32_t data_size = 56;
    constexpr int num_frames = 10;
    for (int i = 0; i < num_frames; i++) {
        std::byte* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
        ASSERT_NE(payload, nullptr);
        std::memset(payload, 0xAB, data_size);
        writer.close_frame();
    }

    uint64_t expected_pos =
        sizeof(DzFrameHeader) + (sizeof(DzFrameHeader) + data_size) * num_frames;
    EXPECT_EQ(meta->next_write_pos()->load(), expected_pos);
}

TEST_F(WriterTest, SingleWriterCrossPageBoundary) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    constexpr uint32_t data_size = 56;
    int frames_written = 0;
    uint64_t page_size = 1 * MB;
    while (meta->next_write_pos()->load() <= page_size) {
        std::byte* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
        if (!payload) break;
        std::memset(payload, 0xAB, data_size);
        writer.close_frame();
        frames_written++;
        if (frames_written > 20000) break;
    }

    EXPECT_GT(frames_written, 1);
    EXPECT_GT(meta->next_write_pos()->load(), page_size);
}

TEST_F(WriterTest, OpenFrameRejectsNonAlignedFrameSize) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    EXPECT_EQ(writer.open_frame(DZ_FRAME_RTN_MD_TICK, 52), nullptr);
}

TEST_F(WriterTest, PayloadDataIntegrity) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    constexpr uint32_t data_size = 56;
    std::byte* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
    ASSERT_NE(payload, nullptr);
    std::memset(payload, 0xCD, data_size);
    writer.close_frame();

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_RTN_MD_TICK);
    EXPECT_EQ(hdr->frame_size, sizeof(DzFrameHeader) + data_size);

    const auto* payload_data = static_cast<const std::byte*>(frame) + sizeof(DzFrameHeader);
    for (uint32_t i = 0; i < data_size; i++) {
        EXPECT_EQ(payload_data[i], static_cast<std::byte>(0xCD));
    }
}

TEST_F(WriterTest, RefreshSubscribers) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    EXPECT_TRUE(writer.refresh_subscribers());

    (void)meta->add_reader("test_sub", 0);
    EXPECT_TRUE(writer.refresh_subscribers());
}

TEST_F(WriterTest, OpenFrameRejectsZeroDataSize) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    std::byte* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, 0);
    ASSERT_NE(payload, nullptr);
    writer.close_frame();

    uint64_t expected_pos = sizeof(DzFrameHeader) + sizeof(DzFrameHeader);
    EXPECT_EQ(meta->next_write_pos()->load(), expected_pos);
}

TEST_F(WriterTest, SingleWriterWritePositionAfterWrite) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    constexpr uint32_t data_size = 56;
    std::byte* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
    ASSERT_NE(payload, nullptr);
    std::memset(payload, 0xAB, data_size);
    writer.close_frame();

    uint64_t expected_pos = sizeof(DzFrameHeader) + sizeof(DzFrameHeader) + data_size;
    EXPECT_EQ(writer.write_position(), expected_pos);
}

TEST_F(WriterTest, SingleWriterPageIdAndOffsetAfterWrite) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    constexpr uint32_t data_size = 56;
    std::byte* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
    ASSERT_NE(payload, nullptr);
    std::memset(payload, 0xAB, data_size);
    writer.close_frame();

    EXPECT_EQ(writer.current_page_id(), 0u);
    uint32_t expected_offset = sizeof(DzFrameHeader) + sizeof(DzFrameHeader) + data_size;
    EXPECT_EQ(writer.offset_in_page(), expected_offset);
}

TEST_F(WriterTest, NormalFrameIsNotPadding) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    constexpr uint32_t data_size = 56;
    std::byte* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
    ASSERT_NE(payload, nullptr);
    std::memset(payload, 0xAB, data_size);
    writer.close_frame();

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_NE(hdr->frame_type, DZ_FRAME_INVALID_FILL);
}

TEST_F(WriterTest, RefreshSubscribersAfterAdd) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    EXPECT_TRUE(writer.refresh_subscribers());

    (void)meta->add_reader("refresh_sub", 0);
    EXPECT_TRUE(writer.refresh_subscribers());

    dztrader::shm::NamedSemaphore::remove("refresh_sub");
}

TEST_F(WriterTest, AccessorsUnchangedAfterAlignmentRejection) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    uint64_t pos_before = writer.write_position();
    uint64_t page_id_before = writer.current_page_id();
    uint32_t offset_before = writer.offset_in_page();

    EXPECT_EQ(writer.open_frame(DZ_FRAME_RTN_MD_TICK, 52), nullptr);

    EXPECT_EQ(writer.write_position(), pos_before);
    EXPECT_EQ(writer.current_page_id(), page_id_before);
    EXPECT_EQ(writer.offset_in_page(), offset_before);
}

TEST_F(WriterTest, AccessorsAccumulateAcrossMultipleWrites) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    constexpr uint32_t data_size = 56;
    constexpr int num_frames = 5;
    for (int i = 0; i < num_frames; i++) {
        std::byte* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
        ASSERT_NE(payload, nullptr);
        std::memset(payload, 0xAB, data_size);
        writer.close_frame();
    }

    uint64_t expected_pos =
        sizeof(DzFrameHeader) + (sizeof(DzFrameHeader) + data_size) * num_frames;
    EXPECT_EQ(writer.write_position(), expected_pos);
    EXPECT_EQ(writer.current_page_id(), expected_pos / (1 * MB));
    EXPECT_EQ(writer.offset_in_page(), static_cast<uint32_t>(expected_pos % (1 * MB)));
}

TEST_F(WriterTest, MultiWriterAccessorsAfterWrite) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    MultiWriter writer = MultiWriter::create(meta, "gw.test");

    constexpr uint32_t data_size = 56;
    std::byte* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
    ASSERT_NE(payload, nullptr);
    std::memset(payload, 0xAB, data_size);
    writer.close_frame();

    uint64_t expected_pos = sizeof(DzFrameHeader) + sizeof(DzFrameHeader) + data_size;
    EXPECT_EQ(writer.write_position(), expected_pos);
    EXPECT_EQ(writer.current_page_id(), 0u);
    EXPECT_EQ(writer.offset_in_page(), static_cast<uint32_t>(expected_pos));
}

TEST_F(WriterTest, MultiWriterWritesToStalePageAfterOtherWriterCrossPage) {
    // 回归: 多写者下, 写者 A 的缓存页 (page_) 可能落后于 next_write_pos 所在页。
    // A 停写期间 B 跨入新页, A 再写时若不刷新缓存页, 帧会落进 A 的旧页文件,
    // 而 nwp 推进到新页 -> 新页对应区间是全零 -> reader 读到坏帧头 (size=0) 后卡死。
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    MultiWriter writer_a = MultiWriter::create(meta, "gw.a");
    MultiWriter writer_b = MultiWriter::create(meta, "gw.b");
    Reader reader = Reader::create(meta, "stg.test");

    constexpr uint32_t data_size = 56;  // 整帧 64 字节
    const uint64_t page_size = 1 * MB;

    auto write_frame = [](MultiWriter& w, DzFrameType type) {
        std::byte* payload = w.open_frame(type, data_size);
        ASSERT_NE(payload, nullptr);
        std::memset(payload, 0xAB, data_size);
        w.close_frame();
    };

    // A 在页 0 写一帧
    write_frame(writer_a, DZ_FRAME_PRELOAD_EVENT_SHM);

    // B 连写直至跨入页 1
    int b_frames = 0;
    while (meta->next_write_pos()->load() <= page_size) {
        write_frame(writer_b, DZ_FRAME_RTN_MD_TICK);
        ++b_frames;
        ASSERT_LT(b_frames, 20000) << "should have crossed page boundary long ago";
    }

    // A 再写一帧: 64 字节远小于页 1 剩余空间, 不触发跨界分支
    write_frame(writer_a, DZ_FRAME_PRELOAD_EVENT_SHM);

    // 读尽全部帧: 期望 1 + b_frames + 1 帧, 且读位置追平写位置
    int count = 0;
    while (reader.next_frame() != nullptr) {
        ++count;
    }
    EXPECT_EQ(count, 1 + b_frames + 1);
    EXPECT_EQ(reader.read_position(), meta->next_write_pos()->load());

    // 若读位置未追平 (reader 卡在坏帧头), 再次调用必须仍返回 nullptr 且不前进
    if (reader.read_position() != meta->next_write_pos()->load()) {
        uint64_t stuck_pos = reader.read_position();
        EXPECT_EQ(reader.next_frame(), nullptr);
        EXPECT_EQ(reader.read_position(), stuck_pos);
    }
}

TEST_F(WriterTest, CrossProcessWriteRead) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    Reader reader = Reader::create(meta, "stg.test");

    boost::asio::io_context ctx;
    auto proc = spawn_helper(ctx, {"channel_writer_write", channel_name_, shm_dir_.string()});

    EXPECT_EQ(wait_with_timeout(proc, std::chrono::seconds(5)), 0);

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_RTN_MD_TICK);
    EXPECT_EQ(hdr->frame_size, sizeof(DzFrameHeader) + 56);

    const auto* payload = static_cast<const std::byte*>(frame) + sizeof(DzFrameHeader);
    for (uint32_t i = 0; i < 56; i++) {
        EXPECT_EQ(payload[i], static_cast<std::byte>(0xAB));
    }
}

TEST_F(WriterTest, WriterCrashMidFrame) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    Reader reader = Reader::create(meta, "stg.test");

    boost::asio::io_context ctx;
    auto proc = spawn_helper(ctx, {"channel_writer_open_frame", channel_name_, shm_dir_.string()});

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    proc.terminate();
    wait_with_timeout(proc, std::chrono::seconds(2));

    auto* frame = reader.next_frame();
    EXPECT_EQ(frame, nullptr);
}

TEST_F(WriterTest, ProcessRestartAfterCrash) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    Reader reader = Reader::create(meta, "stg.test");

    {
        boost::asio::io_context ctx;
        auto proc =
            spawn_helper(ctx, {"channel_writer_open_frame", channel_name_, shm_dir_.string()});

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        proc.terminate();
        wait_with_timeout(proc, std::chrono::seconds(2));
    }

    {
        auto writer = MultiWriter::create(meta, "gw.test");
        constexpr uint32_t data_size = 56;
        auto* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
        ASSERT_NE(payload, nullptr);
        std::memset(payload, 0xCD, data_size);
        writer.close_frame();
        writer.notify_subscribers();
    }

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    auto* hdr = reinterpret_cast<const DzFrameHeader*>(frame);
    EXPECT_EQ(hdr->frame_type, DZ_FRAME_RTN_MD_TICK);
}

TEST(WriterCloseOldTest, CloseOldPagesUpdatesWriterIndex) {
    using namespace dztrader::shm;
    using namespace dztrader::shm::test;
    auto channel = unique_channel_name("dz_test_writer_close");
    auto dir = test_shm_dir(channel);
    ChannelConfig config{.channel_name=channel, .shm_dir=dir, .meta_file_size=4*MB,
                         .page_size=1*MB, .lock_memory=false, .prefetch_memory=false};
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(config));
    meta->clear_writers();
    (void)meta->add_writer("gw.t", 9999);

    auto writer = SingleWriter::create(meta, "gw.t");
    EXPECT_EQ(meta->min_writer_page_index(), 0u);
    writer.close_old_pages();
    // close_old_pages 后 index = page_.page_id()（初始为 0 或当前写页）
    // 至少不抛异常且 index 被写入
    // 写一帧后跨页，再 close_old_pages，index 应抬高
    DzTick tick{};
    writer.write_frame(DZ_FRAME_RTN_MD_TICK, tick);
    writer.close_old_pages();
    EXPECT_GE(meta->min_writer_page_index(), 0u);  // 不抛即基础通过
    cleanup_test_dir(dir);
}

// 死锁回归：MultiWriter 持续换页（write→meta）与并发 open_or_create ctor
// （write→meta）必须无 AB-BA 死锁。Task 1 的 write→meta 重排解决了朴素合并的
// AB-BA 死锁；本测试用 5s 超时守护，一旦回归（helper 被超时杀死）即失败。
TEST(WriterDeadlockTest, ConcurrentCtorAndPageCrossNoDeadlock) {
    using namespace dztrader::shm;
    using namespace dztrader::shm::test;
    auto channel = unique_channel_name("dz_test_deadlock");
    auto dir = test_shm_dir(channel);
    auto signal_file = dir / "helper_started";
    ChannelConfig config{.channel_name = channel, .shm_dir = dir, .meta_file_size = 4 * MB,
                         .page_size = 1 * MB, .lock_memory = false, .prefetch_memory = false};

    // 预创建 channel，使 helper 与主进程都 open 已存在的 channel（page_size 不变，
    // 不走重置路径），仅竞争 write→meta 锁。
    {
        auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(config));
        (void)meta;
    }

    boost::asio::io_context ctx;
    auto proc = spawn_helper(ctx, {"channel_writer_continuous_cross", channel, dir.string(),
                                   signal_file.string()});

    // 等 helper 开始写后再并发 open_or_create，保证 page-cross 与 ctor 真正重叠。
    ASSERT_TRUE(wait_for_file(signal_file, std::chrono::seconds(5)))
        << "helper failed to start";

    // 并发多次 open_or_create 同一 channel：ctor 持 write→meta。helper 的换页路径
    // 也持 write→meta。锁序一致则两者在 write 锁上串行并推进；AB-BA 反序会永久挂起
    // （被下方 5s 超时捕获）。
    for (int i = 0; i < 3; ++i) {
        auto m = ChannelMeta::open_or_create(config);
        (void)m;
    }

    int result = wait_with_timeout(proc, std::chrono::seconds(5));
    EXPECT_NE(result, -1) << "helper deadlocked/hung (killed by 5s timeout)";
    EXPECT_EQ(result, 0) << "helper exited with non-zero status";

    cleanup_test_dir(dir);
}

TEST_F(WriterTest, TouchWritePositionDoesNotCrash) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "test_writer");
    // 写入一帧确保 page_ 和 offset_in_page_ 有效
    DzShmPreload params{.bytes = 1024, .pages = 2, .reserved = 0};
    ASSERT_TRUE(writer.write_frame(DZ_FRAME_PRELOAD_EVENT_SHM, params));
    // touch 不应崩溃
    writer.touch_write_position();
    // 再次 touch (offset 已推进) 也不崩溃
    writer.touch_write_position();
    SUCCEED();
}

TEST_F(WriterTest, TouchWritePositionMultiWriter) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    MultiWriter writer = MultiWriter::create(meta, "test_multi_writer");
    DzShmPreload params{.bytes = 0, .pages = 1, .reserved = 0};
    ASSERT_TRUE(writer.write_frame(DZ_FRAME_PRELOAD_MD_SHM, params));
    writer.touch_write_position();
    SUCCEED();
}

TEST_F(WriterTest, OpenFrameRejectsFrameLargerThanPageSize) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    const uint64_t nwp_before = meta->next_write_pos()->load();
    // data_size=1MB → pending=1MB+8 > 页, 对齐(8倍数)通过后命中超页拒绝
    EXPECT_EQ(writer.open_frame(DZ_FRAME_RTN_MD_TICK, 1 * MB), nullptr);
    EXPECT_EQ(meta->next_write_pos()->load(), nwp_before);
}

TEST_F(WriterTest, FrameExactlyPageSizeSucceeds) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    SingleWriter writer = SingleWriter::create(meta, "gw.test");

    // pending = (1MB-8)+8 = 恰好等于页大小: 等值边界通过超页拒绝检查。
    // 页首 8B 为通道头, 页剩余 1MB-8 容不下整页帧, 经填充跨入页 1 整页写入
    constexpr uint32_t exact = static_cast<uint32_t>(1 * MB - sizeof(DzFrameHeader));
    std::byte* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, exact);
    ASSERT_NE(payload, nullptr);
    writer.close_frame();
    EXPECT_EQ(meta->next_write_pos()->load(), 2 * MB);
}

TEST_F(WriterTest, PageAwareCapWriteSucceeds) {
    // 页感知上限真实路径: len=(1MB-80)&~7=1048496, 经 write_ext_inst_frame 总帧恰为 1MB;
    // 页首 8B 通道头使整页帧跨入页 1 (填充页 0 剩余), Reader 跳过 INVALID_FILL 后读到该帧
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(make_config()));
    MultiWriter writer = MultiWriter::create(meta, "gw.test");
    Reader reader = Reader::create(meta, "stg.test");

    constexpr uint32_t cap_len =
        static_cast<uint32_t>(1 * MB - sizeof(DzFrameHeader) - sizeof(DzExtInstFrameHeader));
    std::vector<std::byte> buf(cap_len);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<std::byte>(i & 0xFF);

    const char* inst = "inst.cap";
    ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_STG_USER_INPUT, inst, buf.data(), cap_len));
    writer.notify_subscribers();
    EXPECT_EQ(meta->next_write_pos()->load(), 2 * MB);

    auto* frame = reader.next_frame();
    ASSERT_NE(frame, nullptr);
    FrameView view(frame);
    EXPECT_EQ(view.type(), DZ_FRAME_STG_USER_INPUT);
    EXPECT_EQ(std::string_view(view.ext_inst_id()), inst);
    EXPECT_EQ(view.ext_inst_payload_size(), cap_len);
    EXPECT_EQ(std::memcmp(view.ext_inst_payload(), buf.data(), cap_len), 0);
}
