#ifndef DZTRADER_SHM_CHANNEL_META_H_
#define DZTRADER_SHM_CHANNEL_META_H_

#include <boost/atomic/ipc_atomic.hpp>
#include <dztrader/shm/common.h>
#include <dztrader/shm/process_mutex.h>
#include <filesystem>
#include <vector>
#include <dztrader/data_type.h>

namespace dztrader::shm {

// next_write_pos 为 uint64，理论上限 1.8e19 字节，实际 tick 量远低于此
// （百万 tick/s 运行百年仍余两个数量级）。文件持续增长是设计意图，非缺陷；
// 满了删文件 + 重启恢复，不做运行期上限判断。
//
// 锁不变量：meta 锁（channel_name + ".meta"）合并原 page 锁，保护
// readers/writers map 与 next_write_pos/page_size 元数据。全局锁层级
// write→meta：open_or_create ctor 是唯一同时持双锁处（先 write 后 meta），
// 其余路径仅持 meta。Linux robust mutex 非重入，故 init 路径不再 re-lock。
using AtomicU64 = boost::ipc_atomic<uint64_t>;

namespace channel_meta_internal {
class ChannelMetaImpl;
}  // namespace channel_meta_internal

class ChannelMeta {
public:
    [[nodiscard]] static ChannelMeta open_or_create(const ChannelConfig& config);
    [[nodiscard]] static ChannelMeta open_only(const std::string& channel_name,
                                               const std::filesystem::path& shm_dir);

    ~ChannelMeta();
    ChannelMeta(ChannelMeta&&) noexcept;
    ChannelMeta& operator=(ChannelMeta&&) noexcept;
    ChannelMeta(const ChannelMeta&) = delete;
    ChannelMeta& operator=(const ChannelMeta&) = delete;

    [[nodiscard]] const std::string& channel_name() const noexcept;
    [[nodiscard]] std::filesystem::path page_dir() const;

    [[nodiscard]] uint64_t page_size() const noexcept;
    [[nodiscard]] AtomicU64* next_write_pos() noexcept;

    [[nodiscard]] ProcessMutex create_write_mutex() const;
    /// 与 ChannelMetaImpl 内部的 meta 锁同名（同一内核对象），供 PagePool/PageCleaner 使用
    [[nodiscard]] ProcessMutex create_meta_mutex() const;

    // reader/writer map：master 创建/删除条目；子进程只调 set_*_page_index
    void clear_readers();
    void remove_reader(const std::string& name);
    [[nodiscard]] bool add_reader(const std::string& name, uint64_t pid);
    void clear_writers();
    void remove_writer(const std::string& name);
    [[nodiscard]] bool add_writer(const std::string& name, uint64_t pid);

    /// 子进程更新自己条目的 page_index；找不到 key 抛 Exception
    void set_reader_page_index(const std::string& name, uint64_t idx);
    void set_writer_page_index(const std::string& name, uint64_t idx);

    /// 空 map 返回 UINT64_MAX
    [[nodiscard]] uint64_t min_reader_page_index() const;
    [[nodiscard]] uint64_t min_writer_page_index() const;

    /// 调用方必须已持有 meta 锁（与 create_meta_mutex 同一内核对象）。空 map 返回 UINT64_MAX。
    [[nodiscard]] uint64_t min_reader_page_index_locked() const;
    [[nodiscard]] uint64_t min_writer_page_index_locked() const;

    [[nodiscard]] std::vector<std::string> reader_names() const;

private:
    using ChannelMetaImpl = channel_meta_internal::ChannelMetaImpl;
    explicit ChannelMeta(std::unique_ptr<ChannelMetaImpl> impl);
    std::unique_ptr<ChannelMetaImpl> impl_;
};

}  // namespace dztrader::shm
#endif  // DZTRADER_SHM_CHANNEL_META_H_