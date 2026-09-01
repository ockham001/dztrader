#include <gtest/gtest.h>

#include <dztrader/data_type.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/struct.h>

#include <cstring>
#include <memory>
#include <vector>

#include "event_preloader.h"

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace dztrader::webui {
namespace {

constexpr uint64_t kMB = 1024 * 1024;
constexpr uint32_t kBigPayload = 600 * 1024;

class EventPreloaderTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<ChannelMeta> meta_;
    std::shared_ptr<Reader> reader_;
    std::shared_ptr<MultiWriter> writer_;

    void SetUp() override {
        channel_name_ = "dz_test_webui_preload";
        shm_dir_ = std::filesystem::temp_directory_path() / channel_name_;
        std::filesystem::remove_all(shm_dir_);
        ChannelConfig cfg{
            .channel_name = channel_name_,
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * kMB,
            .page_size = kMB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        meta_ = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(cfg));
        // 注册页索引上报 key: release_old_pages/close_old_pages 对缺失 key 静默忽略,
        // 注册后 min_*_page_index() 才能观察到维护效果 (对齐 master 的订阅者注册)
        if (!meta_->add_writer("test_writer", 123)) {
            ADD_FAILURE() << "add_writer failed";
        }
        if (!meta_->add_reader("test_reader", 123)) {
            ADD_FAILURE() << "add_reader failed";
        }
        reader_ = std::make_shared<Reader>(Reader::create(meta_, "test_reader"));
        writer_ = std::make_shared<MultiWriter>(MultiWriter::create(meta_, "test_writer"));
    }

    void TearDown() override { std::filesystem::remove_all(shm_dir_); }

    /// 写一个大帧 (payload 近半页): 连写两个即跨页, 使 writer/reader 停在第 1 页
    void write_big_frame(DzFrameType type) {
        std::byte* payload = writer_->open_frame(type, kBigPayload);
        ASSERT_NE(payload, nullptr);
        std::memset(payload, 0xCD, kBigPayload);
        writer_->close_frame();
    }

    /// reader 追平 nwp (读尽所有帧)
    void drain_reader() {
        while (reader_->next_frame() != nullptr) {
        }
    }

    static DzShmPreload make_params(uint64_t bytes, uint32_t pages) {
        return DzShmPreload{.bytes = bytes, .pages = pages, .reserved = 0};
    }
};

/// 跨页铺垫后, on_event_shm_timer 的 reader 半边应把当前页索引发报到 meta。
/// 注: writer 半边经 post_to_io_loop 入队但测试环境循环不运行, 有意不断言
/// (writer 效果由下一个用例直调 maintain_writer_shm 覆盖)。
TEST_F(EventPreloaderTest, OnEventShmTimerReportsReaderPageIndex) {
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    ASSERT_EQ(writer_->current_page_id(), 1u);
    drain_reader();
    ASSERT_EQ(reader_->current_page_id(), 1u);
    ASSERT_EQ(meta_->min_reader_page_index(), 0u);  // 从未上报过

    EventChannelPreloader preloader(reader_, writer_);
    preloader.on_event_shm_timer(make_params(0, 0));  // 全零参数仍执行 release

    EXPECT_EQ(meta_->min_reader_page_index(), reader_->current_page_id());
}

/// writer 半边直调: 应把当前页索引上报到 meta
TEST_F(EventPreloaderTest, MaintainWriterShmReportsWriterPageIndex) {
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    ASSERT_EQ(writer_->current_page_id(), 1u);
    ASSERT_EQ(meta_->min_writer_page_index(), 0u);

    EventChannelPreloader::maintain_writer_shm(*writer_, make_params(1, 0));

    EXPECT_EQ(meta_->min_writer_page_index(), writer_->current_page_id());
}

/// 调度链路: schedule(delay=0) -> tick_due() 同步触发到期回调 (schedule→timer→callback)。
/// 注: writer 半边入队不运行, 本用例只断言 reader 侧效果与 idle 状态迁移。
TEST_F(EventPreloaderTest, ScheduleFiresDueCallback) {
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    drain_reader();

    EventChannelPreloader preloader(reader_, writer_);
    ASSERT_TRUE(preloader.idle());

    preloader.schedule_event_shm_preload(make_params(0, 0), std::chrono::milliseconds(0));
    EXPECT_FALSE(preloader.idle());
    preloader.tick_due();
    EXPECT_TRUE(preloader.idle());                                // 已消费
    EXPECT_EQ(meta_->min_reader_page_index(), reader_->current_page_id());
}

}  // namespace
}  // namespace dztrader::webui
