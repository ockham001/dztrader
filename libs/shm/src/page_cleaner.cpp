#include <dztrader/shm/page_cleaner.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include <dztrader/core/encoding.h>
#include <dztrader/shm/process_mutex.h>
#include <mutex>
#include <spdlog/spdlog.h>

namespace dztrader::shm {

PageCleaner::PageCleaner(std::shared_ptr<ChannelMeta> meta, CleanupPolicy policy)
    : meta_(std::move(meta)), policy_(policy) {}

uint64_t PageCleaner::active_page_id() const noexcept {
    return meta_->next_write_pos()->load() / meta_->page_size();
}

size_t PageCleaner::cleanup() {
    if (policy_.max_page_count == 0 && policy_.max_page_age_hours == 0) {
        return 0;
    }

    auto meta_mutex = meta_->create_meta_mutex();
    std::scoped_lock<ProcessMutex> lk(meta_mutex);  // NOLINT(misc-const-correctness)

    auto dir = meta_->page_dir();
    if (!std::filesystem::exists(dir)) {
        return 0;
    }

    // 收集所有 .dat 文件，按文件名排序（%08d.dat 天然有序 = 从旧到新）
    std::vector<std::filesystem::path> pages;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dat") {
            pages.push_back(entry.path());
        }
    }
    std::ranges::sort(pages);

    // 删除下限 = min(reader/writer page_index, active)，确保任何消费者仍使用的页不被删
    const uint64_t active = active_page_id();
    const uint64_t min_reader = meta_->min_reader_page_index_locked();
    const uint64_t min_writer = meta_->min_writer_page_index_locked();
    const uint64_t consumer_floor = std::min(min_reader, min_writer);
    const uint64_t floor = std::min(consumer_floor, active);
    auto now = std::filesystem::file_time_type::clock::now();

    size_t deleted = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
        const auto& path = pages[i];

        // 从文件名提取 page_id
        uint64_t page_id = 0;
        try {
            page_id = std::stoull(path.stem().string());
        } catch (...) {
            continue;  // 非数字文件名，跳过
        }

        // 不删除 floor 及之后的文件（消费者仍在使用或活跃页）
        if (page_id >= floor) {
            break;  // 文件有序，后续都更大，直接退出
        }

        // 按数量策略：保留最近 max_page_count 个
        bool exceed_count = false;
        if (policy_.max_page_count > 0 && pages.size() > policy_.max_page_count) {
            const size_t keep_from = pages.size() - policy_.max_page_count;
            exceed_count = (i < keep_from);
        }

        // 按时间策略：超过 max_page_age_hours
        bool exceed_age = false;
        if (policy_.max_page_age_hours > 0) {
            auto ftime = std::filesystem::last_write_time(path);
            auto age = std::chrono::duration_cast<std::chrono::hours>(now - ftime);
            exceed_age = (age.count() > 0LL && static_cast<uint64_t>(age.count()) > policy_.max_page_age_hours);
        }

        // 满足任一即删除
        if (!exceed_count && !exceed_age) {
            continue;
        }

        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            // 仍被映射（Windows 缓存映射）或异常竞态：跳过，不中断本次清理
            SPDLOG_WARN("page file remove skipped (still in use): path={} err={}",
                        path.string(), dztrader::to_utf8_from_system(ec.message()));
            continue;
        }
        ++deleted;
    }

    return deleted;
}

}  // namespace dztrader::shm
