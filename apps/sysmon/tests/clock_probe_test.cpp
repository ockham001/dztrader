// 契约 sysmon §2.7 / §2.9 / §1.1：一轮探测的编排（ClockProber）与手动限流判定。
// 全程用假 transport（不访问真实网络）与脚本化时刻读数（不依赖真实时间）：断言多服务器
// 聚合选择、单台失败不影响其它台、全部失败不可用而非 0、KoD 退避/拉黑跨轮保持、限流判定。

#include "clock_probe.h"

#include "ntp_codec.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::sysmon {
namespace {

/// 2026-09-21T14:13:20Z：用例里的「当前墙钟」基准（Unix 秒 1790000000，NTP 秒 3998988800）。
constexpr int64_t kWallBaseNs = 1'790'000'000LL * 1'000'000'000LL;

/// 脚本化时刻读数：墙钟与单调时钟同步推进，模拟交互耗时（用例完全不依赖真实时间）。
class FakeMoment {
public:
    Moment operator()() { return Moment{wall_, steady_}; }

    void advance(int64_t delta_ns) {
        wall_ += delta_ns;
        steady_ += delta_ns;
    }

    [[nodiscard]] int64_t wall() const { return wall_; }

private:
    int64_t wall_ = kWallBaseNs;
    int64_t steady_ = 1'000'000'000LL;
};

/// 一台服务器的脚本化应答。
struct ScriptedReply {
    enum class Kind {
        Answer,       ///< 正常响应（按网络时延与本地偏差合成四个时间戳）
        Timeout,      ///< 超时不响应
        Unreachable,  ///< 解析/收发失败
        Raw,          ///< 直接给出原始字节（KoD / 畸形包 / 假 origin 等）
    };
    Kind kind = Kind::Timeout;
    int64_t one_way_ns = 0;            ///< 单程网络时延（往返 = 2 × 该值 + 服务端处理时间）
    int64_t local_ahead_ns = 0;        ///< 本地比服务端快的量（正 = 本地快）
    int64_t server_processing_ns = 0;  ///< 服务端处理时间（T3 − T2）
    int64_t root_dispersion_ns = 0;    ///< 服务端 root dispersion（进 bound 计算）
    uint8_t stratum = 2;
    std::vector<uint8_t> raw;  ///< `Kind::Raw` 时交付的字节
};

/// 把纳秒换成协议里的 16.16 定点秒原始值（root delay / root dispersion 的线上表示）。
uint32_t fixed_raw_from_ns(int64_t nanos) {
    // raw = round(nanos × 65536 / 1e9)；这里只用于构造测试报文，精度损失是协议固有的。
    return static_cast<uint32_t>((nanos * 65'536LL + 500'000'000LL) / 1'000'000'000LL);
}

/// 48 字节服务端报文的最小构造helper（逐字段按 RFC 5905 §7.3 布局写，不经过被测代码）。
std::vector<uint8_t> make_packet_bytes(uint8_t stratum,
                                       std::array<char, 4> reference_id,
                                       int64_t root_dispersion_ns,
                                       ntp::Timestamp origin,
                                       ntp::Timestamp receive,
                                       ntp::Timestamp transmit) {
    std::vector<uint8_t> packet(48, 0);
    packet[0] = static_cast<uint8_t>((4U << 3) | ntp::kModeServer);
    packet[1] = stratum;
    packet[3] = static_cast<uint8_t>(0xF6U);  // precision = -10
    const auto put32 = [&packet](std::size_t offset, uint32_t value) {
        packet[offset] = static_cast<uint8_t>((value >> 24) & 0xFFU);
        packet[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFFU);
        packet[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFFU);
        packet[offset + 3] = static_cast<uint8_t>(value & 0xFFU);
    };
    put32(8, fixed_raw_from_ns(root_dispersion_ns));
    put32(12, (static_cast<uint32_t>(static_cast<uint8_t>(reference_id[0])) << 24) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(reference_id[1])) << 16) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(reference_id[2])) << 8) |
                  static_cast<uint32_t>(static_cast<uint8_t>(reference_id[3])));
    const auto put_ts = [&put32](std::size_t offset, ntp::Timestamp stamp) {
        put32(offset, stamp.seconds);
        put32(offset + 4, stamp.fraction);
    };
    put_ts(24, origin);
    put_ts(32, receive);
    put_ts(40, transmit);
    return packet;
}

/// 时间戳 + 纳秒偏移（偏移可正可负、可跨秒；按 32 位 era 回绕写回）。
ntp::Timestamp offset_stamp(ntp::Timestamp base, int64_t offset_ns) {
    int64_t seconds_shift = offset_ns / 1'000'000'000LL;
    int64_t remainder = offset_ns % 1'000'000'000LL;
    if (remainder < 0) {
        remainder += 1'000'000'000LL;
        --seconds_shift;
    }
    const int64_t fraction_ns = ntp::fraction_to_nanoseconds(base.fraction) + remainder;
    ntp::Timestamp stamp;
    stamp.seconds = static_cast<uint32_t>(
        (static_cast<int64_t>(base.seconds) + seconds_shift + fraction_ns / 1'000'000'000LL) &
        0xFFFF'FFFFLL);
    stamp.fraction = ntp::nanoseconds_to_fraction(fraction_ns % 1'000'000'000LL);
    return stamp;
}

/// 假传输：按 host 查出脚本化应答并**同步**回调（真实实现是异步的，编排层不得依赖同步性）。
class FakeTransport final : public NtpTransport {
public:
    void async_exchange(std::string host,
                        std::array<uint8_t, ntp::kPacketSize> request,
                        int32_t timeout_ms,
                        Handler handler) override {
        sent_hosts.push_back(host);
        last_timeout_ms = timeout_ms;
        const auto it = replies.find(host);
        NtpExchange result;
        if (it == replies.end()) {
            result.status = NtpExchange::Status::Timeout;
        } else {
            switch (it->second.kind) {
                case ScriptedReply::Kind::Timeout:
                    result.status = NtpExchange::Status::Timeout;
                    break;
                case ScriptedReply::Kind::Unreachable:
                    result.status = NtpExchange::Status::Unreachable;
                    break;
                case ScriptedReply::Kind::Raw:
                    result.status = NtpExchange::Status::Received;
                    result.packet = it->second.raw;
                    break;
                case ScriptedReply::Kind::Answer: {
                    result.status = NtpExchange::Status::Received;
                    const ScriptedReply& reply = it->second;
                    const auto t1 = origin_from_request(request);
                    // 本地快 X ⇒ T2 = T1 − X + 单程；T3 = T2 + 处理时间。
                    result.packet = make_packet_bytes(
                        reply.stratum, {'G', 'P', 'S', '\0'}, reply.root_dispersion_ns, t1,
                        offset_stamp(t1, reply.one_way_ns - reply.local_ahead_ns),
                        offset_stamp(t1, reply.one_way_ns - reply.local_ahead_ns +
                                             reply.server_processing_ns));
                    // 交互确实花掉了往返时间：墙钟与单调时钟同步推进（T4 由此得到）。
                    if (moment != nullptr) {
                        moment->advance(2 * reply.one_way_ns + reply.server_processing_ns);
                    }
                    break;
                }
            }
        }
        handler(std::move(result));
    }

    /// 请求里的 transmit timestamp 就是我方 T1。
    static ntp::Timestamp origin_from_request(
        const std::array<uint8_t, ntp::kPacketSize>& request) {
        const auto read32 = [&request](std::size_t offset) {
            return (static_cast<uint32_t>(request[offset]) << 24) |
                   (static_cast<uint32_t>(request[offset + 1]) << 16) |
                   (static_cast<uint32_t>(request[offset + 2]) << 8) |
                   static_cast<uint32_t>(request[offset + 3]);
        };
        ntp::Timestamp t1;
        t1.seconds = read32(40);
        t1.fraction = read32(44);
        return t1;
    }

    std::map<std::string, ScriptedReply> replies;
    FakeMoment* moment = nullptr;         ///< 非空时按应答耗时推进读数（模拟真实往返）
    std::vector<std::string> sent_hosts;  ///< 实际发出请求的 host（断言去重与跳过）
    int32_t last_timeout_ms = 0;
};

/// 跑一轮探测并把结果取出（假传输同步回调，所以结果在 probe() 返回后必定就绪）。
///
/// 返回 `std::optional` 而不是 `value_or({})`：失败时就地终止用例，否则「本轮没交付」会被后续
/// 断言级联成「servers 为空」的误导性失败（unit-test-standard §1.1）。
std::optional<ProbeOutcome> run_round(ClockProber& prober,
                                      const std::vector<std::string>& hosts,
                                      int32_t timeout_ms = 1500) {
    std::optional<ProbeOutcome> outcome;
    const bool started = prober.probe(
        hosts, timeout_ms, [&outcome](ProbeOutcome result) { outcome = std::move(result); });
    EXPECT_TRUE(started) << "no round should be in flight before this call";
    return outcome;
}

/// 构造一台正常响应的脚本（只给时延、本地偏差与 root dispersion）。
ScriptedReply answer(int64_t one_way_ns,
                     int64_t local_ahead_ns,
                     int64_t root_dispersion_ns = 0,
                     uint8_t stratum = 2) {
    ScriptedReply reply;
    reply.kind = ScriptedReply::Kind::Answer;
    reply.one_way_ns = one_way_ns;
    reply.local_ahead_ns = local_ahead_ns;
    reply.root_dispersion_ns = root_dispersion_ns;
    reply.stratum = stratum;
    return reply;
}

/// 构造 KoD 原始应答（stratum = 0，reference id 即四个 ASCII 字符）。
ScriptedReply kod_reply(std::array<char, 4> code) {
    ScriptedReply reply;
    reply.kind = ScriptedReply::Kind::Raw;
    reply.raw = make_packet_bytes(/*stratum=*/0, code, 0, ntp::Timestamp{}, ntp::Timestamp{},
                                  ntp::Timestamp{});
    return reply;
}

}  // namespace

// AC：全部成功——每台都被探测，聚合值取最小 RTT 的那台，明细保留全部条目。
TEST(ClockProbeTest, AllServersSucceedAggregatesFastestRtt) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    transport.replies = {{"slow.example", answer(100'000'000, 7'000'000, 20'000'000, 2)},
                         {"fast.example", answer(10'000'000, 5'000'000, 30'000'000, 1)},
                         {"medium.example", answer(30'000'000, 5'000'000, 10'000'000, 3)}};
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto outcome_opt = run_round(prober, {"slow.example", "fast.example", "medium.example"});
    ASSERT_TRUE(outcome_opt.has_value()) << "round must be delivered";
    const auto outcome = std::move(*outcome_opt);
    ASSERT_EQ(outcome.servers.size(), 3U);
    for (const auto& server : outcome.servers) {
        EXPECT_TRUE(server.ok) << server.host;
        EXPECT_FALSE(server.error.has_value());
    }
    // 最小 RTT 是 fast（往返 20 ms）：聚合值取它的偏差与 stratum。
    ASSERT_TRUE(outcome.offset_ms.has_value());
    EXPECT_DOUBLE_EQ(*outcome.offset_ms, 5.0);
    EXPECT_EQ(outcome.source, "fast.example");
    ASSERT_TRUE(outcome.source_stratum.has_value());
    EXPECT_EQ(*outcome.source_stratum, 1U);
    // bound = ½·rtt + root dispersion = 10 + 30 = 40 ms（root dispersion 有 16.16 定点量化误差）
    ASSERT_TRUE(outcome.bound_ms.has_value());
    EXPECT_NEAR(*outcome.bound_ms, 40.0, 0.01);
    // 每台明细都带自己的偏差与往返（保留全部明细，不只保留获胜者）
    ASSERT_TRUE(outcome.servers[0].rtt_ms.has_value());
    EXPECT_DOUBLE_EQ(*outcome.servers[0].rtt_ms, 200.0);
    EXPECT_DOUBLE_EQ(*outcome.servers[0].offset_ms, 7.0);
    ASSERT_TRUE(outcome.servers[2].rtt_ms.has_value());
    EXPECT_DOUBLE_EQ(*outcome.servers[2].rtt_ms, 60.0);
    EXPECT_EQ(transport.sent_hosts.size(), 3U);
    EXPECT_EQ(transport.last_timeout_ms, 1500) << "probe_timeout_ms must reach the transport";
    EXPECT_EQ(outcome.finished_unix_ms, moment.wall() / 1'000'000)
        << "last_probe_unix_ms comes from the round completion moment";
}

// AC：部分超时——失败台带 timeout 原因，聚合值仍来自成功台（单台失败不影响其它台）。
TEST(ClockProbeTest, PartialTimeoutKeepsAggregateFromSuccessfulServer) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    transport.replies = {{"ok.example", answer(15'000'000, -3'000'000, 5'000'000)}};
    // "dead.example" 未登记 => 超时
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto outcome_opt = run_round(prober, {"dead.example", "ok.example"});
    ASSERT_TRUE(outcome_opt.has_value()) << "round must be delivered";
    const auto outcome = std::move(*outcome_opt);
    ASSERT_EQ(outcome.servers.size(), 2U);
    EXPECT_FALSE(outcome.servers[0].ok);
    ASSERT_TRUE(outcome.servers[0].error.has_value());
    EXPECT_EQ(*outcome.servers[0].error, "timeout");
    EXPECT_FALSE(outcome.servers[0].offset_ms.has_value());
    EXPECT_FALSE(outcome.servers[0].rtt_ms.has_value());
    EXPECT_FALSE(outcome.servers[0].stratum.has_value());
    EXPECT_TRUE(outcome.servers[1].ok);
    EXPECT_EQ(outcome.source, "ok.example");
    ASSERT_TRUE(outcome.offset_ms.has_value());
    EXPECT_DOUBLE_EQ(*outcome.offset_ms, -3.0);
    EXPECT_EQ(transport.sent_hosts.size(), 2U) << "both servers are contacted each round";
}

// AC：全部失败——聚合值为不可用（nullopt）而不是 0，明细逐台带原因。
TEST(ClockProbeTest, AllFailuresYieldUnavailableAggregateNotZero) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    ScriptedReply broken;
    broken.kind = ScriptedReply::Kind::Raw;
    broken.raw = std::vector<uint8_t>(20, 0);  // 长度不足 48 => 畸形包
    ScriptedReply unreachable;
    unreachable.kind = ScriptedReply::Kind::Unreachable;
    transport.replies = {{"a.example", ScriptedReply{}},  // 默认 => 超时
                         {"b.example", unreachable},
                         {"c.example", broken}};
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto outcome_opt = run_round(prober, {"a.example", "b.example", "c.example"});
    ASSERT_TRUE(outcome_opt.has_value()) << "round must be delivered";
    const auto outcome = std::move(*outcome_opt);
    EXPECT_FALSE(outcome.offset_ms.has_value()) << "must not fake 0 when every server failed";
    EXPECT_FALSE(outcome.bound_ms.has_value());
    EXPECT_FALSE(outcome.source_stratum.has_value());
    EXPECT_TRUE(outcome.source.empty());
    ASSERT_EQ(outcome.servers.size(), 3U);
    ASSERT_TRUE(outcome.servers[0].error.has_value());
    EXPECT_EQ(*outcome.servers[0].error, "timeout");
    ASSERT_TRUE(outcome.servers[1].error.has_value());
    EXPECT_EQ(*outcome.servers[1].error, "unreachable");
    ASSERT_TRUE(outcome.servers[2].error.has_value());
    EXPECT_EQ(*outcome.servers[2].error, "bad_echo");
    for (const auto& server : outcome.servers) {
        EXPECT_FALSE(server.ok);
        EXPECT_FALSE(server.offset_ms.has_value());
        EXPECT_FALSE(server.bound_ms.has_value());
        EXPECT_FALSE(server.rtt_ms.has_value());
        EXPECT_FALSE(server.stratum.has_value());
    }
}

// AC2 字面形态：三台**全部超时**（不是混合失败）——聚合值不可用、明细逐台 timeout。
TEST(ClockProbeTest, AllTimeoutYieldsUnavailableAggregate) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    // 三台都未登记 => 全部超时
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto outcome_opt = run_round(prober, {"a.example", "b.example", "c.example"});
    ASSERT_TRUE(outcome_opt.has_value()) << "round must be delivered";
    const auto outcome = std::move(*outcome_opt);
    EXPECT_FALSE(outcome.offset_ms.has_value()) << "no server answered => must be unavailable";
    EXPECT_FALSE(outcome.bound_ms.has_value());
    EXPECT_FALSE(outcome.source_stratum.has_value());
    EXPECT_TRUE(outcome.source.empty());
    ASSERT_EQ(outcome.servers.size(), 3U);
    for (const auto& server : outcome.servers) {
        EXPECT_FALSE(server.ok) << server.host;
        ASSERT_TRUE(server.error.has_value()) << server.host;
        EXPECT_EQ(*server.error, "timeout") << server.host;
    }
    EXPECT_EQ(transport.sent_hosts.size(), 3U) << "every configured server is still contacted";
}

// origin 回显不匹配 => 该台 bad_echo（防伪造与迟到包），不参与聚合。
TEST(ClockProbeTest, OriginEchoMismatchIsBadEcho) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    // origin 回显一个非零但不是我方 T1 的时间戳；其余字段（含时间戳）都合法——
    // 若缺少回显校验，这一台会算出「完美 0 偏差」并可能成为聚合值。
    ntp::Timestamp forged_origin;
    forged_origin.seconds = 0x1122'3344U;
    ScriptedReply forged;
    forged.kind = ScriptedReply::Kind::Raw;
    forged.raw = make_packet_bytes(/*stratum=*/2, {'G', 'P', 'S', '\0'}, 0, forged_origin,
                                   forged_origin, forged_origin);
    transport.replies = {{"forged.example", forged}};
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto outcome_opt = run_round(prober, {"forged.example"});
    ASSERT_TRUE(outcome_opt.has_value()) << "round must be delivered";
    const auto outcome = std::move(*outcome_opt);
    ASSERT_EQ(outcome.servers.size(), 1U);
    ASSERT_TRUE(outcome.servers[0].error.has_value());
    EXPECT_EQ(*outcome.servers[0].error, "bad_echo");
    EXPECT_FALSE(outcome.offset_ms.has_value());
}

// AC：stratum >= 16 的参考源不采用（其时间戳不可信），按 bad_echo 记失败。
TEST(ClockProbeTest, UnsynchronisedSourceIsRejected) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    transport.replies = {{"unsync.example", answer(10'000'000, 0, 0, /*stratum=*/16)}};
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto outcome_opt = run_round(prober, {"unsync.example"});
    ASSERT_TRUE(outcome_opt.has_value()) << "round must be delivered";
    const auto outcome = std::move(*outcome_opt);
    ASSERT_EQ(outcome.servers.size(), 1U);
    ASSERT_TRUE(outcome.servers[0].error.has_value());
    EXPECT_EQ(*outcome.servers[0].error, "bad_echo");
    EXPECT_FALSE(outcome.offset_ms.has_value());
}

// AC：KoD RATE 对该服务器退避——当轮记 kod_rate，下一轮不再发包，退避到期后恢复探测。
TEST(ClockProbeTest, KissRateBacksOffThenRecovers) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    transport.replies = {{"rate.example", kod_reply({'R', 'A', 'T', 'E'})}};
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto first_opt = run_round(prober, {"rate.example"});
    ASSERT_TRUE(first_opt.has_value()) << "round must be delivered";
    const auto first = std::move(*first_opt);
    ASSERT_EQ(first.servers.size(), 1U);
    ASSERT_TRUE(first.servers[0].error.has_value());
    EXPECT_EQ(*first.servers[0].error, "kod_rate");
    EXPECT_EQ(transport.sent_hosts.size(), 1U);
    EXPECT_FALSE(first.offset_ms.has_value());

    // 第二轮仍在退避窗口内：不发包，明细仍带 kod_rate（明细恒与配置的服务器列表对齐）。
    auto second_opt = run_round(prober, {"rate.example"});
    ASSERT_TRUE(second_opt.has_value()) << "round must be delivered";
    const auto second = std::move(*second_opt);
    EXPECT_EQ(transport.sent_hosts.size(), 1U) << "RATE must stop contacting that server";
    ASSERT_EQ(second.servers.size(), 1U);
    ASSERT_TRUE(second.servers[0].error.has_value());
    EXPECT_EQ(*second.servers[0].error, "kod_rate");

    // 退避到期：重新发包（此处改成超时，用发包次数断言「确实又发了」）。
    moment.advance(kKissRateBackoffNs + 1'000'000'000LL);
    transport.replies.clear();
    auto third_opt = run_round(prober, {"rate.example"});
    ASSERT_TRUE(third_opt.has_value()) << "round must be delivered";
    const auto third = std::move(*third_opt);
    EXPECT_EQ(transport.sent_hosts.size(), 2U) << "backoff expires => probe again";
    ASSERT_EQ(third.servers.size(), 1U);
    ASSERT_TRUE(third.servers[0].error.has_value());
    EXPECT_EQ(*third.servers[0].error, "timeout");
}

// AC：KoD DENY 拉黑该服务器（跨轮保持），RSTR 同处置。
TEST(ClockProbeTest, KissDenyBlacklistsAcrossRounds) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    transport.replies = {{"deny.example", kod_reply({'D', 'E', 'N', 'Y'})},
                         {"rstr.example", kod_reply({'R', 'S', 'T', 'R'})}};
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto first_opt = run_round(prober, {"deny.example", "rstr.example"});
    ASSERT_TRUE(first_opt.has_value()) << "round must be delivered";
    const auto first = std::move(*first_opt);
    ASSERT_EQ(first.servers.size(), 2U);
    ASSERT_TRUE(first.servers[0].error.has_value());
    EXPECT_EQ(*first.servers[0].error, "kod_deny");
    ASSERT_TRUE(first.servers[1].error.has_value());
    EXPECT_EQ(*first.servers[1].error, "kod_deny") << "RSTR is treated like DENY";
    EXPECT_EQ(transport.sent_hosts.size(), 2U);

    // 黑名单跨轮保持：即便时间过去很久（远超 RATE 退避）也不再发包。
    moment.advance(1000LL * 1'000'000'000LL);
    auto second_opt = run_round(prober, {"deny.example", "rstr.example"});
    ASSERT_TRUE(second_opt.has_value()) << "round must be delivered";
    const auto second = std::move(*second_opt);
    EXPECT_EQ(transport.sent_hosts.size(), 2U) << "blacklist persists until process restart";
    ASSERT_EQ(second.servers.size(), 2U);
    ASSERT_TRUE(second.servers[0].error.has_value());
    EXPECT_EQ(*second.servers[0].error, "kod_deny");
    ASSERT_TRUE(second.servers[1].error.has_value());
    EXPECT_EQ(*second.servers[1].error, "kod_deny");
    EXPECT_FALSE(second.offset_ms.has_value());
}

// 其余 KoD（如 INIT）忽略处置：不拉黑、不退避，下一轮照常探测；当轮无可用时间戳 => bad_echo。
TEST(ClockProbeTest, OtherKissCodeIsIgnoredAndProbesAgain) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    transport.replies = {{"init.example", kod_reply({'I', 'N', 'I', 'T'})}};
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto first_opt = run_round(prober, {"init.example"});
    ASSERT_TRUE(first_opt.has_value()) << "round must be delivered";
    const auto first = std::move(*first_opt);
    ASSERT_EQ(first.servers.size(), 1U);
    ASSERT_TRUE(first.servers[0].error.has_value());
    EXPECT_EQ(*first.servers[0].error, "bad_echo") << "no usable timestamp from a KoD packet";
    EXPECT_EQ(transport.sent_hosts.size(), 1U);

    ASSERT_TRUE(run_round(prober, {"init.example"}).has_value());
    EXPECT_EQ(transport.sent_hosts.size(), 2U) << "non-RATE/DENY KoD must not block the server";
}

// 时间戳自相矛盾（服务端处理时间为负）的可用响应必须被拒为 bad_echo，而不是产出畸形样本。
TEST(ClockProbeTest, SelfContradictoryTimestampsBecomeBadEcho) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    // 服务端处理时间为负（T3 < T2）：derive_sample 拒收 => 该台 bad_echo。
    ScriptedReply contradictory = answer(10'000'000, 0);
    contradictory.server_processing_ns = -5'000'000;
    transport.replies = {{"contradictory.example", contradictory}};
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto outcome_opt = run_round(prober, {"contradictory.example"});
    ASSERT_TRUE(outcome_opt.has_value()) << "round must be delivered";
    const auto outcome = std::move(*outcome_opt);
    ASSERT_EQ(outcome.servers.size(), 1U);
    ASSERT_TRUE(outcome.servers[0].error.has_value());
    EXPECT_EQ(*outcome.servers[0].error, "bad_echo");
    EXPECT_FALSE(outcome.offset_ms.has_value());
}

// 单调往返小于服务端处理时间（rtt < 0）同样拒收。用「时钟不推进」的 moment 构造：
// 服务端声称处理了 5 ms，而本地单调时钟走动为 0 => rtt = 0 − 5 ms < 0。
TEST(ClockProbeTest, NegativeRttBecomesBadEcho) {
    class FrozenMomentTransport final : public NtpTransport {
    public:
        void async_exchange(std::string /*host*/,
                            std::array<uint8_t, ntp::kPacketSize> request,
                            int32_t,
                            Handler handler) override {
            const auto t1 = FakeTransport::origin_from_request(request);
            NtpExchange result;
            result.status = NtpExchange::Status::Received;
            // T2 = T1、T3 = T2 + 5 ms（服务端处理 5 ms），但本地读数不推进 => rtt < 0。
            result.packet =
                make_packet_bytes(2, {'G', 'P', 'S', '\0'}, 0, t1, t1, offset_stamp(t1, 5'000'000));
            handler(std::move(result));
        }
    } transport;
    FakeMoment moment;
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto outcome_opt = run_round(prober, {"frozen.example"});
    ASSERT_TRUE(outcome_opt.has_value()) << "round must be delivered";
    const auto outcome = std::move(*outcome_opt);
    ASSERT_EQ(outcome.servers.size(), 1U);
    ASSERT_TRUE(outcome.servers[0].error.has_value());
    EXPECT_EQ(*outcome.servers[0].error, "bad_echo") << "negative rtt must not yield a sample";
    EXPECT_FALSE(outcome.offset_ms.has_value());
}

// 空服务器列表（配置校验会拦，但 probe() 自身必须安全）：同步交付空明细 + 聚合不可用，
// 不得挂起等待一个永远不会来的回调。
TEST(ClockProbeTest, EmptyHostListCompletesSynchronously) {
    FakeTransport transport;
    FakeMoment moment;
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto outcome_opt = run_round(prober, {});
    ASSERT_TRUE(outcome_opt.has_value()) << "round must be delivered";
    const auto outcome = std::move(*outcome_opt);
    EXPECT_TRUE(outcome.servers.empty());
    EXPECT_FALSE(outcome.offset_ms.has_value());
    EXPECT_TRUE(outcome.source.empty());
    EXPECT_TRUE(transport.sent_hosts.empty()) << "nothing to send";
    EXPECT_FALSE(prober.probe_in_flight()) << "an empty round must not leave the prober busy";
}

// 上一轮违约产生的迟到回调不得被当作新一轮的结果（slot 复用 + 轮次令牌）。
TEST(ClockProbeTest, StaleCallbackFromPreviousRoundIsDiscarded) {
    class CaptureTransport final : public NtpTransport {
    public:
        void async_exchange(std::string,
                            std::array<uint8_t, ntp::kPacketSize>,
                            int32_t,
                            Handler handler) override {
            handlers.push_back(std::move(handler));
        }
        std::vector<Handler> handlers;
    } transport;

    FakeMoment moment;
    ClockProber prober(transport, [&moment]() { return moment(); });

    // 第一轮：结算，但保存 handler 以便事后「违约再调一次」。
    int first_completions = 0;
    ASSERT_TRUE(prober.probe({"a.example"}, 1500,
                             [&first_completions](ProbeOutcome) { ++first_completions; }));
    ASSERT_EQ(transport.handlers.size(), 1U);
    NtpExchange timeout;
    timeout.status = NtpExchange::Status::Timeout;
    transport.handlers[0](timeout);
    EXPECT_EQ(first_completions, 1);

    // 第二轮：同一 host（同 slot 复用）重新在飞，此时旧 handler 再次被调用。
    int second_completions = 0;
    std::optional<ProbeOutcome> second_outcome;
    ASSERT_TRUE(prober.probe({"a.example"}, 1500, [&](ProbeOutcome result) {
        second_outcome = std::move(result);
        ++second_completions;
    }));
    EXPECT_TRUE(prober.probe_in_flight())
        << "second round must still be waiting for its own callback";
    transport.handlers[0](timeout);  // 旧轮的迟到回调（轮次令牌不符）
    EXPECT_EQ(second_completions, 0) << "stale callback must not settle the new round";
    EXPECT_TRUE(prober.probe_in_flight());

    // 新一轮自己的回调到达后才交付。
    ASSERT_EQ(transport.handlers.size(), 2U);
    transport.handlers[1](timeout);
    EXPECT_EQ(second_completions, 1);
    ASSERT_TRUE(second_outcome.has_value());
    EXPECT_EQ(second_outcome->servers.size(), 1U);
}

// AC：min-RTT 选择在「同 RTT」时按 bound 更小者决胜（契约 §2.7）。
TEST(ClockProbeTest, TieOnRttBreaksBySmallerBound) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    transport.replies = {{"loose.example", answer(20'000'000, 4'000'000, 40'000'000)},
                         {"tight.example", answer(20'000'000, 6'000'000, 10'000'000)}};
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto outcome_opt = run_round(prober, {"loose.example", "tight.example"});
    ASSERT_TRUE(outcome_opt.has_value()) << "round must be delivered";
    const auto outcome = std::move(*outcome_opt);
    EXPECT_EQ(outcome.source, "tight.example") << "equal rtt => smaller bound wins";
    ASSERT_TRUE(outcome.offset_ms.has_value());
    EXPECT_DOUBLE_EQ(*outcome.offset_ms, 6.0);
}

// 探测在飞时再次发起必须被拒绝（不排队、不叠加），且 done 不会被调用。
TEST(ClockProbeTest, ProbeInFlightIsRejected) {
    // 延迟回调的传输：发包时把 handler 存起来，由测试显式结算（模拟真实异步）。
    class DeferredTransport final : public NtpTransport {
    public:
        void async_exchange(std::string host,
                            std::array<uint8_t, ntp::kPacketSize>,
                            int32_t,
                            Handler handler) override {
            hosts.push_back(host);
            handlers.push_back(std::move(handler));
        }
        std::vector<std::string> hosts;
        std::vector<Handler> handlers;
    } transport;

    FakeMoment moment;
    ClockProber prober(transport, [&moment]() { return moment(); });
    int completions = 0;
    ASSERT_TRUE(prober.probe({"a.example"}, 1500, [&completions](ProbeOutcome) { ++completions; }));
    EXPECT_TRUE(prober.probe_in_flight());
    EXPECT_FALSE(prober.probe({"a.example"}, 1500, [&completions](ProbeOutcome) { ++completions; }))
        << "a second round must not start while one is in flight";
    EXPECT_EQ(transport.hosts.size(), 1U);
    EXPECT_EQ(completions, 0);

    NtpExchange timeout;
    timeout.status = NtpExchange::Status::Timeout;
    transport.handlers[0](std::move(timeout));
    EXPECT_EQ(completions, 1);
    EXPECT_FALSE(prober.probe_in_flight());
    // 在飞结束后可以再发起一轮（限流由调用方的 judge_manual_trigger 负责，不在编排层）。
    EXPECT_TRUE(prober.probe({"a.example"}, 1500, [&completions](ProbeOutcome) { ++completions; }));
}

// 传输违约重复回调同一台时不得让计数错乱（幂等忽略），也不得重复交付本轮结果。
TEST(ClockProbeTest, DuplicateCallbackIsIgnored) {
    class DupTransport final : public NtpTransport {
    public:
        void async_exchange(std::string,
                            std::array<uint8_t, ntp::kPacketSize>,
                            int32_t,
                            Handler handler) override {
            handler_ = std::move(handler);
        }
        Handler handler_;
    } transport;

    FakeMoment moment;
    ClockProber prober(transport, [&moment]() { return moment(); });
    int completions = 0;
    ASSERT_TRUE(prober.probe({"a.example"}, 1500, [&completions](ProbeOutcome) { ++completions; }));
    NtpExchange timeout;
    timeout.status = NtpExchange::Status::Timeout;
    transport.handler_(timeout);
    EXPECT_EQ(completions, 1);
    transport.handler_(timeout);  // 违约的第二跳
    EXPECT_EQ(completions, 1) << "the round must be delivered exactly once";
}

// 快照 servers[].host 要求帧内唯一（契约 §7）：重复配置只探一次、只保留一条明细。
TEST(ClockProbeTest, DuplicateHostsAreProbedOnce) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    transport.replies = {{"dup.example", answer(10'000'000, 2'000'000)}};
    ClockProber prober(transport, [&moment]() { return moment(); });

    auto outcome_opt = run_round(prober, {"dup.example", "dup.example", "other.example"});
    ASSERT_TRUE(outcome_opt.has_value()) << "round must be delivered";
    const auto outcome = std::move(*outcome_opt);
    EXPECT_EQ(transport.sent_hosts.size(), 2U) << "duplicates collapse to one probe";
    ASSERT_EQ(outcome.servers.size(), 2U) << "one detail per unique host (contract §7)";
    EXPECT_EQ(outcome.servers[0].host, "dup.example");
    EXPECT_TRUE(outcome.servers[0].ok);
    EXPECT_EQ(outcome.servers[1].host, "other.example");
}

// 全部服务器都被拉黑时本轮不发任何包，结果同步交付且聚合值不可用。
TEST(ClockProbeTest, FullyBlacklistedRoundCompletesWithoutSending) {
    FakeTransport transport;
    FakeMoment moment;
    transport.moment = &moment;
    transport.replies = {{"deny.example", kod_reply({'D', 'E', 'N', 'Y'})}};
    ClockProber prober(transport, [&moment]() { return moment(); });

    ASSERT_TRUE(run_round(prober, {"deny.example"}).has_value());
    ASSERT_EQ(transport.sent_hosts.size(), 1U);

    int completions = 0;
    std::optional<ProbeOutcome> outcome;
    const bool started = prober.probe({"deny.example"}, 1500, [&](ProbeOutcome result) {
        outcome = std::move(result);
        ++completions;
    });
    EXPECT_TRUE(started);
    EXPECT_EQ(completions, 1) << "a round with nothing to send must still complete";
    EXPECT_EQ(transport.sent_hosts.size(), 1U) << "blacklisted host must not be contacted";
    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(outcome->offset_ms.has_value());
    ASSERT_EQ(outcome->servers.size(), 1U);
    ASSERT_TRUE(outcome->servers[0].error.has_value());
    EXPECT_EQ(*outcome->servers[0].error, "kod_deny");
    EXPECT_FALSE(prober.probe_in_flight());
}

// 手动限流判定（契约 §1.1 的纯判定，不含帧与时钟读取）。
TEST(ManualTriggerTest, JudgeManualTrigger) {
    using enum ManualTriggerDecision;
    constexpr int64_t kSecond = 1'000'000'000LL;
    // 从未触发过 => 允许
    EXPECT_EQ(judge_manual_trigger(100 * kSecond, std::nullopt, 10, false), Allowed);
    // 在飞优先于限流：即便间隔早已满足也拒绝，且原因是「在飞」
    EXPECT_EQ(judge_manual_trigger(100 * kSecond, std::nullopt, 10, true), ProbeInFlight);
    EXPECT_EQ(judge_manual_trigger(100 * kSecond, std::optional<int64_t>(0), 10, true),
              ProbeInFlight);
    // 距上次触发不足最小间隔 => 拒绝
    EXPECT_EQ(judge_manual_trigger(105 * kSecond, std::optional<int64_t>(100 * kSecond), 10, false),
              RateLimited);
    // 恰好等于最小间隔 => 允许（契约未规定严格大于）
    EXPECT_EQ(judge_manual_trigger(110 * kSecond, std::optional<int64_t>(100 * kSecond), 10, false),
              Allowed);
    // 超过最小间隔 => 允许
    EXPECT_EQ(judge_manual_trigger(200 * kSecond, std::optional<int64_t>(100 * kSecond), 10, false),
              Allowed);
    // min_interval = 0 => 不限流
    EXPECT_EQ(judge_manual_trigger(100 * kSecond, std::optional<int64_t>(100 * kSecond), 0, false),
              Allowed);
    // 单调读数倒退（调用方传错）：按「刚触发过」处理，不放大成两轮探测
    EXPECT_EQ(judge_manual_trigger(99 * kSecond, std::optional<int64_t>(100 * kSecond), 10, false),
              RateLimited);
}

}  // namespace dztrader::sysmon
