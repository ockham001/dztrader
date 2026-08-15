#ifndef DZTRADER_SHM_PAGE_H_
#define DZTRADER_SHM_PAGE_H_

#include <cstdint>
#include <memory>

namespace dztrader::shm {

namespace page_internal {
class PageImpl;
}

class Page {
    using ImplPtr = std::shared_ptr<page_internal::PageImpl>;

public:
    Page(const ImplPtr& impl);

    [[nodiscard]] uint64_t page_id() const noexcept { return page_id_; }

    [[nodiscard]] std::byte* data() const noexcept { return address_; }

    [[nodiscard]] uint64_t size() const noexcept { return size_; }

private:
    std::byte* address_;
    uint64_t page_id_;
    uint64_t size_;
    ImplPtr impl_;
};

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_PAGE_H_