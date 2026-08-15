#include "page_impl.h"

#include <dztrader/core/last_error.h>
#include <dztrader/shm/page.h>
#include <dztrader/shm/page_pool.h>

#include <dztrader/core/exception.h>
#include <dztrader/shm/process_mutex.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>

namespace dztrader::shm {

PagePool::PagePool(std::shared_ptr<ChannelMeta> meta, bool read_only)
    : meta_(std::move(meta)), read_only_(read_only) {}

std::optional<Page> PagePool::get_page(uint64_t page_id) noexcept {
    try {
        auto it = pages_.find(page_id);
        if (it != pages_.end()) {
            return Page(it->second);
        }
        auto file_dir = meta_->page_dir();
        auto file_path = file_dir / std::format("{:08d}.dat", page_id);

        auto meta_mutex = meta_->create_meta_mutex();
        std::scoped_lock<ProcessMutex> lk(meta_mutex);
        if (read_only_) {
            // Reader 只开不建: 页文件由写者在其 nwp 推进前创建 (meta 锁 + release/acquire
            // 保证读者需要时文件必已存在); 文件缺失/尺寸不符 = 通道被外部破坏,
            // 报错而非伪造零页 (伪造的零页会让读者永久卡在 invalid-fill)。
            std::error_code ec;
            if (!std::filesystem::exists(file_path, ec) ||
                std::filesystem::file_size(file_path, ec) != meta_->page_size()) {
                LastError::set(DZ_EC_SHM_OPEN_FAILED, "page file missing or size mismatch: path={}",
                               file_path.string());
                return std::nullopt;
            }
        } else {
            if (!std::filesystem::exists(file_dir)) {
                std::filesystem::create_directories(file_dir);
            }
            if (!std::filesystem::exists(file_path)) {
                std::ofstream ofs(file_path, std::ios::binary);
                if (!ofs) {
                    throw Exception(DZ_EC_SHM_CREATE_FAILED, "create page failed: path={}", file_path.string());
                }
            }
            if (std::filesystem::file_size(file_path) != meta_->page_size()) {
                std::filesystem::resize_file(file_path, meta_->page_size());
            }
        }
        // 文件已就绪，meta 锁内构造 mapping（PageImpl 不再持锁）
        auto impl = std::make_shared<page_internal::PageImpl>(file_path, meta_->page_size(), page_id);
        pages_[page_id] = impl;
        return Page(impl);
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
        return std::nullopt;
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_SHM_CREATE_FAILED, "get_page failed: {}", e.what());
        return std::nullopt;
    }
}

bool PagePool::prefetch_page(uint64_t page_id) noexcept {
    try {
        auto it = pages_.find(page_id);
        if (it != pages_.end()) {
            return true;
        }
        auto file_dir = meta_->page_dir();
        auto file_path = file_dir / std::format("{:08d}.dat", page_id);

        auto meta_mutex = meta_->create_meta_mutex();
        std::scoped_lock<ProcessMutex> lk(meta_mutex);
        if (read_only_) {
            // Reader 预取 best-effort: 未来页可能尚未由写者创建, 缺失/尺寸不符直接返回 false
            std::error_code ec;
            if (!std::filesystem::exists(file_path, ec) ||
                std::filesystem::file_size(file_path, ec) != meta_->page_size()) {
                return false;
            }
        } else {
            if (!std::filesystem::exists(file_dir)) {
                std::filesystem::create_directories(file_dir);
            }
            if (!std::filesystem::exists(file_path)) {
                std::ofstream ofs(file_path, std::ios::binary);
                if (!ofs) {
                    throw Exception(DZ_EC_SHM_CREATE_FAILED, "create page failed: path={}", file_path.string());
                }
            }
            if (std::filesystem::file_size(file_path) != meta_->page_size()) {
                std::filesystem::resize_file(file_path, meta_->page_size());
            }
        }
        auto impl = std::make_shared<page_internal::PageImpl>(file_path, meta_->page_size(), page_id);
        pages_[page_id] = impl;
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
        return false;
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_SHM_CREATE_FAILED, "prefetch_page failed: {}", e.what());
        return false;
    }
}

void PagePool::close_pages_before(uint64_t page_id) noexcept {
    std::erase_if(pages_, [page_id](const auto& pair) { return pair.first < page_id; });
}

}  // namespace dztrader::shm
