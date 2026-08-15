#include "test_util.h"

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <cstring>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/page.h>
#include <dztrader/shm/page_pool.h>
#include <dztrader/struct.h>
#include <filesystem>
#include <gtest/gtest.h>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::Page;
using dztrader::shm::PagePool;
using dztrader::shm::test::cleanup_test_dir;
using dztrader::shm::test::MB;
using dztrader::shm::test::test_shm_dir;
using dztrader::shm::test::unique_channel_name;

class PageTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    ChannelConfig config_;
    std::shared_ptr<ChannelMeta> meta_;

    void SetUp() override
    {
        channel_name_ = unique_channel_name("dz_test_page");
        shm_dir_ = test_shm_dir(channel_name_);
        config_ = {
            .channel_name = channel_name_,
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * MB,
            .page_size = 1 * MB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        meta_ = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(config_));
    }

    void TearDown() override
    {
        meta_.reset();
        cleanup_test_dir(shm_dir_);
    }
};

TEST_F(PageTest, CreatePageFromChannelMeta)
{
    PagePool pool(meta_);
    auto page = pool.get_page(0);
    ASSERT_TRUE(page.has_value());

    EXPECT_NE(page->data(), nullptr);
    EXPECT_EQ(page->size(), 1 * MB);
    EXPECT_EQ(page->page_id(), 0u);
}

TEST_F(PageTest, PageAddressWritable)
{
    PagePool pool(meta_);
    auto page = pool.get_page(0);
    ASSERT_TRUE(page.has_value());

    const char test_data[] = "dztrader_page_test_data";
    auto offset = sizeof(DzFrameHeader);
    std::memcpy(page->data() + offset, test_data, sizeof(test_data));

    boost::interprocess::file_mapping mfile((meta_->page_dir() / "00000000.dat").string().c_str(),
                                            boost::interprocess::read_only);
    boost::interprocess::mapped_region region(mfile, boost::interprocess::read_only, offset, sizeof(test_data));

    EXPECT_EQ(std::memcmp(region.get_address(), test_data, sizeof(test_data)), 0);
}

TEST_F(PageTest, PageIdFromFilename)
{
    PagePool pool(meta_);

    auto page0 = pool.get_page(0);
    ASSERT_TRUE(page0.has_value());
    EXPECT_TRUE(std::filesystem::exists(meta_->page_dir() / "00000000.dat"));

    auto page1 = pool.get_page(1);
    ASSERT_TRUE(page1.has_value());
    EXPECT_TRUE(std::filesystem::exists(meta_->page_dir() / "00000001.dat"));
    EXPECT_EQ(page1->page_id(), 1u);
}

TEST_F(PageTest, DataPointerOffsetAccess)
{
    PagePool pool(meta_);
    auto page = pool.get_page(0);
    ASSERT_TRUE(page.has_value());

    const char test_data[] = "offset_test_data";
    auto offset = sizeof(DzFrameHeader) + 8;
    std::memcpy(page->data() + offset, test_data, sizeof(test_data));

    char buf[sizeof(test_data)];
    std::memcpy(buf, page->data() + offset, sizeof(test_data));
    EXPECT_EQ(std::memcmp(buf, test_data, sizeof(test_data)), 0);
}
