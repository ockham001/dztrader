#include <dztrader/shm/reader.h>
#include <memory>

namespace dztrader::shm {

Reader::Reader(std::shared_ptr<ChannelMeta> meta,
               AtomicU64* nwp,
               uint64_t page_size,
               uint64_t read_pos,
               uint64_t current_page_id,
               uint64_t offset_in_page,
               Page page,
               PagePool page_pool,
               std::string reader_name)
    : meta_(std::move(meta)),
      next_write_pos_(nwp),
      page_size_(page_size),
      read_pos_(read_pos),
      current_page_id_(current_page_id),
      offset_in_page_(offset_in_page),
      page_(std::move(page)),
      page_pool_(std::move(page_pool)),
      reader_name_(std::move(reader_name)) {}

Reader Reader::create(std::shared_ptr<ChannelMeta> meta, const std::string& reader_name) {
    auto* nwp = meta->next_write_pos();
    const uint64_t page_size = meta->page_size();
    // 读者只开不建: 页文件由写者在 nwp 推进前创建, 缺失/尺寸不符即报错而非伪造零页
    PagePool page_pool(meta, /*read_only=*/true);

    const uint64_t read_pos = nwp->load(boost::memory_order_acquire);
    const uint64_t current_page_id = read_pos / page_size;
    const uint64_t offset_in_page = read_pos % page_size;

    auto page_opt = page_pool.get_page(current_page_id);
    if (!page_opt) {
        throw Exception(DZ_EC_SHM_OPEN_FAILED, "reader create: failed to get page {}",
                        current_page_id);
    }

    return Reader(std::move(meta), nwp, page_size, read_pos, current_page_id, offset_in_page,
                  std::move(page_opt.value()), std::move(page_pool), reader_name);
}

Reader Reader::create(const std::string& channel_name, const std::filesystem::path& shm_dir,
                      const std::string& reader_name) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_only(channel_name, shm_dir));
    return create(meta, reader_name);
}

const std::byte* Reader::next_frame() noexcept {
    const uint64_t current_write_pos = next_write_pos_->load(boost::memory_order_acquire);

    if (read_pos_ >= current_write_pos) {
        return nullptr;
    }

    uint64_t offset = offset_in_page_;
    uint64_t page_id = current_page_id_;
    uint64_t rpos = read_pos_;

    while (page_size_ - offset < sizeof(DzFrameHeader)) {
        const uint64_t skip = page_size_ - offset;
        rpos += skip;
        page_id++;
        offset = 0;
        auto new_page = page_pool_.get_page(page_id);
        if (!new_page) {
            LastError::set(DZ_EC_SHM_READ_INVALID, "next_frame: failed to get page {}", page_id);
            return nullptr;
        }
        page_ = std::move(new_page.value());
        current_page_id_ = page_id;
        offset_in_page_ = 0;
        read_pos_ = rpos;
    }

    auto* hdr = reinterpret_cast<DzFrameHeader*>(page_.data() + offset);

    while (hdr->frame_type == DZ_FRAME_INVALID_FILL) {
        const uint32_t frame_sz = hdr->frame_size;
        if (frame_sz < sizeof(DzFrameHeader) || frame_sz > page_size_) {
            LastError::set(DZ_EC_SHM_FRAME_INVALID, "next_frame: invalid fill frame size {}",
                           frame_sz);
            return nullptr;
        }

        offset += frame_sz;
        rpos += frame_sz;

        if (page_size_ - offset < sizeof(DzFrameHeader)) {
            rpos += (page_size_ - offset);
            page_id++;
            offset = 0;
            auto new_page = page_pool_.get_page(page_id);
            if (!new_page) {
                LastError::set(DZ_EC_SHM_READ_INVALID, "next_frame: failed to get page {}",
                               page_id);
                return nullptr;
            }
            page_ = std::move(new_page.value());
            current_page_id_ = page_id;
            offset_in_page_ = 0;
            read_pos_ = rpos;
        }

        hdr = reinterpret_cast<DzFrameHeader*>(page_.data() + offset);
    }

    offset_in_page_ = offset + hdr->frame_size;
    read_pos_ = rpos + hdr->frame_size;
    current_page_id_ = page_id;

    return reinterpret_cast<std::byte*>(hdr);
}

void Reader::prefetch_pages(uint64_t num_pages) {
    auto page_id = page_.page_id();
    for (uint64_t i = 0; i < num_pages; ++i) {
        ++page_id;
        page_pool_.prefetch_page(page_id);
    }
}

void Reader::prefetch_for_bytes(uint64_t bytes) {
    const auto target_pos = next_write_pos_->load(boost::memory_order_relaxed) + bytes;
    const auto target_page_id = target_pos / page_size_;
    if (target_page_id > page_.page_id()) {
        prefetch_pages(target_page_id - page_.page_id());
    }
}

void Reader::release_old_pages() noexcept {
    page_pool_.close_pages_before(current_page_id_);
    try {
        meta_->set_reader_page_index(reader_name_, current_page_id_);
    } catch (...) {
        // 找不到 key 属 master bug；release 不抛出，忽略（开发期测试会暴露）
    }
}

}  // namespace dztrader::shm
