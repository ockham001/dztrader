#include "test_util.h"

#include <gtest/gtest.h>

#include <dztrader/core/this_process.h>
#include <dztrader/data_type.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/struct.h>

#include <cstring>
#include <memory>
#include <random>
#include <string>
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

/// 进程唯一通道名（PID + 随机后缀）：ctest -j 下同二进制各用例是独立进程,
/// 固定名（dz_test_webui_preload）会互相 remove_all shm 目录 → 用例互踩 (§3.5)。
std::string unique_channel_name(const std::string& base) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return base + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
           std::to_string(dist(gen));
}

class EventPreloaderTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<ChannelMeta> meta_;
    std::shared_ptr<Reader> reader_;
    std::shared_ptr<MultiWriter> writer_;

    void SetUp() override {
        channel_name_ = unique_channel_name("dz_test_webui_preload");
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

    void TearDown() override {
        reader_.reset();
        writer_.reset();
        meta_.reset();
        EXPECT_TRUE(dztrader::shm::test::cleanup_channel(channel_name_, shm_dir_, {"test_reader"}));
    }

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
    EXPECT_TRUE(preloader.idle());  // 已消费
    EXPECT_EQ(meta_->min_reader_page_index(), reader_->current_page_id());
}

/// reader 侧 pages/bytes 非零: prefetch_pages + prefetch_for_bytes 两分支实际执行,
/// 且后续跨页读仍正确 (prefetch 缓存的页不破坏读取顺序/内容)。
TEST_F(EventPreloaderTest, OnEventShmTimerPrefetchesWithNonZeroParams) {
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    ASSERT_EQ(writer_->current_page_id(), 3u);  // 页 0..3 均已存在

    // 只消费前 2 帧: reader 停在 page 1, 页 2/3 成为 prefetch 目标
    ASSERT_NE(reader_->next_frame(), nullptr);
    ASSERT_NE(reader_->next_frame(), nullptr);
    ASSERT_EQ(reader_->current_page_id(), 1u);

    EventChannelPreloader preloader(reader_, writer_);
    // pages=1: 映射页 2; bytes=1: 目标 = nwp+1 → 页 3, 映射页 2..3 (两分支均真实执行)
    preloader.on_event_shm_timer(make_params(/*bytes=*/1, /*pages=*/1));

    // release 上报的读者页索引 = 1 (尚未读到页 2/3)
    EXPECT_EQ(meta_->min_reader_page_index(), 1u);

    // prefetch 后继续跨页读: 页 2/3 各 1 帧, 类型完整 (池状态未因 prefetch 破坏)
    int read = 0;
    while (const auto* f = reader_->next_frame()) {
        EXPECT_EQ(shm::FrameView(f).type(), DZ_FRAME_PRELOAD_EVENT_SHM);
        ++read;
    }
    EXPECT_EQ(read, 2);
    EXPECT_EQ(reader_->current_page_id(), 3u);  // 读毕停在页 3 (prefetch 目标 = 页 2..3)
}

/// 调度替换 (header 承诺): 同 tag 连续 schedule 只保留最新一次 —
/// 旧 (delay=0) 被取消, tick 不执行; 最新未到期亦不因 tick 触发; 再次替换为到期项后执行。
TEST_F(EventPreloaderTest, ScheduleReplaceKeepsOnlyLatest) {
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    drain_reader();

    EventChannelPreloader preloader(reader_, writer_);
    // 第一次 schedule (到期): 若未被替换, tick 会执行它 → release 上报索引
    preloader.schedule_event_shm_preload(make_params(0, 1), std::chrono::milliseconds(0));
    // 第二次 schedule 替换同一 tag 且未到期 (delay>0)
    preloader.schedule_event_shm_preload(make_params(0, 2), std::chrono::minutes(10));

    preloader.tick_due();  // 未到期 → 不触发 (旧定时器已被替换取消, 不会补执行)
    EXPECT_FALSE(preloader.idle());
    EXPECT_EQ(meta_->min_reader_page_index(), 0u);  // 未 release = 旧定时器确实被取消
    EXPECT_GT(preloader.next_wait_ms(), 0u);

    // 最新一次以 delay=0 替换 → tick 执行 (消费)
    preloader.schedule_event_shm_preload(make_params(0, 1), std::chrono::milliseconds(0));
    preloader.tick_due();
    EXPECT_TRUE(preloader.idle());
    EXPECT_EQ(meta_->min_reader_page_index(), reader_->current_page_id());
}

/// delay>0 未到期: 单次 schedule 后 tick 不触发, 队列保留
TEST_F(EventPreloaderTest, ScheduledFutureNotFiredByTick) {
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    write_big_frame(DZ_FRAME_PRELOAD_EVENT_SHM);
    drain_reader();

    EventChannelPreloader preloader(reader_, writer_);
    preloader.schedule_event_shm_preload(make_params(0, 1), std::chrono::minutes(10));
    EXPECT_FALSE(preloader.idle());

    preloader.tick_due();
    EXPECT_FALSE(preloader.idle());                 // 仍在队列 (未到期)
    EXPECT_EQ(meta_->min_reader_page_index(), 0u);  // 回调未执行
    EXPECT_GT(preloader.next_wait_ms(), 0u);
}

}  // namespace
}  // namespace dztrader::webui
