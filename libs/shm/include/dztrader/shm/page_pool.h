#ifndef DZTRADER_SHM_PAGE_POOL_H_
#define DZTRADER_SHM_PAGE_POOL_H_

#include <dztrader/shm/channel_meta.h>
#include <memory>
#include <optional>
#include <unordered_map>

namespace dztrader::shm {

class Page;

namespace page_internal {
class PageImpl;
}  // namespace page_internal

class PagePool {
    using PageImplPtr = std::shared_ptr<page_internal::PageImpl>;

public:
    /// @param read_only true=只打开已存在的页文件, 不创建目录/文件、不 resize
    ///   (Reader 侧使用: 文件缺失/尺寸不符即报错, 不伪造零页 — 见 P2 审查结论)。
    ///   false=可创建目录/文件并 resize (Writer 侧, 默认, 保持既有行为)。
    explicit PagePool(std::shared_ptr<ChannelMeta> meta, bool read_only = false);

    std::optional<Page> get_page(uint64_t page_id) noexcept;
    bool prefetch_page(uint64_t page_id) noexcept;
    void close_pages_before(uint64_t page_id) noexcept;

private:
    std::unordered_map<uint64_t, PageImplPtr> pages_;
    std::shared_ptr<ChannelMeta> meta_;
    bool read_only_ = false;
};

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_PAGE_POOL_H_
