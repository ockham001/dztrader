#ifndef DZTRADER_SHM_READER_H_
#define DZTRADER_SHM_READER_H_

#include <boost/atomic.hpp>
#include <cstdint>
#include <dztrader/core/exception.h>
#include <dztrader/core/last_error.h>
#include <dztrader/error.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/page.h>
#include <dztrader/shm/page_pool.h>
#include <dztrader/struct.h>
#include <memory>

namespace dztrader::shm {

class Reader {
public:
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) noexcept = default;
    Reader& operator=(Reader&&) = default;
    ~Reader() = default;

    [[nodiscard]] static Reader create(std::shared_ptr<ChannelMeta> meta,
                                       const std::string& reader_name);
    [[nodiscard]] static Reader create(const std::string& channel_name,
                                       const std::filesystem::path& shm_dir,
                                       const std::string& reader_name);

    const std::byte* next_frame() noexcept;

    void prefetch_pages(uint64_t num_pages);

    void prefetch_for_bytes(uint64_t bytes);

    void release_old_pages() noexcept;

    /// 触页预热: 读取当前读取位置的内存, 触发 page fault, 确保物理页已加载到 RAM。
    /// 本方法不获取锁, 由调用方保证线程安全。
    void touch_read_position() noexcept;

    [[nodiscard]] uint64_t read_position() const noexcept { return read_pos_; }

    [[nodiscard]] uint64_t current_page_id() const noexcept { return current_page_id_; }

private:
    Reader(std::shared_ptr<ChannelMeta> meta,
           AtomicU64* nwp,
           uint64_t page_size,
           uint64_t read_pos,
           uint64_t current_page_id,
           uint64_t offset_in_page,
           Page page,
           PagePool page_pool,
           std::string reader_name);

    std::shared_ptr<ChannelMeta> meta_;
    AtomicU64* next_write_pos_ = nullptr;
    uint64_t page_size_ = 0;
    uint64_t read_pos_ = 0;
    uint64_t current_page_id_ = 0;
    uint64_t offset_in_page_ = 0;
    Page page_;
    PagePool page_pool_;
    std::string reader_name_;
};

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_READER_H_
