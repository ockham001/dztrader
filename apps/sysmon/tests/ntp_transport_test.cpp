/**
 * @file ntp_transport_test.cpp
 * @brief 真实 UDP 传输（#299）：Asio 异步收发的成功 / 超时 / 不可达 / 并发与截断包透传。
 *
 * 不依赖外网、不依赖特权端口：本文件在 **127.0.0.1 的临时端口** 上起一个本地 UDP 替身
 * 服务端，被测的仍是完整真实路径（resolver → socket → send_to → receive_from → 超时定时器）。
 * 目标端口经 `UdpNtpTransport` 构造参数注入（生产恒为 123；绑定 123 需要特权，CI 不能依赖）。
 */

#include "ntp_transport.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/system/error_code.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;
using asio::ip::udp;

/// 一次交换的默认超时（远大于本机回环往返，只用于让用例失败得快）。
constexpr int32_t kTestTimeoutMs = 1000;

std::array<uint8_t, ntp::kPacketSize> make_request(uint32_t seconds) {
    ntp::Timestamp transmit;
    transmit.seconds = seconds;
    return ntp::build_request(transmit);
}

class UdpNtpTransportTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<udp::socket>(io_, udp::endpoint(udp::v4(), 0));
        server_port_ = server_->local_endpoint().port();
    }

    void TearDown() override {
        boost::system::error_code ec;
        server_->close(ec);
    }

    /// 收一次请求，把字节与来源交给回调（回调里可决定是否回复）。
    void serve_once(
        std::function<void(const uint8_t*, std::size_t, const udp::endpoint&)> on_request) {
        server_->async_receive_from(asio::buffer(server_buffer_), sender_,
                                    [this, on_request = std::move(on_request)](
                                        const boost::system::error_code& ec, std::size_t bytes) {
                                        if (!ec) {
                                            on_request(server_buffer_.data(), bytes, sender_);
                                        }
                                    });
    }

    /// 连收 @p count 次请求。
    void serve_many(
        int count,
        std::function<void(const uint8_t*, std::size_t, const udp::endpoint&)> on_request) {
        if (count <= 0) {
            return;
        }
        serve_once([this, count, on_request = std::move(on_request)](
                       const uint8_t* data, std::size_t bytes, const udp::endpoint& from) {
            on_request(data, bytes, from);
            serve_many(count - 1, on_request);
        });
    }

    /// 回复任意字节（缓冲区必须活到发送完成——用成员缓冲区）。
    void reply(const std::vector<uint8_t>& bytes, const udp::endpoint& to) {
        reply_buffer_ = bytes;
        server_->async_send_to(asio::buffer(reply_buffer_), to,
                               [](const boost::system::error_code&, std::size_t) {});
    }

    asio::io_context io_;
    std::unique_ptr<udp::socket> server_;
    uint16_t server_port_ = 0;
    std::array<uint8_t, 1024> server_buffer_{};
    udp::endpoint sender_;
    std::vector<uint8_t> reply_buffer_;
};

TEST_F(UdpNtpTransportTest, ReceivesServerResponseBytes) {
    UdpNtpTransport transport(io_, server_port_);
    const auto request = make_request(3'900'000'000U);

    std::optional<NtpExchange> received;
    int calls = 0;
    transport.async_exchange("127.0.0.1", request, kTestTimeoutMs, [&](NtpExchange result) {
        ++calls;
        received = std::move(result);
    });

    serve_once([this, request](const uint8_t* data, std::size_t bytes, const udp::endpoint& from) {
        EXPECT_EQ(bytes, ntp::kPacketSize);
        EXPECT_TRUE(std::equal(data, data + bytes, request.begin()));
        std::vector<uint8_t> response(data, data + bytes);
        response[0] = 0x24;  // 服务端模式标记（传输层不解析，只透传字节）
        reply(response, from);
    });

    io_.restart();
    io_.run_for(3s);

    ASSERT_EQ(calls, 1);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->status, NtpExchange::Status::Received);
    ASSERT_EQ(received->packet.size(), ntp::kPacketSize);
    EXPECT_EQ(received->packet[0], 0x24);
}

TEST_F(UdpNtpTransportTest, PassesThroughTruncatedResponse) {
    // 截断包由报文层判 bad_echo（契约 §2.9）；传输层只负责如实交出收到的字节。
    UdpNtpTransport transport(io_, server_port_);
    std::optional<NtpExchange> received;
    transport.async_exchange("127.0.0.1", make_request(1U), kTestTimeoutMs,
                             [&](NtpExchange result) { received = std::move(result); });

    serve_once([this](const uint8_t* data, std::size_t, const udp::endpoint& from) {
        const std::vector<uint8_t> truncated(data, data + 10);
        reply(truncated, from);
    });

    io_.restart();
    io_.run_for(3s);

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->status, NtpExchange::Status::Received);
    EXPECT_EQ(received->packet.size(), 10U);
}

TEST_F(UdpNtpTransportTest, TimesOutWhenServerDoesNotReply) {
    UdpNtpTransport transport(io_, server_port_);
    constexpr int32_t kTimeoutMs = 100;

    int calls = 0;
    std::optional<NtpExchange> received;
    transport.async_exchange("127.0.0.1", make_request(2U), kTimeoutMs, [&](NtpExchange result) {
        ++calls;
        received = std::move(result);
    });

    // 收下请求但不回复：交换只能由超时定时器结算。
    serve_once([](const uint8_t*, std::size_t, const udp::endpoint&) {});

    const auto started = std::chrono::steady_clock::now();
    io_.restart();
    io_.run_for(2s);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_EQ(calls, 1);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->status, NtpExchange::Status::Timeout);
    EXPECT_GE(elapsed, 80ms) << "timeout fired too early (configured 100ms)";
    EXPECT_LT(elapsed, 1500ms) << "timeout did not bound the exchange";
}

TEST_F(UdpNtpTransportTest, UnresolvableHostIsUnreachable) {
    // 超过 DNS 名字长度上限（255）的主机名在本地即被判非法：getaddrinfo 立即返回
    // EAI_NONAME，不产生 DNS 查询、不依赖网络（空主机名反而会被解析成回环，不适用）。
    const std::string unresolvable(300, 'x');
    UdpNtpTransport transport(io_, server_port_);
    std::optional<NtpExchange> received;
    int calls = 0;
    transport.async_exchange(unresolvable, make_request(3U), kTestTimeoutMs,
                             [&](NtpExchange result) {
                                 ++calls;
                                 received = std::move(result);
                             });

    io_.restart();
    io_.run_for(3s);

    ASSERT_EQ(calls, 1);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->status, NtpExchange::Status::Unreachable);
}

TEST_F(UdpNtpTransportTest, ConcurrentExchangesStayIndependent) {
    UdpNtpTransport transport(io_, server_port_);
    std::array<std::optional<NtpExchange>, 2> received;
    std::array<int, 2> calls{};

    transport.async_exchange("127.0.0.1", make_request(10U), kTestTimeoutMs,
                             [&received, &calls](NtpExchange result) {
                                 ++calls[0];
                                 received[0] = std::move(result);
                             });
    transport.async_exchange("127.0.0.1", make_request(20U), kTestTimeoutMs,
                             [&received, &calls](NtpExchange result) {
                                 ++calls[1];
                                 received[1] = std::move(result);
                             });

    int served = 0;
    serve_many(2,
               [this, &served](const uint8_t* data, std::size_t bytes, const udp::endpoint& from) {
                   std::vector<uint8_t> response(data, data + bytes);
                   response[0] = static_cast<uint8_t>(0x30 + served);  // 两台各回不同标记
                   ++served;
                   reply(response, from);
               });

    io_.restart();
    io_.run_for(3s);

    ASSERT_EQ(served, 2);
    ASSERT_EQ(calls[0], 1);
    ASSERT_EQ(calls[1], 1);
    ASSERT_TRUE(received[0].has_value());
    ASSERT_TRUE(received[1].has_value());
    EXPECT_EQ(received[0]->status, NtpExchange::Status::Received);
    EXPECT_EQ(received[1]->status, NtpExchange::Status::Received);
    // 两台的结果不得串台：每个回调收到的正是自己那条回复。
    EXPECT_NE(received[0]->packet[0], received[1]->packet[0]);
}

}  // namespace
}  // namespace dztrader::sysmon
