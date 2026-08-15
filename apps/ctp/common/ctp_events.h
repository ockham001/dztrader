#ifndef DZTRADER_CTP_CTP_EVENTS_H_
#define DZTRADER_CTP_CTP_EVENTS_H_

#include <cstdint>
#include <limits>
#include <string>
#include <chrono>
#include <format>

#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <ThostFtdcMdApi.h>

#include <dztrader/data_type.h>
#include <dztrader/shm/named_semaphore.h>

namespace dztrader::ctp {

/// CTP SPI 回调事件类型。
/// OnRtnDepthMarketData 不入队, 由 SPI 线程直接写 SHM (低延迟路径)。
enum class EventType : int16_t {
    Unknown = 0,
    OnFrontConnected = 1,       ///< 前置连接成功
    OnFrontDisconnected = 2,    ///< 前置连接断开
    OnHeartbeatWarning = 3,     ///< 心跳超时警告
    OnRspUserLogin = 4,         ///< 登录响应
    OnRspUserLogout = 5,        ///< 登出响应
    OnRspQueryMulticastInstrument = 6,  ///< 组播合约查询响应 (预留)
    OnRspError = 7,             ///< 错误响应
    OnRspSubMarketData = 8,     ///< 订阅行情响应
    OnRspUnsubMarketData = 9,   ///< 退订行情响应
    OnRspSubForQuoteSp = 10,    ///< 订阅询价响应 (预留)
    OnRspUnsubForQuoteSp = 11,  ///< 退订询价响应 (预留)
    OnRtnDepthMarketData = 12,  ///< 行情推送 (不入队, 直写 SHM)
    OnRtnForQuoteResp = 13,     ///< 询价响应 (预留)
    // td 扩展 (100+, 避开 md 的 0-13)
    // 注意: td 的 *Field 结构和 delete_data 处理在 apps/ctp/td/td_events.h
    // Event::delete_data() 对 td 类型走 default 分支 (记 WARN, 由 td_delete_event_data 处理)
    OnRspAuthenticate = 100,                 ///< td 认证响应
    OnRspTdUserLogin = 101,                  ///< td 登录响应 (Field 含 MaxOrderRef, 与 md 的 OnRspUserLogin=4 不同)
    OnRspSettlementInfoConfirm = 102,        ///< 结算单确认响应
    OnRspQryInstrument = 103,                ///< 合约查询响应 (多次回调)
    OnRspQryTradingAccount = 104,            ///< 资金查询响应
    OnRspQryInvestorPosition = 105,          ///< 持仓查询响应
    OnRspQryInvestorPositionDetail = 106,    ///< 持仓明细查询响应
    OnRspQryInstrumentMarginRate = 107,      ///< 保证金率查询响应
    OnRspQryInstrumentCommissionRate = 108,  ///< 手续费率查询响应
    OnRspQryOrder = 109,                     ///< 委托查询响应 (RESTART 补登用)
    OnRtnOrder = 110,                        ///< 委托回报 (实时推送)
    OnRtnTrade = 111,                        ///< 成交回报 (实时推送)
    OnRtnInstrumentStatus = 112,             ///< 合约交易状态回报
    OnRspOrderInsert = 113,                  ///< 报单录入响应
    OnRspOrderAction = 114,                  ///< 报单操作响应 (撤单)
    OnErrRtnOrderInsert = 115,               ///< 报单录入错误回报
    OnErrRtnOrderAction = 116,               ///< 报单操作错误回报
    OnRspFromBankToFutureByFuture = 117,     ///< 出入金响应 (OnRsp)
    OnRtnFromBankToFutureByFuture = 118,     ///< 出入金实时通知 (OnRtn, 权威)
    OnRspUserPasswordUpdate = 119,           ///< 修改登录密码响应
    OnRspTradingAccountPasswordUpdate = 120, ///< 修改资金密码响应
};

/// SPI 线程到主线程的事件载体。
/// data 指向对应 EventType 的 *Field 结构体, 由 push 端 new, pop 端 delete。
struct Event {
    void* data = nullptr;
    EventType type = EventType::Unknown;

    /// 按 EventType 释放 data (析构清理用, 不调 handler)。
    /// Unknown 类型无法确定构造类型, 不释放 (由调用方记日志)。
    void delete_data() noexcept;
};

/// 事件队列: 包装 lock-free 队列 + 信号量唤醒。
/// SPI 线程 push (单生产者), 主线程 pop (单消费者)。
/// 信号量由调用方传入 (非拥有), master 的 notify_subscribers 与 SPI 线程的 push
/// 共用同一个信号量, 主线程 wait 它即可同时响应事件通道帧和 SPI 事件。
template <typename QueueImpl>
class EventQueue {
public:
    /// 构造事件队列。
    /// @param sem 外部拥有的信号量, 用于唤醒主线程 (非拥有, 不负责释放)
    /// @param size 队列容量
    EventQueue(shm::NamedSemaphore* sem, uint64_t size)
        : sem_(sem),
          queue_(size) {}

    /// 入队事件并通知信号量。
    /// @param type 事件类型
    /// @param data 事件数据指针 (允许 nullptr), 所有权转移给队列
    /// @throws std::runtime_error 队列满时抛出 (data 已释放)。
    template <typename T>
    void push(EventType type, T* data) {
        // 可以接受data是nullptr
        if (!queue_.push({.data = data, .type = type})) {
            delete data;  // NOLINT
            throw std::runtime_error(
                std::format("event queue push failed, type={}", magic_enum::enum_name(type)));
        }
        static_assert(noexcept(sem_->notify()));
        sem_->notify();
    }

    /// 阻塞等待信号量唤醒
    void wait() noexcept {
        static_assert(noexcept(sem_->wait()));
        sem_->wait();
    }

    /// 等待信号量唤醒, 支持超时
    /// @return true 被唤醒, false 超时
    bool wait_for(uint32_t timeout_ms) noexcept {
        static_assert(noexcept(sem_->wait_for(timeout_ms)));
        return sem_->wait_for(timeout_ms);
    }

    /// 非阻塞出队
    /// @return true 成功, false 队列空
    bool pop(Event& event) { return queue_.pop(event); }

private:
    shm::NamedSemaphore* sem_;
    QueueImpl queue_;
};

using SpscQueue = EventQueue<boost::lockfree::spsc_queue<Event>>;

using SpscQueuePtr = std::shared_ptr<SpscQueue>;

/// MPMC 事件队列: 多生产者 (多账户 SPI 线程) + 单消费者 (主线程).
/// td 模块用此队列, 因多账户同进程, 每账户 1 个 SPI 线程共享队列
using MpmcQueue = EventQueue<boost::lockfree::queue<Event>>;

using MpmcQueuePtr = std::shared_ptr<MpmcQueue>;

/// OnFrontConnected 回调数据
struct OnFrontConnectedField {
    std::chrono::system_clock::time_point rsp_time;
};

/// OnFrontDisconnected 回调数据
struct OnFrontDisconnectedField {
    int reason;
};

/// OnHeartbeatWarning 回调数据
struct OnHeartBeatWarningField {
    int time_lapse;
};

/// OnRspUserLogin 回调数据
struct OnRspUserLoginField {
    std::optional<CThostFtdcRspUserLoginField> rsp_user_login;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    /// 当前交易日距纪元天数。登录成功且 TradingDay 解析成功时为有效值。
    /// 解析失败时保持 days_since_epoch_ 当前值 (首次登录为哨兵 INT32_MIN,
    /// 重连后为上次有效值)。下游应结合 trading_day_parse_error 字段判断是否可信。
    int32_t days_since_epoch = std::numeric_limits<int32_t>::min();
    /// 交易日解析错误信息 (仅 trading_day 格式非法时填, 否则为 nullopt)
    std::optional<std::string> trading_day_parse_error;
    /// 收到响应的时间戳
    std::chrono::system_clock::time_point rsp_time;
};

/// OnRspUserLogout 回调数据
struct OnRspUserLogoutField {
    std::optional<CThostFtdcUserLogoutField> user_logout;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    /// 交易日距离纪元的天数。复用 spi 线程 login 时写入的 days_since_epoch_ 缓存值
    /// (logout 本身不重新解析 trading_day, 因此该值与 login 时刻一致)
    int32_t days_since_epoch = std::numeric_limits<int32_t>::min();
};

/// OnRspError 回调数据
struct OnRspErrorField {
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
};

/// OnRspSubMarketData / OnRspUnsubMarketData 回调数据
struct OnRspSubMarketDataField {
    std::optional<CThostFtdcSpecificInstrumentField> specific_instrument;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
};

using OnRspUnSubMarketDataField = OnRspSubMarketDataField;

// --- Event::delete_data 实现 (需所有 *Field 定义可见) ---

inline void Event::delete_data() noexcept {
    switch (type) {
        case EventType::OnFrontConnected:
            delete static_cast<OnFrontConnectedField*>(data);  // NOLINT
            break;
        case EventType::OnFrontDisconnected:
            delete static_cast<OnFrontDisconnectedField*>(data);  // NOLINT
            break;
        case EventType::OnHeartbeatWarning:
            delete static_cast<OnHeartBeatWarningField*>(data);  // NOLINT
            break;
        case EventType::OnRspUserLogin:
            delete static_cast<OnRspUserLoginField*>(data);  // NOLINT
            break;
        case EventType::OnRspUserLogout:
            delete static_cast<OnRspUserLogoutField*>(data);  // NOLINT
            break;
        case EventType::OnRspError:
            delete static_cast<OnRspErrorField*>(data);  // NOLINT
            break;
        case EventType::OnRspSubMarketData:
            delete static_cast<OnRspSubMarketDataField*>(data);  // NOLINT
            break;
        case EventType::OnRspUnsubMarketData:
            delete static_cast<OnRspUnSubMarketDataField*>(data);  // NOLINT
            break;
        default:
            // 未入队类型 (OnRtnDepthMarketData 走 SHM 直写) 或 Unknown
            // 不应出现在事件队列中, 出现则记日志 (data 不释放, 调用方处理)
            if (data != nullptr) {
                SPDLOG_ERROR("event delete_data unhandled type | type={} data={}",
                             magic_enum::enum_name(type), data);
            }
            break;
    }
    data = nullptr;
}

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_CTP_EVENTS_H_