#include "test_util.h"

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <dztrader/core/exception.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/common.h>
#include <dztrader/struct.h>
#include <filesystem>
#include <format>
#include <gtest/gtest.h>
#include <string>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::test::cleanup_test_dir;
using dztrader::shm::test::create_page_file;
using dztrader::shm::test::MB;
using dztrader::shm::test::test_shm_dir;
using dztrader::shm::test::unique_channel_name;

class ChannelMetaTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    ChannelConfig config_;

    void SetUp() override {
        channel_name_ = unique_channel_name("dz_test_meta");
        shm_dir_ = test_shm_dir(channel_name_);
        config_ = ChannelConfig{
            .channel_name = channel_name_,
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * MB,
            .page_size = 1 * MB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
    }

    void TearDown() override { cleanup_test_dir(shm_dir_); }
};

TEST_F(ChannelMetaTest, OpenOrCreateInitializesNextWritePos) {
    ChannelConfig config{
        .channel_name = channel_name_,
        .shm_dir = shm_dir_,
        .meta_file_size = 4 * MB,
        .page_size = 1 * MB,
        .lock_memory = false,
        .prefetch_memory = false,
    };

    auto meta = ChannelMeta::open_or_create(config);
    ASSERT_NE(meta.next_write_pos(), nullptr);
    EXPECT_GT(meta.next_write_pos()->load(), 0u);
}

TEST_F(ChannelMetaTest, OpenOrCreateCreatesPageZeroFile) {
    ChannelConfig config{
        .channel_name = channel_name_,
        .shm_dir = shm_dir_,
        .meta_file_size = 4 * MB,
        .page_size = 1 * MB,
        .lock_memory = false,
        .prefetch_memory = false,
    };

    auto meta = ChannelMeta::open_or_create(config);
    auto page0 = meta.page_dir() / std::format("{:08d}.dat", 0);
    EXPECT_TRUE(std::filesystem::exists(page0));
}

TEST_F(ChannelMetaTest, OpenExistingPreservesNextWritePos) {
    ChannelConfig config{
        .channel_name = channel_name_,
        .shm_dir = shm_dir_,
        .meta_file_size = 4 * MB,
        .page_size = 1 * MB,
        .lock_memory = false,
        .prefetch_memory = false,
    };

    uint64_t pos = 0;
    {
        auto meta = ChannelMeta::open_or_create(config);
        pos = meta.next_write_pos()->load();
        ASSERT_GT(pos, 0u);
    }

    {
        auto meta = ChannelMeta::open_only(config.channel_name, config.shm_dir);
        EXPECT_EQ(meta.next_write_pos()->load(), pos);
    }
}

TEST_F(ChannelMetaTest, PageSizeChangeCleansUpFiles) {
    ChannelConfig config1{
        .channel_name = channel_name_,
        .shm_dir = shm_dir_,
        .meta_file_size = 4 * MB,
        .page_size = 1 * MB,
        .lock_memory = false,
        .prefetch_memory = false,
    };

    {
        auto meta = ChannelMeta::open_or_create(config1);
        auto dir = meta.page_dir();
        create_page_file(dir, 1, 1 * MB);
        create_page_file(dir, 2, 1 * MB);
    }

    ChannelConfig config2{
        .channel_name = channel_name_,
        .shm_dir = shm_dir_,
        .meta_file_size = 4 * MB,
        .page_size = 2 * MB,
        .lock_memory = false,
        .prefetch_memory = false,
    };

    auto meta = ChannelMeta::open_or_create(config2);
    auto dir = meta.page_dir();

    EXPECT_FALSE(std::filesystem::exists(dir / std::format("{:08d}.dat", 1)));
    EXPECT_FALSE(std::filesystem::exists(dir / std::format("{:08d}.dat", 2)));

    EXPECT_GT(meta.next_write_pos()->load(), 0u);

    auto page0 = dir / std::format("{:08d}.dat", 0);
    EXPECT_TRUE(std::filesystem::exists(page0));
    EXPECT_EQ(std::filesystem::file_size(page0), 2 * MB);
}

// page_size 重置只清数字页文件: meta.dat 必须保留, 且重置后子进程视角
// open_only 按名打开 meta.dat 必须成功 (回归: 旧实现误删 meta.dat)
TEST_F(ChannelMetaTest, PageSizeChangeKeepsMetaFileAndReopenSucceeds) {
    ChannelConfig config1{
        .channel_name = channel_name_,
        .shm_dir = shm_dir_,
        .meta_file_size = 4 * MB,
        .page_size = 1 * MB,
        .lock_memory = false,
        .prefetch_memory = false,
    };

    {
        auto meta = ChannelMeta::open_or_create(config1);
        auto dir = meta.page_dir();
        create_page_file(dir, 1, 1 * MB);
        create_page_file(dir, 2, 1 * MB);
    }

    ChannelConfig config2{
        .channel_name = channel_name_,
        .shm_dir = shm_dir_,
        .meta_file_size = 4 * MB,
        .page_size = 2 * MB,
        .lock_memory = false,
        .prefetch_memory = false,
    };

    {
        auto meta = ChannelMeta::open_or_create(config2);
        auto dir = meta.page_dir();

        // (a) meta.dat 仍存在 (正被本进程映射, 重置不得删除其名字)
        EXPECT_TRUE(std::filesystem::exists(dir / "meta.dat"));

        // (b) 旧数字页文件已清, 页 0 以新 page_size 重建, 写位置归零重写初始帧
        EXPECT_FALSE(std::filesystem::exists(dir / std::format("{:08d}.dat", 1)));
        EXPECT_FALSE(std::filesystem::exists(dir / std::format("{:08d}.dat", 2)));
        auto page0 = dir / std::format("{:08d}.dat", 0);
        EXPECT_TRUE(std::filesystem::exists(page0));
        EXPECT_EQ(std::filesystem::file_size(page0), 2 * MB);
        EXPECT_GT(meta.next_write_pos()->load(), 0u);
    }

    // (c) 模拟子进程视角: 重置后 open_only 按名打开 meta.dat 不抛
    EXPECT_NO_THROW({
        auto meta = ChannelMeta::open_only(channel_name_, shm_dir_);
        EXPECT_EQ(meta.page_size(), 2 * MB);
    });
}

TEST_F(ChannelMetaTest, FirstFrameIsInvalidFill) {
    ChannelConfig config{
        .channel_name = channel_name_,
        .shm_dir = shm_dir_,
        .meta_file_size = 4 * MB,
        .page_size = 1 * MB,
        .lock_memory = false,
        .prefetch_memory = false,
    };

    auto meta = ChannelMeta::open_or_create(config);
    auto page0 = meta.page_dir() / std::format("{:08d}.dat", 0);

    boost::interprocess::file_mapping mfile(page0.string().c_str(), boost::interprocess::read_only);
    boost::interprocess::mapped_region region(mfile, boost::interprocess::read_only, 0,
                                              sizeof(DzFrameHeader));

    auto* header = static_cast<const DzFrameHeader*>(region.get_address());
    EXPECT_EQ(static_cast<int>(header->frame_type), 0);
    EXPECT_EQ(header->frame_size, static_cast<uint32_t>(sizeof(DzFrameHeader)));
}

TEST_F(ChannelMetaTest, PageSizeValidationRejectsTooSmall) {
    ChannelConfig config{
        .channel_name = channel_name_,
        .shm_dir = shm_dir_,
        .meta_file_size = 4 * MB,
        .page_size = 512,
        .lock_memory = false,
        .prefetch_memory = false,
    };

    EXPECT_THROW((void)ChannelMeta::open_or_create(config), dztrader::Exception);
}

TEST_F(ChannelMetaTest, MetaFileSizeValidationRejectsTooSmall) {
    ChannelConfig config{
        .channel_name = channel_name_,
        .shm_dir = shm_dir_,
        .meta_file_size = 100,
        .page_size = 1 * MB,
        .lock_memory = false,
        .prefetch_memory = false,
    };

    EXPECT_THROW((void)ChannelMeta::open_or_create(config), dztrader::Exception);
}

TEST_F(ChannelMetaTest, ReaderWriterMapAddRemoveSetIndex) {
    auto meta = ChannelMeta::open_or_create(config_);
    meta.clear_readers();
    meta.clear_writers();

    EXPECT_TRUE(meta.add_reader("stg.r1", 1234));
    EXPECT_FALSE(meta.add_reader("stg.r1", 1234));  // 重复
    EXPECT_TRUE(meta.add_writer("gw.w1", 5678));

    EXPECT_EQ(meta.min_reader_page_index(), 0u);     // 初值 0
    EXPECT_EQ(meta.min_writer_page_index(), 0u);

    meta.set_reader_page_index("stg.r1", 5);
    meta.set_writer_page_index("gw.w1", 7);
    EXPECT_EQ(meta.min_reader_page_index(), 5u);
    EXPECT_EQ(meta.min_writer_page_index(), 7u);

    auto names = meta.reader_names();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "stg.r1");

    meta.remove_reader("stg.r1");
    EXPECT_EQ(meta.min_reader_page_index(), UINT64_MAX);  // 空 map
}

TEST_F(ChannelMetaTest, SetPageIndexThrowsWhenKeyAbsent) {
    auto meta = ChannelMeta::open_or_create(config_);
    meta.clear_readers();
    EXPECT_THROW(meta.set_reader_page_index("absent", 1), dztrader::Exception);
}

TEST_F(ChannelMetaTest, CreateMetaMutexSameNameAsPageMutexReplaced) {
    auto meta = ChannelMeta::open_or_create(config_);
    auto m = meta.create_meta_mutex();  // 不抛异常即通过
    (void)m;
}

// ---- P3: 启动结构校验 (仅日志, 不抛异常、不修复) ----

// ① nwp 错位: open_or_create 仅记 ERROR 日志, 不抛
TEST_F(ChannelMetaTest, OpenOrCreateToleratesMisalignedNwp) {
    {
        auto meta = ChannelMeta::open_or_create(config_);
        meta.next_write_pos()->store(7, boost::memory_order_release);  // 7 % 8 != 0
    }
    EXPECT_NO_THROW({
        auto meta = ChannelMeta::open_or_create(config_);
        (void)meta;
    });
}

// ② 活跃页文件缺失: open_or_create 仅记 ERROR 日志, 不抛
TEST_F(ChannelMetaTest, OpenOrCreateToleratesMissingActivePageFile) {
    {
        auto meta = ChannelMeta::open_or_create(config_);
        // nwp 推进到页 1, 但不创建页 1 文件
        meta.next_write_pos()->store(1 * MB + 8, boost::memory_order_release);
    }
    EXPECT_NO_THROW({
        auto meta = ChannelMeta::open_or_create(config_);
        (void)meta;
    });
}

// ③ 活跃页坏帧链: 链完好时静默通过; 破坏后仅记 WARN, 不抛
TEST_F(ChannelMetaTest, OpenOrCreateToleratesCorruptActivePageChain) {
    namespace bip2 = boost::interprocess;
    const auto page_path = [this]() {
        return shm_dir_ / channel_name_ / std::format("{:08d}.dat", 0);
    };
    {
        auto meta = ChannelMeta::open_or_create(config_);
        // 页 0 追加一个 8 字节空帧, nwp=16: 链 = [0,8) 初始填充 + [8,16) 空帧
        bip2::file_mapping mfile{page_path().string().c_str(), bip2::read_write};
        bip2::mapped_region region{mfile, bip2::read_write, 0, 1 * MB};
        auto* hdr = reinterpret_cast<DzFrameHeader*>(
            static_cast<std::byte*>(region.get_address()) + 8);
        hdr->frame_type = DZ_FRAME_RTN_MD_TICK;
        hdr->frame_size = 8;
        meta.next_write_pos()->store(16, boost::memory_order_release);
    }
    // 链完好: 校验通过, 不抛
    EXPECT_NO_THROW({
        auto meta = ChannelMeta::open_or_create(config_);
        (void)meta;
    });

    // 破坏第 2 帧头 (frame_size=7 错位且 < 帧头) -> 校验仅 WARN, 不抛
    {
        bip2::file_mapping mfile{page_path().string().c_str(), bip2::read_write};
        bip2::mapped_region region{mfile, bip2::read_write, 0, 1 * MB};
        auto* hdr = reinterpret_cast<DzFrameHeader*>(
            static_cast<std::byte*>(region.get_address()) + 8);
        hdr->frame_size = 7;
    }
    EXPECT_NO_THROW({
        auto meta = ChannelMeta::open_or_create(config_);
        (void)meta;
    });
}
