#include <dztrader/shm/writer.h>

namespace dztrader::shm {

template <AllowedWriteLock WriteLock>
Writer<WriteLock>::Writer(std::shared_ptr<ChannelMeta> meta,
                          AtomicU64* nwp,
                          uint64_t page_size,
                          uint64_t current_page_id,
                          uint64_t offset_in_page,
                          Page page,
                          PagePool page_pool,
                          WriteLock write_lock,
                          SubscriberList subscriber_list,
                          std::string writer_name)
    : meta_(std::move(meta)),
      next_write_pos_(nwp),
      page_size_(page_size),
      current_page_id_(current_page_id),
      offset_in_page_(offset_in_page),
      page_(std::move(page)),
      page_pool_(std::move(page_pool)),
      write_lock_(std::move(write_lock)),
      subscriber_list_(std::move(subscriber_list)),
      writer_name_(std::move(writer_name)) {}

template <AllowedWriteLock WriteLock>
Writer<WriteLock> Writer<WriteLock>::create(std::shared_ptr<ChannelMeta> meta,
                                            const std::string& writer_name) {
    auto* nwp = meta->next_write_pos();
    uint64_t page_size = meta->page_size();
    PagePool page_pool(meta);
    SubscriberList subscriber_list;
    subscriber_list.sync_from_channel_meta(*meta);

    const uint64_t pos = nwp->load(boost::memory_order_acquire);
    uint64_t current_page_id = pos / page_size;
    uint64_t offset_in_page = pos % page_size;

    auto page_opt = page_pool.get_page(current_page_id);
    if (!page_opt) {
        throw Exception(DZ_EC_SHM_OPEN_FAILED, "writer create: failed to get page {}",
                        current_page_id);
    }

    auto make = [&](WriteLock wl) -> Writer {
        return Writer(std::move(meta), nwp, page_size, current_page_id, offset_in_page,
                      std::move(page_opt.value()), std::move(page_pool), std::move(wl),
                      std::move(subscriber_list), writer_name);
    };

    if constexpr (std::is_same_v<WriteLock, ProcessMutex>) {
        return make(meta->create_write_mutex());
    } else {
        return make(NullLock{});
    }
}

template <AllowedWriteLock WriteLock>
Writer<WriteLock> Writer<WriteLock>::create(const std::string& channel_name,
                                            const std::filesystem::path& shm_dir,
                                            const std::string& writer_name) {
    auto meta =
        std::make_shared<shm::ChannelMeta>(shm::ChannelMeta::open_only(channel_name, shm_dir));
    return create(meta, writer_name);
}

template <AllowedWriteLock WriteLock>
std::byte* Writer<WriteLock>::open_frame(DzFrameType frame_type, uint32_t data_size) noexcept {
    write_lock_.lock();

    pending_size_ = data_size + sizeof(DzFrameHeader);
    if (pending_size_ % 8 != 0) {
        LastError::set(DZ_EC_SHM_ALIGN_ERROR, "write: frame size {} not 8-byte aligned",
                       pending_size_);
        pending_size_ = 0;
        write_lock_.unlock();
        return nullptr;
    }

    // 单帧不得超过单页大小: 跨页写入会导致 page_.data() 之外内存越界
    if (pending_size_ > page_size_) {
        LastError::set(DZ_EC_SHM_OPEN_FAILED,
                       "write: frame size {} exceeds page size {}",
                       pending_size_, page_size_);
        pending_size_ = 0;
        write_lock_.unlock();
        return nullptr;
    }

    if constexpr (IS_NULL_WRITE_LOCK) {
        pending_offset_ = offset_in_page_;
        pending_page_id_ = current_page_id_;
    } else {
        uint64_t pos = next_write_pos_->load(boost::memory_order_relaxed);
        pending_offset_ = pos % page_size_;
        pending_page_id_ = pos / page_size_;

        // 多写者: 其他写者可能已把 nwp 推进到新页, 而 page_ 仍指向本写者上次的页。
        // 写入前必须对齐到 pending_page_id_ 对应的映射, 否则帧(和填充标记)会写进陈旧
        // 页文件: 目标页对应区间从未被写入, 读者读到坏帧头后静默丢帧/卡死
        // (回归测试: WriterTest.MultiWriterWritesToStalePageAfterOtherWriterCrossPage)。
        // 同页连写时该判断恒假, 分支预测命中, 无实际开销。
        if (pending_page_id_ != page_.page_id()) {
            auto cur_page = page_pool_.get_page(pending_page_id_);
            if (!cur_page) {
                LastError::set(DZ_EC_SHM_OPEN_FAILED, "write: failed to get page {}",
                               pending_page_id_);
                pending_size_ = 0;
                write_lock_.unlock();
                return nullptr;
            }
            page_ = std::move(cur_page.value());
        }
    }

    uint64_t remaining = page_size_ - pending_offset_;

    if (pending_size_ > remaining) {
        if (remaining >= sizeof(DzFrameHeader)) {
            write_padding(page_.data() + pending_offset_, remaining);
        }
        pending_page_id_++;
        pending_offset_ = 0;
        auto new_page = page_pool_.get_page(pending_page_id_);
        if (!new_page) {
            LastError::set(DZ_EC_SHM_OPEN_FAILED, "write: failed to get page {}", pending_page_id_);
            pending_size_ = 0;
            write_lock_.unlock();
            return nullptr;
        }
        page_ = std::move(new_page.value());
    }

    auto* hdr = reinterpret_cast<DzFrameHeader*>(page_.data() + pending_offset_);
    hdr->frame_type = frame_type;
    hdr->frame_size = static_cast<uint32_t>(pending_size_);
    return page_.data() + pending_offset_ + sizeof(DzFrameHeader);
}

template <AllowedWriteLock WriteLock>
void Writer<WriteLock>::prefetch_pages(uint64_t count) {
    auto page_id = page_.page_id();
    for (uint64_t i = 0; i < count; ++i) {
        ++page_id;
        page_pool_.prefetch_page(page_id);
    }
}

template <AllowedWriteLock WriteLock>
void Writer<WriteLock>::prefetch_for_bytes(uint64_t bytes) {
    const auto target_pos = next_write_pos_->load(boost::memory_order_relaxed) + bytes;
    const auto target_page_id = target_pos / page_size_;
    if (target_page_id > page_.page_id()) {
        prefetch_pages(target_page_id - page_.page_id());
    }
}

template <AllowedWriteLock WriteLock>
void Writer<WriteLock>::close_old_pages() noexcept {
    page_pool_.close_pages_before(page_.page_id());
    try {
        meta_->set_writer_page_index(writer_name_, page_.page_id());
    } catch (...) {
        // key 不存在 = master bug; 按 noexcept 契约吞掉异常
    }
}

template class Writer<ProcessMutex>;
template class Writer<NullLock>;

}  // namespace dztrader::shm
