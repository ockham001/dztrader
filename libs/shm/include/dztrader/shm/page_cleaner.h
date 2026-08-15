#ifndef DZTRADER_SHM_PAGE_CLEANER_H_
#define DZTRADER_SHM_PAGE_CLEANER_H_

#include <cstdint>
#include <memory>

#include <dztrader/shm/channel_meta.h>

namespace dztrader::shm {

/// page 文件清理策略
struct CleanupPolicy {
    uint64_t max_page_count = 0;      ///< 保留最近 N 个 page 文件，0=不限
    uint64_t max_page_age_hours = 0;  ///< 删除超过此小时数的 page 文件，0=不限
    // 两者都为 0 时不清理，两者都非 0 时同时生效（满足任一即删除）
};

/// 主进程持有，定时扫描并删除旧 page 文件
class PageCleaner {
public:
    PageCleaner(std::shared_ptr<ChannelMeta> meta, CleanupPolicy policy);

    /// 执行一次清理，返回删除的文件数量
    /// 从最旧的文件开始删除；删除下限为 min(reader/writer page_index, active)，
    /// 确保任何消费者仍使用的页不被删。remove 失败则跳过（告警）不中断清理
    size_t cleanup();

private:
    /// 从 next_write_pos 推算当前活跃的 page_id
    [[nodiscard]] uint64_t active_page_id() const noexcept;

    std::shared_ptr<ChannelMeta> meta_;
    CleanupPolicy policy_;
};

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_PAGE_CLEANER_H_
