#include <boost/interprocess/creation_tags.hpp>

#include <algorithm>
#include <boost/interprocess/containers/map.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <cstdint>
#include <cstring>
#include <dztrader/core/encoding.h>
#include <dztrader/core/exception.h>
#include <dztrader/error.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/process_mutex.h>
#include <dztrader/struct.h>
#include <format>
#include <fstream>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>
#include <vector>

namespace bip = boost::interprocess;

namespace dztrader::shm {

namespace {

constexpr uint64_t KB = 1024ULL;
constexpr uint64_t MB = KB * 1024ULL;
constexpr uint64_t GB = MB * 1024ULL;

/// 启动结构校验: 活跃页帧链抽查的帧数上限 (防极端小帧场景拖慢启动)。
constexpr uint64_t kMaxScanFrames = 10000;

}  // namespace

namespace channel_meta_internal {

class ChannelMetaImpl {
    using IpcCharAllocator =
        boost::interprocess::allocator<char,
                                       boost::interprocess::managed_shared_memory::segment_manager>;
    using IpcString =
        boost::interprocess::basic_string<char, std::char_traits<char>, IpcCharAllocator>;

    struct ReaderInfo {
        uint64_t pid;
        uint64_t page_index;
    };
    struct WriterInfo {
        uint64_t pid;
        uint64_t page_index;
    };
    using IpcReaderMap = boost::interprocess::map<
        IpcString, ReaderInfo, std::less<IpcString>,
        boost::interprocess::allocator<std::pair<const IpcString, ReaderInfo>,
                                       boost::interprocess::managed_shared_memory::segment_manager>>;
    using IpcWriterMap = boost::interprocess::map<
        IpcString, WriterInfo, std::less<IpcString>,
        boost::interprocess::allocator<std::pair<const IpcString, WriterInfo>,
                                       boost::interprocess::managed_shared_memory::segment_manager>>;

public:
    ChannelMetaImpl(const ChannelConfig& config)
        : page_size_{config.page_size},
          channel_meta_lock_{config.channel_name + ".meta"},
          is_creator_{true},
          channel_name_{config.channel_name},
          shm_dir_{config.shm_dir} {
        validate_config(config);
        open_or_create_segment(config);

        // 全局锁层级 write→meta：ctor 是唯一同时持双锁处
        auto write_mutex = create_write_mutex();
        std::scoped_lock<ProcessMutex> lk_write(write_mutex);
        std::scoped_lock<ProcessMutex> lk_meta(channel_meta_lock_);
        init_shared_objects_locked();
        ensure_initial_frame_locked();
        init_readers_writers_locked();
        validate_active_page_locked();
    }

    ChannelMetaImpl(const std::string& channel_name, const std::filesystem::path& shm_dir)
        : channel_meta_lock_{channel_name + ".meta"},
          channel_name_{channel_name},
          shm_dir_{shm_dir} {
        open_existing_segment();

        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        find_shared_objects_locked();
    }

    ~ChannelMetaImpl() = default;
    ChannelMetaImpl(const ChannelMetaImpl&) = delete;
    ChannelMetaImpl& operator=(const ChannelMetaImpl&) = delete;
    ChannelMetaImpl(ChannelMetaImpl&&) = delete;
    ChannelMetaImpl& operator=(ChannelMetaImpl&&) = delete;

    [[nodiscard]] AtomicU64* next_write_pos() noexcept { return next_write_pos_; }

    [[nodiscard]] bool add_reader(const std::string& name, uint64_t pid) {
        if (!is_creator_) {
            throw Exception(DZ_EC_SHM_SUBSCRIBER_FAILED, "add_reader requires creator role");
        }
        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        auto key = IpcString(name.c_str(), segment_.get_segment_manager());
        auto [it, ok] = readers_->emplace(std::move(key), ReaderInfo{pid, 0});
        (void)it;
        return ok;
    }
    void remove_reader(const std::string& name) {
        if (!is_creator_) {
            throw Exception(DZ_EC_SHM_SUBSCRIBER_FAILED, "remove_reader requires creator role");
        }
        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        readers_->erase(IpcString(name.c_str(), segment_.get_segment_manager()));
    }
    void clear_readers() {
        if (!is_creator_) {
            throw Exception(DZ_EC_SHM_SUBSCRIBER_FAILED, "clear_readers requires creator role");
        }
        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        readers_->clear();
    }
    [[nodiscard]] bool add_writer(const std::string& name, uint64_t pid) {
        if (!is_creator_) {
            throw Exception(DZ_EC_SHM_SUBSCRIBER_FAILED, "add_writer requires creator role");
        }
        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        auto key = IpcString(name.c_str(), segment_.get_segment_manager());
        auto [it, ok] = writers_->emplace(std::move(key), WriterInfo{pid, 0});
        (void)it;
        return ok;
    }
    void remove_writer(const std::string& name) {
        if (!is_creator_) {
            throw Exception(DZ_EC_SHM_SUBSCRIBER_FAILED, "remove_writer requires creator role");
        }
        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        writers_->erase(IpcString(name.c_str(), segment_.get_segment_manager()));
    }
    void clear_writers() {
        if (!is_creator_) {
            throw Exception(DZ_EC_SHM_SUBSCRIBER_FAILED, "clear_writers requires creator role");
        }
        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        writers_->clear();
    }

    void set_reader_page_index(const std::string& name, uint64_t idx) {
        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        auto it = readers_->find(IpcString(name.c_str(), segment_.get_segment_manager()));
        if (it == readers_->end()) {
            throw Exception(DZ_EC_SHM_SUBSCRIBER_FAILED,
                            "set_reader_page_index: key not found: {}", name);
        }
        it->second.page_index = idx;
    }
    void set_writer_page_index(const std::string& name, uint64_t idx) {
        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        auto it = writers_->find(IpcString(name.c_str(), segment_.get_segment_manager()));
        if (it == writers_->end()) {
            throw Exception(DZ_EC_SHM_SUBSCRIBER_FAILED,
                            "set_writer_page_index: key not found: {}", name);
        }
        it->second.page_index = idx;
    }

    [[nodiscard]] uint64_t min_reader_page_index() const {
        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        return min_reader_page_index_locked();
    }
    [[nodiscard]] uint64_t min_writer_page_index() const {
        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        return min_writer_page_index_locked();
    }

    [[nodiscard]] uint64_t min_reader_page_index_locked() const {
        if (readers_->empty()) { return UINT64_MAX; }
        uint64_t m = UINT64_MAX;
        for (const auto& [k, v] : *readers_) { (void)k; m = std::min(m, v.page_index); }
        return m;
    }
    [[nodiscard]] uint64_t min_writer_page_index_locked() const {
        if (writers_->empty()) { return UINT64_MAX; }
        uint64_t m = UINT64_MAX;
        for (const auto& [k, v] : *writers_) { (void)k; m = std::min(m, v.page_index); }
        return m;
    }

    [[nodiscard]] std::vector<std::string> reader_names() const {
        std::scoped_lock<ProcessMutex> lk(channel_meta_lock_);
        std::vector<std::string> out;
        out.reserve(readers_->size());
        for (const auto& [k, v] : *readers_) {
            (void)v;
            out.emplace_back(k.c_str());
        }
        return out;
    }

    [[nodiscard]] uint64_t page_size() const noexcept { return page_size_; }

    [[nodiscard]] const std::string& channel_name() const noexcept { return channel_name_; }

    [[nodiscard]] std::filesystem::path page_dir() const { return shm_dir_ / channel_name_; }

    [[nodiscard]] ProcessMutex create_write_mutex() const {
        return ProcessMutex(channel_name_ + ".write");
    }

    [[nodiscard]] ProcessMutex create_meta_mutex() const {
        return ProcessMutex(channel_name_ + ".meta");
    }

private:
    static void validate_config(const ChannelConfig& config) {
        if (config.meta_file_size < 1 * MB || config.meta_file_size > 128 * MB) {
            throw Exception(DZ_EC_SHM_CREATE_FAILED,
                            "meta_file_size is invalid, min 1MB, max 128MB");
        }
        if (config.page_size < 1 * MB || config.page_size > 64 * GB) {
            throw Exception(DZ_EC_SHM_CREATE_FAILED, "page_size is invalid, min 1MB, max 64GB");
        }
    }

    void open_or_create_segment(const ChannelConfig& config) {
        auto meta_dir = shm_dir_ / config.channel_name;
        std::filesystem::create_directories(meta_dir);
        auto meta_file_path = meta_dir / "meta.dat";

        // meta_file_size 不匹配时删除 meta 文件 + 所有 page 文件后重建
        // 通道存续期间 meta_file_size 不变,只有 open_or_create 时才会检查
        // page 文件与 meta.dat 同目录(page_dir() = shm_dir_/channel_name_),扩展名 .dat
        if (std::filesystem::exists(meta_file_path)) {
            std::error_code ec;
            auto actual_size = std::filesystem::file_size(meta_file_path, ec);
            if (!ec && actual_size != config.meta_file_size) {
                // 删除 meta 文件
                std::filesystem::remove(meta_file_path, ec);
                // 删除所有 page 文件(page_size 改变时旧 page 也无效)
                if (std::filesystem::exists(meta_dir, ec)) {
                    for (const auto& entry : std::filesystem::directory_iterator(meta_dir)) {
                        if (entry.is_regular_file() && entry.path().extension() == ".dat") {
                            std::error_code rm_ec;
                            std::filesystem::remove(entry.path(), rm_ec);
                        }
                    }
                }
            }
        }

        segment_ = bip::managed_mapped_file{bip::open_or_create, meta_file_path.string().c_str(),
                                            config.meta_file_size};
    }

    void open_existing_segment() {
        auto meta_path = shm_dir_ / channel_name_ / "meta.dat";
        segment_ = bip::managed_mapped_file{bip::open_only, meta_path.string().c_str()};
    }

    void init_shared_objects_locked() {
        next_write_pos_ = segment_.find_or_construct<AtomicU64>("next_write_pos")(0);
        if (!next_write_pos_) {
            throw Exception(DZ_EC_SHM_CREATE_FAILED, "next_write_pos is null");
        }
        auto* page_size_ptr = segment_.find_or_construct<uint64_t>("page_size")(page_size_);
        if (!page_size_ptr) {
            throw Exception(DZ_EC_SHM_CREATE_FAILED, "page_size is null");
        }
        if (page_size_ != *page_size_ptr) {
            // meta+write 已由 ctor 持有，直接操作
            clean_page_files_locked();
            next_write_pos_->store(0, boost::memory_order_release);
            *page_size_ptr = page_size_;
            write_initial_frame();
        }
    }

    void ensure_initial_frame_locked() {
        if (*next_write_pos_ == 0) {
            // meta+write 已由 ctor 持有
            write_initial_frame();
        }
    }

    void init_readers_writers_locked() {
        readers_ = segment_.find_or_construct<IpcReaderMap>("readers")(
            std::less<IpcString>(),
            boost::interprocess::allocator<std::pair<const IpcString, ReaderInfo>,
                                           boost::interprocess::managed_shared_memory::segment_manager>(
                segment_.get_segment_manager()));
        if (!readers_) {
            throw Exception(DZ_EC_SHM_CREATE_FAILED, "readers is null");
        }
        writers_ = segment_.find_or_construct<IpcWriterMap>("writers")(
            std::less<IpcString>(),
            boost::interprocess::allocator<std::pair<const IpcString, WriterInfo>,
                                           boost::interprocess::managed_shared_memory::segment_manager>(
                segment_.get_segment_manager()));
        if (!writers_) {
            throw Exception(DZ_EC_SHM_CREATE_FAILED, "writers is null");
        }
    }

    void find_shared_objects_locked() {
        auto* page_size_ptr = segment_.find<uint64_t>("page_size").first;
        if (!page_size_ptr) {
            throw Exception(DZ_EC_SHM_OPEN_FAILED, "page_size is null");
        }
        page_size_ = *page_size_ptr;

        next_write_pos_ = segment_.find<AtomicU64>("next_write_pos").first;
        if (!next_write_pos_) {
            throw Exception(DZ_EC_SHM_OPEN_FAILED, "next_write_pos is null");
        }

        readers_ = segment_.find<IpcReaderMap>("readers").first;
        if (!readers_) {
            throw Exception(DZ_EC_SHM_OPEN_FAILED, "readers is null");
        }
        writers_ = segment_.find<IpcWriterMap>("writers").first;
        if (!writers_) {
            throw Exception(DZ_EC_SHM_OPEN_FAILED, "writers is null");
        }
    }

    void clean_page_files_locked() const {
        auto dir = page_dir();
        if (!std::filesystem::exists(dir)) {
            return;
        }
        std::vector<std::filesystem::path> page_files;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".dat") {
                page_files.push_back(entry.path());
            }
        }
        std::ranges::sort(page_files);

        for (const auto& pf : page_files) {
            std::error_code ec;
            std::filesystem::remove(pf, ec);
            if (ec) {
                throw Exception(DZ_EC_SHM_FILE_REMOVE_FAILED,
                                "page file remove failed during config change: path={} err={}",
                                pf.string(), dztrader::to_utf8_from_system(ec.message()));
            }
        }
    }

    void write_initial_frame() {
        auto file_dir = page_dir();
        auto file_path = file_dir / std::format("{:08d}.dat", 0);

        if (!std::filesystem::exists(file_dir)) {
            std::filesystem::create_directories(file_dir);
        }
        if (!std::filesystem::exists(file_path)) {
            std::ofstream ofs(file_path, std::ios::binary);  // NOLINT(misc-const-correctness)
            if (!ofs) {
                throw Exception(DZ_EC_SHM_CREATE_FAILED, "create initial page failed: path={}",
                                file_path.string());
            }
        }
        if (std::filesystem::file_size(file_path) != page_size_) {
            std::filesystem::resize_file(file_path, page_size_);
        }

        namespace bip2 = boost::interprocess;
        bip2::file_mapping mfile{file_path.string().c_str(),
                                 bip2::read_write};  // NOLINT(misc-const-correctness)
        // NOLINTNEXTLINE(misc-const-correctness)
        bip2::mapped_region region{mfile, bip2::read_write, 0, sizeof(DzFrameHeader)};

        auto* header = static_cast<DzFrameHeader*>(region.get_address());
        std::memset(header, 0, sizeof(DzFrameHeader));
        header->frame_type = DZ_FRAME_INVALID_FILL;
        header->frame_size = sizeof(DzFrameHeader);

        next_write_pos_->store(sizeof(DzFrameHeader), boost::memory_order_release);
    }

    /// 启动结构校验 (仅 open_or_create, master 启动时一次性):
    /// ① nwp 对齐与下限 (防写者沿错位偏移续写, 之后所有帧全坏)
    /// ② 活跃页文件存在且尺寸正确 (防外部删除/磁盘损坏被静默"重建零页"掩盖)
    /// ③ 活跃页帧链抽查 (页首走到 nwp, 帧数上限 kMaxScanFrames, 纯诊断)
    /// v1 只记日志不修复: 失败路径返回, 不抛异常、不毁数据。
    void validate_active_page_locked() {
        const uint64_t nwp = next_write_pos_->load(boost::memory_order_relaxed);
        if (nwp % 8 != 0 || nwp < sizeof(DzFrameHeader)) {
            SPDLOG_ERROR(
                "shm meta corrupted | reason=next_write_pos_invalid nwp={} channel={}", nwp,
                channel_name_);
            return;
        }

        const uint64_t page_id = nwp / page_size_;
        auto file_path = page_dir() / std::format("{:08d}.dat", page_id);
        std::error_code ec;
        if (!std::filesystem::exists(file_path, ec)) {
            SPDLOG_ERROR(
                "shm meta corrupted | reason=active_page_missing page={} channel={}", page_id,
                channel_name_);
            return;
        }
        const auto file_size = std::filesystem::file_size(file_path, ec);
        if (ec || file_size != page_size_) {
            SPDLOG_ERROR(
                "shm meta corrupted | reason=active_page_size_mismatch page={} size={} expected={} channel={}",
                page_id, file_size, page_size_, channel_name_);
            return;
        }

        try {
            namespace bip2 = boost::interprocess;
            bip2::file_mapping mfile{file_path.string().c_str(), bip2::read_only};
            bip2::mapped_region region{mfile, bip2::read_only, 0, page_size_};
            const auto* base = static_cast<const std::byte*>(region.get_address());
            const uint64_t end = nwp % page_size_;
            uint64_t offset = 0;
            uint64_t scanned = 0;
            while (offset < end && scanned < kMaxScanFrames) {
                if (page_size_ - offset < sizeof(DzFrameHeader)) {
                    SPDLOG_WARN(
                        "shm chain broken | reason=trailing_header page={} offset={} channel={}",
                        page_id, offset, channel_name_);
                    return;
                }
                const auto* hdr = reinterpret_cast<const DzFrameHeader*>(base + offset);
                const uint32_t fs = hdr->frame_size;
                if (fs < sizeof(DzFrameHeader) || fs % 8 != 0 || fs > page_size_ - offset) {
                    SPDLOG_WARN(
                        "shm chain broken | reason=bad_frame_size page={} offset={} frame_size={} channel={}",
                        page_id, offset, fs, channel_name_);
                    return;
                }
                offset += fs;
                ++scanned;
            }
            if (scanned >= kMaxScanFrames && offset < end) {
                SPDLOG_INFO("shm chain scan capped | page={} scanned={} channel={}", page_id,
                            scanned, channel_name_);
                return;
            }
            if (offset != end) {
                SPDLOG_WARN(
                    "shm chain broken | reason=chain_ends_short page={} offset={} nwp_in_page={} channel={}",
                    page_id, offset, end, channel_name_);
            }
        } catch (const std::exception& e) {
            SPDLOG_WARN("shm chain scan failed | error=\"{}\" page={} channel={}", e.what(),
                        page_id, channel_name_);
        }
    }

    AtomicU64* next_write_pos_ = nullptr;
    uint64_t page_size_ = 0;
    mutable ProcessMutex channel_meta_lock_;
    IpcReaderMap* readers_ = nullptr;
    IpcWriterMap* writers_ = nullptr;
    bool is_creator_ = false;
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    boost::interprocess::managed_mapped_file segment_;
};

}  // namespace channel_meta_internal

ChannelMeta::~ChannelMeta() = default;
ChannelMeta::ChannelMeta(ChannelMeta&&) noexcept = default;
ChannelMeta& ChannelMeta::operator=(ChannelMeta&&) noexcept = default;

ChannelMeta::ChannelMeta(std::unique_ptr<ChannelMetaImpl> impl)
    : impl_{std::move(impl)} {}

ChannelMeta ChannelMeta::open_or_create(const ChannelConfig& config) {
    auto impl = std::make_unique<ChannelMetaImpl>(config);
    return ChannelMeta(std::move(impl));
}

ChannelMeta ChannelMeta::open_only(const std::string& channel_name,
                                   const std::filesystem::path& shm_dir) {
    auto impl = std::make_unique<ChannelMetaImpl>(channel_name, shm_dir);
    return ChannelMeta(std::move(impl));
}

const std::string& ChannelMeta::channel_name() const noexcept { return impl_->channel_name(); }

std::filesystem::path ChannelMeta::page_dir() const { return impl_->page_dir(); }

[[nodiscard]] uint64_t ChannelMeta::page_size() const noexcept {
    static_assert(noexcept(impl_->page_size()));
    return impl_->page_size();
}

AtomicU64* ChannelMeta::next_write_pos() noexcept {
    static_assert(noexcept(impl_->next_write_pos()));
    return impl_->next_write_pos();
}

ProcessMutex ChannelMeta::create_write_mutex() const { return impl_->create_write_mutex(); }
ProcessMutex ChannelMeta::create_meta_mutex() const { return impl_->create_meta_mutex(); }

void ChannelMeta::clear_readers() { impl_->clear_readers(); }
void ChannelMeta::remove_reader(const std::string& n) { impl_->remove_reader(n); }
bool ChannelMeta::add_reader(const std::string& n, uint64_t pid) { return impl_->add_reader(n, pid); }
void ChannelMeta::clear_writers() { impl_->clear_writers(); }
void ChannelMeta::remove_writer(const std::string& n) { impl_->remove_writer(n); }
bool ChannelMeta::add_writer(const std::string& n, uint64_t pid) { return impl_->add_writer(n, pid); }
void ChannelMeta::set_reader_page_index(const std::string& n, uint64_t i) {
    impl_->set_reader_page_index(n, i);
}
void ChannelMeta::set_writer_page_index(const std::string& n, uint64_t i) {
    impl_->set_writer_page_index(n, i);
}
uint64_t ChannelMeta::min_reader_page_index() const { return impl_->min_reader_page_index(); }
uint64_t ChannelMeta::min_writer_page_index() const { return impl_->min_writer_page_index(); }
uint64_t ChannelMeta::min_reader_page_index_locked() const { return impl_->min_reader_page_index_locked(); }
uint64_t ChannelMeta::min_writer_page_index_locked() const { return impl_->min_writer_page_index_locked(); }
std::vector<std::string> ChannelMeta::reader_names() const { return impl_->reader_names(); }

}  // namespace dztrader::shm
