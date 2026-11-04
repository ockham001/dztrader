/**
 * @file order_id_meta.h
 * @brief 跨进程共享的订单ID原子计数器
 *
 * 仿照 ChannelMeta 的 pImpl 模式。共享内存文件布局：shm_dir/<name>/order_id.dat
 * 仅提供 creator 角色 (open_or_create)：持有进程负责创建共享对象并保证其存活；
 * 计数器经 generate / ensure_at_least / current 访问。
 */
#ifndef DZTRADER_SHM_ORDER_ID_META_H_
#define DZTRADER_SHM_ORDER_ID_META_H_

#include <boost/atomic/ipc_atomic.hpp>
#include <dztrader/shm/process_mutex.h>
#include <filesystem>
#include <memory>
#include <dztrader/data_type.h>

namespace dztrader::shm {

using ShmAtomicOrderId = boost::ipc_atomic<DzOrderId>;

namespace order_id_meta_internal {
class OrderIdMetaImpl;
}  // namespace order_id_meta_internal

class OrderIdMeta {
public:
    [[nodiscard]] static OrderIdMeta open_or_create(const std::string& name,
                                                    const std::filesystem::path& shm_dir);

    ~OrderIdMeta();
    OrderIdMeta(OrderIdMeta&&) noexcept;
    OrderIdMeta& operator=(OrderIdMeta&&) noexcept;
    OrderIdMeta(const OrderIdMeta&) = delete;
    OrderIdMeta& operator=(const OrderIdMeta&) = delete;

    [[nodiscard]] DzOrderId generate() noexcept {
        return id_->fetch_add(1, boost::memory_order_relaxed);
    }

    [[nodiscard]] DzOrderId current() const noexcept {
        return id_->load(boost::memory_order_relaxed);
    }

    /// 确保计数器至少达到 value (fetch_max 语义, 单调不减).
    /// 用途: master 启动建库后按交易库 MAX(order_id)+1 播种共享计数器 (ADR 0041 §6);
    /// 重复/并发调用幂等。返回调用前的值 (仅日志参考, 无需强制使用).
    DzOrderId ensure_at_least(DzOrderId value) noexcept {
        DzOrderId prev = id_->load(boost::memory_order_relaxed);
        while (prev < value) {
            if (id_->compare_exchange_weak(prev, value, boost::memory_order_relaxed)) {
                return prev;
            }
            // CAS 失败: prev 已更新为最新值, 若仍 < value 继续重试
        }
        return prev;
    }

private:
    using OrderIdMetaImpl = order_id_meta_internal::OrderIdMetaImpl;
    explicit OrderIdMeta(std::unique_ptr<OrderIdMetaImpl> impl);
    ShmAtomicOrderId* id_;
    std::unique_ptr<OrderIdMetaImpl> impl_;
};

}  // namespace dztrader::shm
#endif  // DZTRADER_SHM_ORDER_ID_META_H_
