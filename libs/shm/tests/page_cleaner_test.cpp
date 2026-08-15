#include "test_util.h"

#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/page_cleaner.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "test_ipc_util.h"

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::CleanupPolicy;
using dztrader::shm::PageCleaner;
using dztrader::shm::test::MB;
using dztrader::shm::test::cleanup_test_dir;
using dztrader::shm::test::create_page_file;
using dztrader::shm::test::spawn_helper;
using dztrader::shm::test::test_shm_dir;
using dztrader::shm::test::unique_channel_name;
using dztrader::shm::test::wait_for_file;
using dztrader::shm::test::wait_with_timeout;

class PageCleanerTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    ChannelConfig config_;
    std::shared_ptr<ChannelMeta> meta_;

    void SetUp() override
    {
        channel_name_ = unique_channel_name("dz_test_cleaner");
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

TEST_F(PageCleanerTest, NoCleanupWhenPolicyIsZero)
{
    CleanupPolicy policy{.max_page_count = 0, .max_page_age_hours = 0};
    PageCleaner cleaner(meta_, policy);
    EXPECT_EQ(cleaner.cleanup(), 0u);
}

TEST_F(PageCleanerTest, CleanupByCountDeletesOldFiles)
{
    auto dir = meta_->page_dir();
    create_page_file(dir, 1, 1 * MB);
    create_page_file(dir, 2, 1 * MB);
    create_page_file(dir, 3, 1 * MB);

    meta_->next_write_pos()->store(3 * 1 * MB, boost::memory_order_release);

    CleanupPolicy policy{.max_page_count = 2, .max_page_age_hours = 0};
    PageCleaner cleaner(meta_, policy);

    size_t deleted = cleaner.cleanup();
    EXPECT_GE(deleted, 1u);

    EXPECT_TRUE(std::filesystem::exists(dir / std::format("{:08d}.dat", 3)));
}

TEST_F(PageCleanerTest, CleanupDoesNotDeleteActivePage)
{
    auto dir = meta_->page_dir();
    create_page_file(dir, 1, 1 * MB);
    create_page_file(dir, 2, 1 * MB);

    meta_->next_write_pos()->store(2 * 1 * MB, boost::memory_order_release);

    CleanupPolicy policy{.max_page_count = 1, .max_page_age_hours = 0};
    PageCleaner cleaner(meta_, policy);

    size_t deleted = cleaner.cleanup();
    (void)deleted;
    EXPECT_TRUE(std::filesystem::exists(dir / std::format("{:08d}.dat", 2)));
}

TEST_F(PageCleanerTest, CleanupSkipsNonDatFiles)
{
    auto dir = meta_->page_dir();
    create_page_file(dir, 1, 1 * MB);
    std::ofstream ofs(dir / "readme.txt", std::ios::binary);
    ofs.seekp(1024 - 1);
    ofs.write("", 1);

    meta_->next_write_pos()->store(2 * 1 * MB, boost::memory_order_release);

    CleanupPolicy policy{.max_page_count = 1, .max_page_age_hours = 0};
    PageCleaner cleaner(meta_, policy);

    size_t deleted = cleaner.cleanup();
    (void)deleted;
    EXPECT_TRUE(std::filesystem::exists(dir / "readme.txt"));
}

TEST_F(PageCleanerTest, CleanupReturnsZeroWhenNoFiles)
{
    CleanupPolicy policy{.max_page_count = 1, .max_page_age_hours = 0};
    PageCleaner cleaner(meta_, policy);

    size_t deleted = cleaner.cleanup();
    EXPECT_EQ(deleted, 0u);
}

TEST_F(PageCleanerTest, CleanupByAgeDeletesOldFiles)
{
    auto dir = meta_->page_dir();
    create_page_file(dir, 1, 1 * MB);
    create_page_file(dir, 2, 1 * MB);

    meta_->next_write_pos()->store(3 * 1 * MB, boost::memory_order_release);

    auto old_path1 = dir / std::format("{:08d}.dat", 1);
    auto old_path2 = dir / std::format("{:08d}.dat", 2);

    auto old_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
    std::filesystem::last_write_time(old_path1, old_time);
    std::filesystem::last_write_time(old_path2, old_time);

    CleanupPolicy policy{.max_page_count = 0, .max_page_age_hours = 24};
    PageCleaner cleaner(meta_, policy);

    size_t deleted = cleaner.cleanup();
    EXPECT_GE(deleted, 2u);

    EXPECT_FALSE(std::filesystem::exists(old_path1));
    EXPECT_FALSE(std::filesystem::exists(old_path2));
}

TEST_F(PageCleanerTest, CleanupByAgeKeepsRecentFiles)
{
    auto dir = meta_->page_dir();
    create_page_file(dir, 1, 1 * MB);

    meta_->next_write_pos()->store(2 * 1 * MB, boost::memory_order_release);

    auto recent_path = dir / std::format("{:08d}.dat", 1);
    auto recent_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::last_write_time(recent_path, recent_time);

    CleanupPolicy policy{.max_page_count = 0, .max_page_age_hours = 24};
    PageCleaner cleaner(meta_, policy);

    size_t deleted = cleaner.cleanup();
    EXPECT_EQ(deleted, 0u);
    EXPECT_TRUE(std::filesystem::exists(recent_path));
}

TEST_F(PageCleanerTest, CombinedCountAndAgePolicy)
{
    auto dir = meta_->page_dir();
    create_page_file(dir, 1, 1 * MB);
    create_page_file(dir, 2, 1 * MB);
    create_page_file(dir, 3, 1 * MB);

    meta_->next_write_pos()->store(4 * 1 * MB, boost::memory_order_release);

    auto old_path = dir / std::format("{:08d}.dat", 1);
    auto old_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
    std::filesystem::last_write_time(old_path, old_time);

    CleanupPolicy policy{.max_page_count = 2, .max_page_age_hours = 24};
    PageCleaner cleaner(meta_, policy);

    size_t deleted = cleaner.cleanup();
    EXPECT_GE(deleted, 1u);
}

TEST_F(PageCleanerTest, CleanupRespectsReaderPageIndex) {
    auto dir = meta_->page_dir();
    create_page_file(dir, 1, 1 * MB);
    create_page_file(dir, 2, 1 * MB);
    meta_->next_write_pos()->store(3 * 1 * MB, boost::memory_order_release);

    meta_->clear_readers();
    EXPECT_TRUE(meta_->add_reader("stg.slow", 1234));
    meta_->set_reader_page_index("stg.slow", 1);  // reader 仍需 page 1

    CleanupPolicy policy{.max_page_count = 1, .max_page_age_hours = 0};
    PageCleaner cleaner(meta_, policy);
    size_t deleted = cleaner.cleanup();
    (void)deleted;
    // page 1 受 reader index 保护，不得删除
    EXPECT_TRUE(std::filesystem::exists(dir / std::format("{:08d}.dat", 1)));
}

TEST_F(PageCleanerTest, CleanupSkipsRemoveFailure) {
    auto dir = meta_->page_dir();
    create_page_file(dir, 1, 1 * MB);
    meta_->next_write_pos()->store(2 * 1 * MB, boost::memory_order_release);
    // 无 reader/writer，floor = active = 2，page 1 应被删
    CleanupPolicy policy{.max_page_count = 1, .max_page_age_hours = 0};
    PageCleaner cleaner(meta_, policy);
    // 正常情况删除成功；此测试主要保证不抛
    EXPECT_NO_THROW(cleaner.cleanup());
}

// 多进程：另一进程以裸 file_mapping 持有 page 1（不注册 reader，对 cleaner 不可见）。
// 无 reader/writer → floor = active = 3，page 1/2 均符合删除条件。
//  - Linux：remove 成功（unlink），但 helper 的 mapping 借 inode 引用计数仍可读。
//  - Windows：remove 因文件被映射而失败 → cleaner 跳过并告警，page 1 留在原处。
// 两种平台下 cleaner 都不得抛异常，且 helper 的 mapping 必须保持可读。
TEST_F(PageCleanerTest, CleanupMultiProcMappingHeld) {
    auto dir = meta_->page_dir();
    create_page_file(dir, 1, 1 * MB);
    create_page_file(dir, 2, 1 * MB);
    create_page_file(dir, 3, 1 * MB);
    meta_->next_write_pos()->store(3 * 1 * MB, boost::memory_order_release);  // active = page 3

    auto signal_file = shm_dir_ / "helper_mapped";
    boost::asio::io_context ctx;
    // helper 持 page 1 的裸 mapping（非 reader），hold 3s 保证与 cleaner 重叠
    auto proc = spawn_helper(ctx, {"file_mapping_hold", channel_name_, shm_dir_.string(),
                                   "1", "3000", signal_file.string()});
    ASSERT_TRUE(wait_for_file(signal_file, std::chrono::seconds(5)))
        << "helper failed to establish mapping";

    CleanupPolicy policy{.max_page_count = 1, .max_page_age_hours = 0};
    PageCleaner cleaner(meta_, policy);
    EXPECT_NO_THROW(cleaner.cleanup());

    // active page 始终保留；page 2 未被映射，两平台均删除
    EXPECT_TRUE(std::filesystem::exists(dir / std::format("{:08d}.dat", 3)));
    EXPECT_FALSE(std::filesystem::exists(dir / std::format("{:08d}.dat", 2)));
    // page 1 被 helper 映射：Linux 删除（inode 引用计数）、Windows 跳过（映射），
    // 行为平台相关，故不断言其存在性，由 helper 退出码验证 mapping 完整性。

    int result = wait_with_timeout(proc, std::chrono::seconds(5));
    EXPECT_NE(result, -1) << "helper hung (killed by timeout)";
    EXPECT_EQ(result, 0) << "helper mapping was corrupted or unreadable";
}
