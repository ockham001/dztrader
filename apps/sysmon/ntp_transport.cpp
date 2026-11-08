#include "ntp_transport.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace dztrader::sysmon {
namespace {

/// 响应接收缓冲区：NTP 报文 48 字节，部分实现会带扩展字段；多收的字节由报文层忽略。
constexpr std::size_t kReceiveBufferSize = 1024;

}  // namespace

struct UdpNtpTransport::Exchange {
    explicit Exchange(boost::asio::io_context& io)
        : resolver(io),
          socket(io),
          timer(io) {}

    boost::asio::ip::udp::resolver resolver;
    boost::asio::ip::udp::socket socket;
    boost::asio::steady_timer timer;
    boost::asio::ip::udp::endpoint endpoint;
    std::array<uint8_t, ntp::kPacketSize> request{};
    std::array<uint8_t, kReceiveBufferSize> response{};
    boost::asio::ip::udp::endpoint sender;
    NtpTransport::Handler handler;
    bool done = false;
};

UdpNtpTransport::UdpNtpTransport(boost::asio::io_context& io, uint16_t port)
    : io_(io),
      port_(port) {}

UdpNtpTransport::~UdpNtpTransport() {
    for (const auto& exchange : pending_) {
        exchange->done = true;
        exchange->timer.cancel();
        exchange->resolver.cancel();
        boost::system::error_code close_ec;
        exchange->socket.close(close_ec);
        exchange->handler = nullptr;
    }
    pending_.clear();
}

void UdpNtpTransport::async_exchange(std::string host,
                                     std::array<uint8_t, ntp::kPacketSize> request,
                                     int32_t timeout_ms,
                                     Handler handler) {
    auto exchange = std::make_shared<Exchange>(io_);
    exchange->request = request;
    exchange->handler = std::move(handler);
    pending_.insert(exchange);

    // 超时覆盖「解析 + 发送 + 等待响应」全过程：DNS 卡住也按这一台失败处理，
    // 不让单台服务器把整轮探测拖成无限期。
    exchange->timer.expires_after(std::chrono::milliseconds(timeout_ms));
    exchange->timer.async_wait([this, exchange](const boost::system::error_code& ec) {
        // `done` 兜底：定时器已到期但 handler 尚未执行时被响应抢先结算（cancel 不会
        // 使已到期的 handler 失效），此时不得再走 finish。
        if (ec || exchange->done) {
            return;
        }
        NtpExchange result;
        result.status = NtpExchange::Status::Timeout;
        finish(exchange, std::move(result));
    });

    exchange->resolver.async_resolve(
        host, std::to_string(port_),
        [this, exchange](const boost::system::error_code& ec,
                         const boost::asio::ip::udp::resolver::results_type& results) {
            if (exchange->done) {
                return;
            }
            if (ec || results.empty()) {
                NtpExchange result;
                result.status = NtpExchange::Status::Unreachable;
                finish(exchange, std::move(result));
                return;
            }
            // 只取解析结果的第一条地址：NTP 服务器通常只有一个 A/AAAA 记录；
            // 多地址轮询不是本票语义（契约 §2.7 只要求每台各发一次）。
            exchange->endpoint = results.begin()->endpoint();
            boost::system::error_code open_ec;
            exchange->socket.open(exchange->endpoint.protocol(), open_ec);
            if (open_ec) {
                NtpExchange result;
                result.status = NtpExchange::Status::Unreachable;
                finish(exchange, std::move(result));
                return;
            }
            exchange->socket.async_send_to(
                boost::asio::buffer(exchange->request), exchange->endpoint,
                [this, exchange](const boost::system::error_code& send_ec, std::size_t) {
                    if (exchange->done) {
                        return;
                    }
                    if (send_ec) {
                        NtpExchange result;
                        result.status = NtpExchange::Status::Unreachable;
                        finish(exchange, std::move(result));
                        return;
                    }
                    exchange->socket.async_receive_from(
                        boost::asio::buffer(exchange->response), exchange->sender,
                        [this, exchange](const boost::system::error_code& recv_ec,
                                         std::size_t bytes) {
                            if (exchange->done) {
                                return;
                            }
                            if (recv_ec) {
                                // 超时取消会走到这里（done 已置位）；其余收发错误按不可达。
                                NtpExchange result;
                                result.status = NtpExchange::Status::Unreachable;
                                finish(exchange, std::move(result));
                                return;
                            }
                            NtpExchange result;
                            result.status = NtpExchange::Status::Received;
                            result.packet.assign(
                                exchange->response.begin(),
                                exchange->response.begin() + static_cast<std::ptrdiff_t>(bytes));
                            finish(exchange, std::move(result));
                        });
                });
        });
}

void UdpNtpTransport::finish(const std::shared_ptr<Exchange>& exchange, NtpExchange result) {
    if (exchange->done) {
        return;  // 恰好回调一次：超时与响应竞争时后到者直接丢弃
    }
    exchange->done = true;
    exchange->timer.cancel();
    exchange->resolver.cancel();
    boost::system::error_code ec;
    exchange->socket.close(ec);
    pending_.erase(exchange);

    Handler handler = std::move(exchange->handler);
    exchange->handler = nullptr;
    if (handler) {
        handler(std::move(result));
    }
}

}  // namespace dztrader::sysmon
