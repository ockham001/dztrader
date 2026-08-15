#ifndef DZTRADER_SHM_WRITER_H_
#define DZTRADER_SHM_WRITER_H_

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>

#include <dztrader/core/exception.h>
#include <dztrader/core/last_error.h>
#include <dztrader/error.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/null_lock.h>
#include <dztrader/shm/page.h>
#include <dztrader/shm/page_pool.h>
#include <dztrader/shm/process_mutex.h>
#include <dztrader/shm/spin_lock.h>
#include <dztrader/shm/subscriber_list.h>
#include <dztrader/struct.h>
#include <dztrader/core/string_util.h>

namespace dztrader::shm {

template <typename T>
concept AllowedWriteLock = std::is_same_v<T, NullLock> || std::is_same_v<T, ProcessMutex>;

template <AllowedWriteLock WriteLock>
class Writer {
    static constexpr bool IS_NULL_WRITE_LOCK = std::is_same_v<WriteLock, NullLock>;

public:
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) noexcept = default;
    Writer& operator=(Writer&&) = default;
    ~Writer() = default;

    [[nodiscard]] static Writer create(std::shared_ptr<ChannelMeta> meta,
                                       const std::string& writer_name);
    [[nodiscard]] static Writer create(const std::string& channel_name,
                                       const std::filesystem::path& shm_dir,
                                       const std::string& writer_name);

    template <typename T>
    bool write_frame(DzFrameType frame_type, const T& data) noexcept {
        static_assert(sizeof(T) % 8 == 0, "T must be 8-byte aligned");
        auto* frame = open_frame(frame_type, sizeof(T));
        if (frame == nullptr) {
            return false;
        }
        memcpy(frame, &data, sizeof(T));
        close_frame();
        return true;
    }

    bool write_ext_inst_frame(DzFrameType frame_type,
                              const char* instance_id,
                              const std::byte* data,
                              uint32_t size) noexcept {
        uint32_t padded_size = (size + 7u) & ~7u;
        auto* frame = open_frame(frame_type, sizeof(DzExtInstFrameHeader) + padded_size);
        if (frame == nullptr) {
            return false;
        }
        auto* inst_hdr = reinterpret_cast<DzExtInstFrameHeader*>(frame);
        copy_string(inst_hdr->instance_id, instance_id, true);
        inst_hdr->data_size = size;
        std::ranges::fill(inst_hdr->reserved, '\0');
        auto* payload = frame + sizeof(DzExtInstFrameHeader);
        if (size > 0) {
            memcpy(payload, data, size);
        }
        if (padded_size > size) {
            memset(payload + size, 0, padded_size - size);
        }
        close_frame();
        return true;
    }

    bool write_ext_frame(DzFrameType frame_type,
                         const std::byte* data,
                         uint32_t size) noexcept {
        uint32_t padded_size = (size + 7u) & ~7u;
        auto* frame = open_frame(frame_type, sizeof(DzExtFrameHeader) + padded_size);
        if (frame == nullptr) {
            return false;
        }
        auto* ext_hdr = reinterpret_cast<DzExtFrameHeader*>(frame);
        ext_hdr->data_size = size;
        std::ranges::fill(ext_hdr->reserved, '\0');
        auto* payload = frame + sizeof(DzExtFrameHeader);
        if (size > 0) {
            memcpy(payload, data, size);
        }
        if (padded_size > size) {
            memset(payload + size, 0, padded_size - size);
        }
        close_frame();
        return true;
    }

    std::byte* open_frame(DzFrameType frame_type, uint32_t data_size) noexcept;

    void close_frame() noexcept {
        if (pending_size_ == 0) {
            return;
        }  // open_frame 失败，跳过
        uint64_t new_pos = (pending_page_id_ * page_size_) + pending_offset_ + pending_size_;
        next_write_pos_->store(new_pos, boost::memory_order_release);
        if constexpr (IS_NULL_WRITE_LOCK) {
            offset_in_page_ = pending_offset_ + pending_size_;
            current_page_id_ = pending_page_id_;
        }
        pending_size_ = 0;
        write_lock_.unlock();
    }

    void notify_subscribers() noexcept { subscriber_list_.notify_all(); }

    bool refresh_subscribers() noexcept { return subscriber_list_.sync_from_channel_meta(*meta_); }

    void prefetch_pages(uint64_t count);

    void prefetch_for_bytes(uint64_t bytes);

    void close_old_pages() noexcept;

    /// 触页预热: 读取当前写入位置的内存, 触发 page fault, 确保物理页已加载到 RAM。
    /// 本方法不获取 write_lock_, 纳秒级, 由调用方保证线程安全 (如外层 SpinLock)。
    void touch_write_position() noexcept;

    [[nodiscard]] uint64_t write_position() const noexcept {
        return next_write_pos_->load(boost::memory_order_acquire);
    }

    [[nodiscard]] uint64_t current_page_id() const noexcept {
        if constexpr (IS_NULL_WRITE_LOCK) {
            return current_page_id_;
        } else {
            return write_position() / page_size_;
        }
    }

    [[nodiscard]] uint32_t offset_in_page() const noexcept {
        if constexpr (IS_NULL_WRITE_LOCK) {
            return static_cast<uint32_t>(offset_in_page_);
        } else {
            return static_cast<uint32_t>(write_position() % page_size_);
        }
    }

private:
    Writer(std::shared_ptr<ChannelMeta> meta,
           AtomicU64* nwp,
           uint64_t page_size,
           uint64_t current_page_id,
           uint64_t offset_in_page,
           Page page,
           PagePool page_pool,
           WriteLock write_lock,
           SubscriberList subscriber_list,
           std::string writer_name);

    static void write_padding(void* addr, uint64_t size) noexcept {
        auto* hdr = static_cast<DzFrameHeader*>(addr);
        hdr->frame_type = DZ_FRAME_INVALID_FILL;
        hdr->frame_size = static_cast<uint32_t>(size);
    }

    std::shared_ptr<ChannelMeta> meta_;
    AtomicU64* next_write_pos_ = nullptr;
    uint64_t page_size_ = 0;
    uint64_t current_page_id_ = 0;
    uint64_t offset_in_page_ = 0;
    uint64_t pending_page_id_ = 0;
    uint64_t pending_offset_ = 0;
    uint64_t pending_size_ = 0;
    Page page_;
    PagePool page_pool_;
    WriteLock write_lock_;
    SubscriberList subscriber_list_;
    std::string writer_name_;
};

using MultiWriter = Writer<ProcessMutex>;
using SingleWriter = Writer<NullLock>;

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_WRITER_H_
