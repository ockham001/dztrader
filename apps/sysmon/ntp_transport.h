#ifndef DZTRADER_SYSMON_NTP_TRANSPORT_H_
#define DZTRADER_SYSMON_NTP_TRANSPORT_H_

/**
 * @file ntp_transport.h
 * @brief NTPv4 客户端模式的真实 UDP 传输实现（契约 sysmon §2.7、§2.9）。
 *
 * 实现 `NtpTransport` 接缝：Asio 异步 UDP（`resolver` + `send_to` + `receive_from` +
 * `steady_timer`），**不阻塞调用线程**（io_context 单线程，阻塞会同时拖住采集与帧处理），
 * 且**恰好回调 `handler` 一次**：
 * - 收到任意响应字节 → `Received`（是否可用由报文层判定，截断包也按 `Received` 交出，
 *   由 `parse_response` 归入 `bad_echo`）
 * - 解析/收发失败 → `Unreachable`
 * - `timeout_ms` 内没有响应 → `Timeout`（超时覆盖 DNS 解析 + 收发全过程：解析卡住同样
 *   算这一台失败，不把整轮探测拖到无限期）
 *
 * 线程模型：非线程安全，只在创建它的 io_context 线程上调用（与 `ClockProber` 同一条
 * 单线程约束）。析构时若仍有 pending 交换，调用方应已停止 io_context（进程退出路径
 * `ioc_.stop()` 之后才析构）——停止后 asio 不再投递 handler，pending 状态安全释放。
 */

#include "clock_probe.h"

#include <boost/asio/io_context.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

namespace dztrader::sysmon {

/// NTP 服务端口（RFC 5905：UDP/123）。
inline constexpr uint16_t kNtpPort = 123;

/**
 * @brief 真实 UDP 传输：一台服务器一次交换 = 一个独立 socket + 超时定时器。
 *
 * 每台服务器各开一个 socket（不共享、不串行），多台服务器因此真正并发探测：
 * 一台解析慢/超时不影响其它台（契约 §2.7「单台失败不影响其它台」）。
 */
class UdpNtpTransport final : public NtpTransport {
public:
    /**
     * @param io   事件循环（全部异步操作绑定它）
     * @param port 目标端口；生产恒为 `kNtpPort`（123）。单元测试传本地临时端口——
     *             绑定 123 需要特权，CI 不能依赖；被测的仍是完整真实 UDP 收发路径。
     */
    explicit UdpNtpTransport(boost::asio::io_context& io, uint16_t port = kNtpPort);
    ~UdpNtpTransport() override;

    UdpNtpTransport(const UdpNtpTransport&) = delete;
    UdpNtpTransport& operator=(const UdpNtpTransport&) = delete;
    UdpNtpTransport(UdpNtpTransport&&) = delete;
    UdpNtpTransport& operator=(UdpNtpTransport&&) = delete;

    void async_exchange(std::string host,
                        std::array<uint8_t, ntp::kPacketSize> request,
                        int32_t timeout_ms,
                        Handler handler) override;

private:
    /// 一次交换的进行中状态（socket / resolver / 超时定时器 / 回调）。
    struct Exchange;

    /// 结算一次交换：保证只回调一次，并释放全部 pending 资源。
    void finish(const std::shared_ptr<Exchange>& exchange, NtpExchange result);

    boost::asio::io_context& io_;
    uint16_t port_;
    /// 进行中的交换（析构时统一取消；`shared_ptr` 让异步 handler 持有上下文生命周期）。
    std::set<std::shared_ptr<Exchange>> pending_;
};

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_NTP_TRANSPORT_H_
