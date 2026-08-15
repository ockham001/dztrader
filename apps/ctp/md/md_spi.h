#ifndef DZTRADER_CTP_MD_SPI_H_
#define DZTRADER_CTP_MD_SPI_H_

#include <cstdint>
#include <filesystem>

#include <ThostFtdcMdApi.h>

#include <dztrader/data_type.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/spin_lock.h>
#include <dztrader/shm/writer.h>

#include "common/ctp_events.h"

namespace dztrader::ctp {

/// CTP 行情 SPI 实现。
///
/// 职责:
/// - 实现 CThostFtdcMdSpi 回调接口, 接收 CTP 推送的行情/响应
/// - 将非行情回调 (连接/登录/订阅确认等) 打包为事件, 经 lockfree SPSC 队列转发给主线程
/// - 将行情回调 (OnRtnDepthMarketData) 直接写入共享内存, 供订阅者消费
///
/// 线程模型:
/// - CTP MD API 的所有 SPI 回调在同一线程触发 (CTP 内部工作线程)
/// - 主线程消费事件队列, 并周期性调用 refresh_subscribers/prefetch_*/close_old_pages
/// - 跨线程共享资源:
///   * md_writer_        : SPI 线程写, 主线程维护, 由 thread_lock_ 互斥保护
///   * days_since_epoch_  : 仅 SPI 线程读写, 无需加锁 (依赖 CTP 单线程模型)
///   * event_queue_       : 构造后不可变; push 由 SPI 线程独占 (单生产者)
class MdSpi : public CThostFtdcMdSpi {
public:
    /// 构造 SPI 并创建共享内存 writer。
    /// @param name          通道名 (用于 SHM 文件名与 writer 名)
    /// @param shm_dir       SHM 文件目录
    /// @param event_queue   事件队列 (SPSC), 用于向主线程转发非行情回调
    /// @throws std::runtime_error event_queue 为空时抛出
    MdSpi(const std::string& name,
          const std::filesystem::path& shm_dir,
          const SpscQueuePtr& event_queue);

    MdSpi(const MdSpi&) = delete;
    MdSpi& operator=(const MdSpi&) = delete;
    MdSpi(MdSpi&&) = delete;
    MdSpi& operator=(MdSpi&&) = delete;
    virtual ~MdSpi() = default;

    /// 从 ChannelMeta 同步订阅者列表。主线程调用, 与 SPI 线程的写操作互斥。
    void refresh_subscribers();

    /// 预取指定数量的页。主线程调用, 与 SPI 线程的写操作互斥。
    void prefetch_pages(uint64_t count);

    /// 预取指定字节大小的页。主线程调用, 与 SPI 线程的写操作互斥。
    void prefetch_for_bytes(uint64_t bytes);

    /// 关闭旧页 (回收)。主线程调用, 与 SPI 线程的写操作互斥。
    void close_old_pages();

    /// 触页预热: 读取当前写入位置, 触发 page fault。主线程调用, 与 SPI 线程互斥。
    void touch_write_position();

    // NOLINTBEGIN
    void OnFrontConnected() override;

    void OnFrontDisconnected(int nReason) override;

    void OnHeartBeatWarning(int nTimeLapse) override;

    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo,
                        int nRequestID,
                        bool bIsLast) override;

    void OnRspUserLogout(CThostFtdcUserLogoutField* pUserLogout,
                         CThostFtdcRspInfoField* pRspInfo,
                         int nRequestID,
                         bool bIsLast) override;

    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                            CThostFtdcRspInfoField* pRspInfo,
                            int nRequestID,
                            bool bIsLast) override;

    void OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                              CThostFtdcRspInfoField* pRspInfo,
                              int nRequestID,
                              bool bIsLast) override;

    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) override;

    // NOLINTEND

private:
    /// 保护 md_writer_ 的自旋锁。SPI 线程 (写行情) 与主线程 (维护订阅者/页) 互斥。
    /// 非递归, 所有加锁点均为单层 lock_guard, 无嵌套, 无死锁风险。
    shm::SpinLock thread_lock_;

    /// 共享内存单写者。所有 open_frame/close_frame/refresh/prefetch 访问均需持有 thread_lock_。
    /// 内部 NullLock, 实际互斥完全依赖外部 thread_lock_。
    shm::SingleWriter md_writer_;

    /// 当前交易日距纪元天数。仅 SPI 线程读写 (OnRspUserLogin 写, OnRspUserLogout/OnRtnDepthMarketData 读)。
    /// 依赖 CTP MD API 单线程回调模型, 无需加锁。主线程禁止直接读取。
    /// 初始值为 INT32_MIN 哨兵, 登录成功后更新为有效值。
    DzDate days_since_epoch_ = std::numeric_limits<DzDate>::min();

    /// 事件队列 (SPSC, 构造后不可变)。SPI 线程单生产者 push, 主线程单消费者 pop/wait。
    SpscQueuePtr event_queue_;
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_MD_SPI_H_
