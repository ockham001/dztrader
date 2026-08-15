#include "test_util.h"

#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/page.h>
#include <dztrader/shm/page_pool.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::PagePool;
using dztrader::shm::test::cleanup_test_dir;
using dztrader::shm::test::MB;
using dztrader::shm::test::test_shm_dir;
using dztrader::shm::test::unique_channel_name;

class PagePoolTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    ChannelConfig config_;
    std::shared_ptr<ChannelMeta> meta_;

    void SetUp() override {
        channel_name_ = unique_channel_name("dz_test_pagepool");
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

    void TearDown() override {
        meta_.reset();
        cleanup_test_dir(shm_dir_);
    }
};

TEST_F(PagePoolTest, GetPageReturnsValidPage) {
    PagePool pool(meta_);
    auto page = pool.get_page(0);
    ASSERT_TRUE(page.has_value());
    EXPECT_NE(page->data(), nullptr);
    EXPECT_EQ(page->size(), 1 * MB);
    EXPECT_EQ(page->page_id(), 0u);
}

TEST_F(PagePoolTest, GetPageForNewPageIdCreatesAndReturnsPage) {
    PagePool pool(meta_);
    auto page = pool.get_page(1);
    ASSERT_TRUE(page.has_value());
    EXPECT_NE(page->data(), nullptr);
    EXPECT_EQ(page->page_id(), 1u);
    EXPECT_TRUE(std::filesystem::exists(meta_->page_dir() / "00000001.dat"));
}

TEST_F(PagePoolTest, GetPageCachesSamePage) {
    PagePool pool(meta_);
    auto page1 = pool.get_page(0);
    auto page2 = pool.get_page(0);
    ASSERT_TRUE(page1.has_value());
    ASSERT_TRUE(page2.has_value());
    EXPECT_EQ(page1->data(), page2->data());
}

TEST_F(PagePoolTest, PreloadPageSucceeds) {
    PagePool pool(meta_);
    EXPECT_TRUE(pool.prefetch_page(0));
}

TEST_F(PagePoolTest, ClosePagesBeforeRemovesOldPages) {
    PagePool pool(meta_);

    auto page0 = pool.get_page(0);
    auto page1 = pool.get_page(1);
    auto page2 = pool.get_page(2);
    ASSERT_TRUE(page0.has_value());
    ASSERT_TRUE(page1.has_value());
    ASSERT_TRUE(page2.has_value());

    auto addr0 = page0->data();
    auto addr1 = page1->data();

    pool.close_pages_before(2);

    auto page2_after = pool.get_page(2);
    ASSERT_TRUE(page2_after.has_value());
    EXPECT_EQ(page2_after->page_id(), 2u);

    auto page0_new = pool.get_page(0);
    ASSERT_TRUE(page0_new.has_value());
    EXPECT_NE(page0_new->data(), addr0);

    auto page1_new = pool.get_page(1);
    ASSERT_TRUE(page1_new.has_value());
    EXPECT_NE(page1_new->data(), addr1);
}

// ---- P2: 只读 PagePool (Reader 侧) ----

TEST_F(PagePoolTest, ReadOnlyPoolDoesNotCreateMissingFile) {
    PagePool pool(meta_, /*read_only=*/true);
    auto page = pool.get_page(5);
    EXPECT_FALSE(page.has_value());
    EXPECT_FALSE(std::filesystem::exists(meta_->page_dir() / std::format("{:08d}.dat", 5)));
}

TEST_F(PagePoolTest, ReadOnlyPrefetchMissingFileReturnsFalseWithoutCreate) {
    PagePool pool(meta_, /*read_only=*/true);
    EXPECT_FALSE(pool.prefetch_page(5));
    EXPECT_FALSE(std::filesystem::exists(meta_->page_dir() / std::format("{:08d}.dat", 5)));
}

TEST_F(PagePoolTest, ReadOnlyPoolRejectsSizeMismatchWithoutResize) {
    // 手工创建尺寸错误的页文件 (101 字节 != 1MB)
    auto dir = meta_->page_dir();
    std::filesystem::create_directories(dir);
    auto path = dir / std::format("{:08d}.dat", 7);
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs.seekp(100);
        ofs.write("", 1);
    }

    PagePool pool(meta_, /*read_only=*/true);
    auto page = pool.get_page(7);
    EXPECT_FALSE(page.has_value());
    // 不得被 resize 成 page_size
    EXPECT_EQ(std::filesystem::file_size(path), 101u);
}

TEST_F(PagePoolTest, ReadOnlyPoolOpensExistingPage) {
    PagePool pool(meta_, /*read_only=*/true);
    auto page = pool.get_page(0);  // open_or_create 已写好页 0
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(page->size(), 1 * MB);
}
