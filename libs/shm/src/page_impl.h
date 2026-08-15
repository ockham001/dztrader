#ifndef DZTRADER_SHM_PAGE_IMPL_H_
#define DZTRADER_SHM_PAGE_IMPL_H_

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <cstdint>
#include <dztrader/core/exception.h>
#include <filesystem>

namespace dztrader::shm {

namespace page_internal {

/// 只做 file_mapping + mapped_region。文件 create/resize 由 PagePool 在 meta 锁内完成，
/// 故本类不再持锁（meta 锁即原 page 锁，已合并，见 ChannelMeta 注释）。
class PageImpl {
public:
    PageImpl(const std::filesystem::path& file_path, uint64_t page_size, uint64_t page_id)
        : page_id_{page_id} {
        namespace bip = boost::interprocess;
        try {
            mfile_ = bip::file_mapping{file_path.string().c_str(), bip::read_write};
        } catch (const bip::interprocess_exception& e) {
            throw Exception(DZ_EC_SHM_OPEN_FAILED, "open file mapping failed: path={} err={}",
                            file_path.string(), e.what());
        }
        try {
            region_ = bip::mapped_region{mfile_, bip::read_write, 0, page_size};
        } catch (const bip::interprocess_exception& e) {
            throw Exception(DZ_EC_SHM_MAP_FAILED, "map region failed: path={} size={} err={}",
                            file_path.string(), page_size, e.what());
        }
        if (region_.get_address() == nullptr) {
            throw Exception(DZ_EC_SHM_MAP_FAILED, "mapped region is null: path={}", file_path.string());
        }
        if (region_.get_size() != page_size) {
            throw Exception(DZ_EC_SHM_MAP_FAILED, "mapped region size mismatch: path={} expected={} actual={}",
                            file_path.string(), page_size, region_.get_size());
        }
    }

    [[nodiscard]] std::byte* address() const noexcept { return static_cast<std::byte*>(region_.get_address()); }
    [[nodiscard]] uint64_t size() const noexcept { return static_cast<uint64_t>(region_.get_size()); }
    [[nodiscard]] uint64_t page_id() const noexcept { return page_id_; }

private:
    boost::interprocess::file_mapping mfile_;
    boost::interprocess::mapped_region region_;
    uint64_t page_id_;
};

}  // namespace page_internal

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_PAGE_IMPL_H_
