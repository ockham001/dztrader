// 契约 sysmon §2.7 / §2.9：NTPv4 客户端模式报文编解码与偏差派生纯函数。
// 断言外部可观察行为：请求字节布局、响应解析、origin 回显/KoD/未同步三种拒收、era 回绕、
// 分数与定点换算精度、偏差符号（正 = 本地快）。期望值全部来自手算表与 RFC 5905 的字段
// 定义，不调用被测实现推导。

#include "ntp_codec.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace dztrader::sysmon::ntp {
namespace {

/// 2026-09-21T14:13:20Z 的 NTP 秒字段（Unix 1790000000 + 2208988800 = 3998988800）。
constexpr uint32_t kNtpAnchorSec = 3'998'988'800U;

/// 构造 48 字节报文（逐字段按 RFC 5905 §7.3 的布局写入，不经过被测代码）。
std::array<uint8_t, kPacketSize> make_packet(uint8_t leap,
                                             uint8_t version,
                                             uint8_t mode,
                                             uint8_t stratum,
                                             uint32_t root_delay_raw,  // 16.16 定点（有符号位）
                                             uint32_t root_dispersion_raw,
                                             std::array<char, 4> reference_id,
                                             Timestamp origin,
                                             Timestamp receive,
                                             Timestamp transmit) {
    std::array<uint8_t, kPacketSize> packet{};
    packet[0] =
        static_cast<uint8_t>(((leap & 0x3U) << 6) | ((version & 0x7U) << 3) | (mode & 0x7U));
    packet[1] = stratum;
    packet[2] = 4;                            // poll
    packet[3] = static_cast<uint8_t>(0xF6U);  // precision = -10
    const auto put32 = [&packet](std::size_t offset, uint32_t value) {
        packet[offset] = static_cast<uint8_t>((value >> 24) & 0xFFU);
        packet[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFFU);
        packet[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFFU);
        packet[offset + 3] = static_cast<uint8_t>(value & 0xFFU);
    };
    put32(4, root_delay_raw);
    put32(8, root_dispersion_raw);
    // reference id 是四个 ASCII 字符（stratum = 0 时即 KoD 代码）
    packet[12] = static_cast<uint8_t>(reference_id[0]);
    packet[13] = static_cast<uint8_t>(reference_id[1]);
    packet[14] = static_cast<uint8_t>(reference_id[2]);
    packet[15] = static_cast<uint8_t>(reference_id[3]);
    const auto put_ts = [&put32](std::size_t offset, Timestamp ts) {
        put32(offset, ts.seconds);
        put32(offset + 4, ts.fraction);
    };
    put_ts(16, Timestamp{});  // reference timestamp（本模块不消费）
    put_ts(24, origin);
    put_ts(32, receive);
    put_ts(40, transmit);
    return packet;
}

/// 独立的分数换算 oracle：`fraction = round(ns × 2^32 / 1e9)`。
///
/// 刻意**不**调用被测的 `nanoseconds_to_fraction()`——夹具与被测实现互为 oracle 会让
/// 「两边一致地算错」也保持绿色（review-checklist §2.2 对比测试的 oracle 不得与被测实现共享）。
constexpr uint32_t fraction_of_ns_oracle(int64_t ns) {
    return static_cast<uint32_t>((ns * ((1LL << 32) + 0)) / 1'000'000'000LL);
}

/// 同秒内按纳秒偏移构造时间戳（不跨秒，保持用例可读；分数用独立 oracle 换算）。
Timestamp stamp_at(uint32_t seconds, int64_t offset_ns) {
    Timestamp stamp;
    stamp.seconds = seconds;
    stamp.fraction = fraction_of_ns_oracle(offset_ns);
    return stamp;
}

/// 常规可用响应：stratum 2、root delay 1/16 s、root dispersion 1/32 s、origin 回显正确。
std::array<uint8_t, kPacketSize> make_usable_packet(Timestamp origin,
                                                    Timestamp receive,
                                                    Timestamp transmit) {
    return make_packet(/*leap=*/0, /*version=*/4, kModeServer, /*stratum=*/2,
                       /*root_delay_raw=*/0x0000'1000U, /*root_dispersion_raw=*/0x0000'0800U,
                       {'G', 'P', 'S', '\0'}, origin, receive, transmit);
}

/// 构造 KoD 响应（stratum = 0，reference id 即四个 ASCII 字符）。
std::array<uint8_t, kPacketSize> make_kod_packet(std::array<char, 4> code, Timestamp origin) {
    return make_packet(/*leap=*/3, /*version=*/4, kModeServer, /*stratum=*/0, 0, 0, code, origin,
                       origin, origin);
}

}  // namespace

// AC：请求字段布局——48 字节、LI=0 / VN=4 / Mode=3、transmit timestamp 填入发送时刻、其余全 0。
TEST(NtpCodecTest, BuildRequestLayout) {
    Timestamp transmit;
    transmit.seconds = kNtpAnchorSec;
    transmit.fraction = 0x4000'0000U;  // 0.25 s
    const auto request = build_request(transmit);
    ASSERT_EQ(request.size(), 48U);
    EXPECT_EQ(request[0], 0x23U) << "expected LI=0 VN=4 Mode=3 (0b00100011)";
    for (std::size_t i = 1; i < 40; ++i) {
        EXPECT_EQ(request[i], 0U) << "byte " << i << " must be zero";
    }
    // transmit timestamp：秒 3998988800 = 0xEE5BBA00、分数 0x40000000
    EXPECT_EQ(request[40], 0xEEU);
    EXPECT_EQ(request[41], 0x5BU);
    EXPECT_EQ(request[42], 0xBAU);
    EXPECT_EQ(request[43], 0x00U);
    EXPECT_EQ(request[44], 0x40U);
    EXPECT_EQ(request[45], 0x00U);
    EXPECT_EQ(request[46], 0x00U);
    EXPECT_EQ(request[47], 0x00U);
}

// AC：响应解析——LI/VN/Mode、stratum、root delay/dispersion、origin/receive/transmit。
TEST(NtpCodecTest, ParseResponseFields) {
    const auto origin = stamp_at(kNtpAnchorSec, 250'000'000);    // 0x40000000
    const auto receive = stamp_at(kNtpAnchorSec, 312'500'000);   // 0x50000000
    const auto transmit = stamp_at(kNtpAnchorSec, 375'000'000);  // 0x60000000
    const auto packet = make_usable_packet(origin, receive, transmit);

    ParseError error = ParseError::BadMode;
    const auto response = parse_response(packet, &error);
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(error, ParseError::None);
    EXPECT_EQ(response->leap, 0);
    EXPECT_EQ(response->version, 4);
    EXPECT_EQ(response->mode, kModeServer);
    EXPECT_EQ(response->stratum, 2);
    EXPECT_EQ(response->poll, 4);
    EXPECT_EQ(response->precision, -10);
    EXPECT_EQ(response->root_delay, 62'500'000) << "0x1000 = 1/16 s = 62.5 ms";
    EXPECT_EQ(response->root_dispersion, 31'250'000) << "0x0800 = 1/32 s = 31.25 ms";
    EXPECT_EQ(response->reference_id, 0x47505300U) << "'GPS\\0' big-endian";
    EXPECT_EQ(response->origin.seconds, kNtpAnchorSec);
    EXPECT_EQ(response->origin.fraction, 0x4000'0000U);
    EXPECT_EQ(response->receive.fraction, 0x5000'0000U);
    EXPECT_EQ(response->transmit.fraction, 0x6000'0000U);
}

// AC：畸形包拒绝——长度不足、版本不符、mode 不是服务端。
TEST(NtpCodecTest, RejectsMalformedPackets) {
    const auto origin = stamp_at(kNtpAnchorSec, 0);
    const auto good = make_usable_packet(origin, origin, origin);

    ParseError error = ParseError::None;
    EXPECT_FALSE(parse_response(std::span<const uint8_t>(good.data(), 47), &error).has_value());
    EXPECT_EQ(error, ParseError::TooShort);

    const auto bad_version = make_packet(0, /*version=*/2, kModeServer, 2, 0, 0,
                                         {'\0', '\0', '\0', '\0'}, origin, origin, origin);
    EXPECT_FALSE(parse_response(bad_version, &error).has_value());
    EXPECT_EQ(error, ParseError::BadVersion);

    const auto bad_mode = make_packet(0, /*version=*/4, /*mode=*/3, 2, 0, 0,
                                      {'\0', '\0', '\0', '\0'}, origin, origin, origin);
    EXPECT_FALSE(parse_response(bad_mode, &error).has_value());
    EXPECT_EQ(error, ParseError::BadMode);

    // v3 服务端仍常见，必须接受（报文布局与 v4 相同）。
    const auto v3 = make_packet(0, /*version=*/3, kModeServer, 2, 0, 0, {'\0', '\0', '\0', '\0'},
                                origin, origin, origin);
    const auto parsed = parse_response(v3, &error);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->version, 3);
    EXPECT_EQ(error, ParseError::None);
}

// AC：origin 回显不匹配时拒绝（防伪造与迟到包）。
TEST(NtpCodecTest, RejectsOriginEchoMismatch) {
    const auto t1 = stamp_at(kNtpAnchorSec, 250'000'000);
    auto forged = t1;
    forged.fraction = t1.fraction + 1U;  // 差一个最小分数单位
    const auto packet = make_usable_packet(forged, t1, t1);

    const auto response = parse_response(packet);
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(classify_response(*response, t1), Verdict::BadEcho);
    EXPECT_EQ(classify_response(*response, forged), Verdict::Usable);
}

// AC：stratum=0 是 KoD，时间戳无定义、绝不参与计算；stratum>=16 的参考源同样不采用。
TEST(NtpCodecTest, RejectsKissOfDeathAndUnsynchronisedSource) {
    const auto t1 = stamp_at(kNtpAnchorSec, 0);
    const auto kod_response = parse_response(make_kod_packet({'R', 'A', 'T', 'E'}, t1));
    ASSERT_TRUE(kod_response.has_value());
    EXPECT_EQ(classify_response(*kod_response, t1), Verdict::KissOfDeath);
    EXPECT_EQ(kiss_code(*kod_response), KissCode::Rate);

    // 判定顺序是语义：KoD 包的时间戳无定义，其 origin 常常也算不出来。即使回显不匹配，
    // 也必须先归为 KoD（否则 KoD 代码会被回显校验吞掉，退避/拉黑永远不会发生）。
    auto kod_bad_echo = make_kod_packet({'R', 'A', 'T', 'E'}, t1);
    // 把 origin 段改成与我方 T1 不同的值
    const auto put32_origin = [&kod_bad_echo](std::size_t offset, uint32_t value) {
        kod_bad_echo[offset] = static_cast<uint8_t>((value >> 24) & 0xFFU);
        kod_bad_echo[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFFU);
        kod_bad_echo[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFFU);
        kod_bad_echo[offset + 3] = static_cast<uint8_t>(value & 0xFFU);
    };
    put32_origin(24, 0x1122'3344U);
    const auto kod_bad_echo_response = parse_response(kod_bad_echo);
    ASSERT_TRUE(kod_bad_echo_response.has_value());
    EXPECT_EQ(classify_response(*kod_bad_echo_response, t1), Verdict::KissOfDeath)
        << "KoD must be classified before the origin-echo check";
    EXPECT_EQ(kiss_code(*kod_bad_echo_response), KissCode::Rate);

    // 服务端把时间戳照抄回我方 T1：若按普通包计算会得到「完美 0 偏差」，必须由 stratum 判定拦下。
    const auto unsync =
        make_packet(0, 4, kModeServer, /*stratum=*/16, 0, 0, {'\0', '\0', '\0', '\0'}, t1, t1, t1);
    const auto unsync_response = parse_response(unsync);
    ASSERT_TRUE(unsync_response.has_value());
    EXPECT_EQ(classify_response(*unsync_response, t1), Verdict::Unsynchronised);
    EXPECT_EQ(kiss_code(*unsync_response), KissCode::None);
}

// AC：KoD 代码取值——RATE / DENY / RSTR 各有处置，其余 KoD 归为「忽略」。
TEST(NtpCodecTest, ReadsKissCodes) {
    const auto t1 = stamp_at(kNtpAnchorSec, 0);
    const auto code_of = [](std::array<char, 4> code) {
        const auto response = parse_response(make_kod_packet(code, Timestamp{}));
        return response.has_value() ? kiss_code(*response) : KissCode::None;
    };
    EXPECT_EQ(code_of({'R', 'A', 'T', 'E'}), KissCode::Rate);
    EXPECT_EQ(code_of({'D', 'E', 'N', 'Y'}), KissCode::Deny);
    EXPECT_EQ(code_of({'R', 'S', 'T', 'R'}), KissCode::Restrict);
    EXPECT_EQ(code_of({'I', 'N', 'I', 'T'}), KissCode::Other);
    EXPECT_EQ(code_of({'A', 'U', 'T', 'H'}), KissCode::Other);

    // 非 KoD 响应没有 KoD 代码（即使 stratum 已拦下，也不能把 reference id 当 KoD 读）。
    const auto normal = parse_response(make_usable_packet(t1, t1, t1));
    ASSERT_TRUE(normal.has_value());
    EXPECT_EQ(kiss_code(*normal), KissCode::None);
}

// AC：θ/δ/bound 计算——正偏差 = 本地快；负偏差 = 本地慢；bound = ½·RTT + root dispersion。
TEST(NtpCodecTest, DerivesOffsetRttAndBound) {
    // 本地比服务端快 12.5 ms，去/回各 30 ms，服务端处理 0：
    //   T1 = 0、T2 = T3 = 30 − 12.5 = 17.5 ms、T4 = 60 ms（均以本地墙钟为参照的报文读数）。
    Timestamps fast_local;
    fast_local.t1 = stamp_at(kNtpAnchorSec, 0);
    fast_local.t2 = stamp_at(kNtpAnchorSec, 17'500'000);
    fast_local.t3 = fast_local.t2;
    fast_local.t4 = stamp_at(kNtpAnchorSec, 60'000'000);

    const auto fast_sample = derive_sample(fast_local, /*steady_rtt_ns=*/60'000'000,
                                           /*root_dispersion_ns=*/30'000'000);
    ASSERT_TRUE(fast_sample.has_value());
    EXPECT_EQ(fast_sample->offset_ns, 12'500'000) << "local ahead => positive (contract §2.9)";
    EXPECT_EQ(fast_sample->rtt_ns, 60'000'000);
    EXPECT_EQ(fast_sample->bound_ns, 60'000'000);

    // 本地比服务端慢 25 ms，去/回各 40 ms：
    //   T1 = 0、T2 = T3 = 40 + 25 = 65 ms、T4 = 80 ms。
    Timestamps slow_local;
    slow_local.t1 = stamp_at(kNtpAnchorSec, 0);
    slow_local.t2 = stamp_at(kNtpAnchorSec, 65'000'000);
    slow_local.t3 = slow_local.t2;
    slow_local.t4 = stamp_at(kNtpAnchorSec, 80'000'000);

    const auto slow_sample = derive_sample(slow_local, 80'000'000, 10'000'000);
    ASSERT_TRUE(slow_sample.has_value());
    EXPECT_EQ(slow_sample->offset_ns, -25'000'000) << "local behind => negative";
    EXPECT_EQ(slow_sample->rtt_ns, 80'000'000);
    EXPECT_EQ(slow_sample->bound_ns, 50'000'000);

    // 服务端处理时间 20 ms：往返只扣网络部分，偏差不受处理时间影响（T2/T3 同步后移）。
    Timestamps processed;
    processed.t1 = stamp_at(kNtpAnchorSec, 0);
    processed.t2 = stamp_at(kNtpAnchorSec, 30'000'000);  // T1 + 30 ms
    processed.t3 = stamp_at(kNtpAnchorSec, 50'000'000);  // T2 + 20 ms
    processed.t4 = stamp_at(kNtpAnchorSec, 80'000'000);  // 墙钟往返 80 ms
    const auto processed_sample = derive_sample(processed, 80'000'000, 0);
    ASSERT_TRUE(processed_sample.has_value());
    EXPECT_EQ(processed_sample->rtt_ns, 60'000'000) << "wall rtt minus server processing";
    EXPECT_EQ(processed_sample->bound_ns, 30'000'000);
    EXPECT_EQ(processed_sample->offset_ns, 0) << "symmetric network, no skew => zero offset";
}

// AC：服务端处理时间为负、或单调往返小于服务端处理时间 => 样本不可信，必须拒收。
TEST(NtpCodecTest, RejectsSelfContradictoryTimestamps) {
    Timestamps contradiction;
    contradiction.t1 = stamp_at(kNtpAnchorSec, 0);
    contradiction.t2 = stamp_at(kNtpAnchorSec, 20'000'000);
    contradiction.t3 = stamp_at(kNtpAnchorSec, 10'000'000);  // T3 < T2
    contradiction.t4 = stamp_at(kNtpAnchorSec, 40'000'000);
    EXPECT_FALSE(derive_sample(contradiction, 40'000'000, 0).has_value());

    Timestamps negative_rtt;
    negative_rtt.t1 = stamp_at(kNtpAnchorSec, 0);
    negative_rtt.t2 = stamp_at(kNtpAnchorSec, 10'000'000);
    negative_rtt.t3 = stamp_at(kNtpAnchorSec, 20'000'000);  // 服务端处理 10 ms
    negative_rtt.t4 = stamp_at(kNtpAnchorSec, 30'000'000);
    EXPECT_FALSE(derive_sample(negative_rtt, 5'000'000, 0).has_value())
        << "steady rtt below server processing time";
    EXPECT_TRUE(derive_sample(negative_rtt, 30'000'000, 0).has_value());
}

// AC：era 回绕——2036-02-07 之后秒字段回到 0，差值必须按 era 展开而不是变成负数。
TEST(NtpCodecTest, DifferenceExpandsEraWrap) {
    Timestamp before_wrap;
    before_wrap.seconds = 0xFFFF'FF00U;  // 回绕前 256 秒
    Timestamp after_wrap;
    after_wrap.seconds = 0x0000'0100U;  // 回绕后 256 秒
    EXPECT_EQ(difference_nanoseconds(after_wrap, before_wrap), 512LL * 1'000'000'000LL);
    EXPECT_EQ(difference_nanoseconds(before_wrap, after_wrap), -512LL * 1'000'000'000LL);

    // 同一秒内、分数反向：差 0.5 s
    const auto low = stamp_at(kNtpAnchorSec, 0);
    const auto high = stamp_at(kNtpAnchorSec, 500'000'000);
    EXPECT_EQ(difference_nanoseconds(high, low), 500'000'000);
    EXPECT_EQ(difference_nanoseconds(low, high), -500'000'000);
}

// AC：分数换算精度——整数运算、不用浮点；边界值不越界、不把「1.0 秒」编码成非法分数。
TEST(NtpCodecTest, FractionConversionPrecision) {
    EXPECT_EQ(fraction_to_nanoseconds(0U), 0);
    EXPECT_EQ(fraction_to_nanoseconds(0x8000'0000U), 500'000'000);
    EXPECT_EQ(fraction_to_nanoseconds(0x4000'0000U), 250'000'000);
    EXPECT_EQ(fraction_to_nanoseconds(0x2000'0000U), 125'000'000);
    EXPECT_EQ(fraction_to_nanoseconds(0x1000'0000U), 62'500'000);
    EXPECT_EQ(fraction_to_nanoseconds(0xE000'0000U), 875'000'000);
    EXPECT_EQ(fraction_to_nanoseconds(0x0000'0001U), 0) << "1 LSB = 0.233 ns，四舍五入为 0";
    EXPECT_EQ(fraction_to_nanoseconds(0xFFFF'FFFFU), 999'999'999)
        << "最大分数必须钳制在 1 s 以内，不得进位成 1000000000";

    EXPECT_EQ(nanoseconds_to_fraction(0), 0U);
    EXPECT_EQ(nanoseconds_to_fraction(500'000'000), 0x8000'0000U);
    EXPECT_EQ(nanoseconds_to_fraction(250'000'000), 0x4000'0000U);
    EXPECT_EQ(nanoseconds_to_fraction(1'000'000'000), 0xFFFF'FFFCU)
        << "输入钳制到 [0, 999999999] 后不得溢出成 2^32 或进位到下一秒";

    // 往返一致必须与**独立手算表**对齐，不能让两个方向互相验证——那样两边一起算错也保持绿色
    // （review-checklist §2.2：对比测试的 oracle 不得与被测实现共享）。
    // 先断言 fraction -> ns 命中手算值，再断言这些手算 ns 编码回分数命中同一原始值。
    const std::array<std::pair<uint32_t, int64_t>, 6> exact_pairs{{
        {0x0000'0000U, 0},
        {0x1000'0000U, 62'500'000},
        {0x2000'0000U, 125'000'000},
        {0x4000'0000U, 250'000'000},
        {0x8000'0000U, 500'000'000},
        {0xE000'0000U, 875'000'000},
    }};
    for (const auto& [fraction, expected_ns] : exact_pairs) {
        EXPECT_EQ(fraction_to_nanoseconds(fraction), expected_ns) << "fraction " << fraction;
        EXPECT_EQ(nanoseconds_to_fraction(expected_ns), fraction) << "ns " << expected_ns;
    }
    // 不可精确表示的组合只能就近取整，但误差必须 <= 1 ns（保亚微秒精度）
    EXPECT_EQ(fraction_to_nanoseconds(0x5555'5555U), 333'333'333);
    EXPECT_EQ(nanoseconds_to_fraction(333'333'333), 0x5555'5554U);
}

// 16.16 定点：root delay 有符号、root dispersion 无符号，换算到纳秒（四舍五入）。
TEST(NtpCodecTest, FixedPointConversion) {
    EXPECT_EQ(fixed_point_nanoseconds(0), 0);
    EXPECT_EQ(fixed_point_nanoseconds(0x0000'8000), 500'000'000);    // 0.5 s
    EXPECT_EQ(fixed_point_nanoseconds(0x0001'0000), 1'000'000'000);  // 1.0 s
    EXPECT_EQ(fixed_point_nanoseconds(1), 15'259)                    // 1 LSB = 15258.79 ns
        << "round half up, not truncation";
    // root delay 为有符号：0xFFFF8000 表示 -0.5 s
    const auto negative = static_cast<int32_t>(0xFFFF'8000U);
    EXPECT_EQ(fixed_point_nanoseconds(negative), -500'000'000);
}

// Unix 纳秒 -> NTP 时间戳：秒按 era 回绕、分数四舍五入、边界不溢出。
TEST(NtpCodecTest, ConvertsUnixNanosecondsToTimestamp) {
    const auto anchor = from_unix_nanoseconds(1'790'000'000LL * 1'000'000'000LL + 250'000'000LL);
    EXPECT_EQ(anchor.seconds, kNtpAnchorSec);
    EXPECT_EQ(anchor.fraction, 0x4000'0000U);

    // 最后一纳秒仍是本秒的最大分数：不得溢出成 2^32、也不得跨到下一秒（避免出现「分数 = 1.0」）
    const auto last_nano = from_unix_nanoseconds(1'790'000'000LL * 1'000'000'000LL + 999'999'999LL);
    EXPECT_EQ(last_nano.seconds, kNtpAnchorSec);
    EXPECT_EQ(last_nano.fraction, 0xFFFF'FFFCU);
    // 与时间戳往返一致（纳秒精度内）
    EXPECT_EQ(fraction_to_nanoseconds(last_nano.fraction), 999'999'999);
}

// 长度 > 48 的报文合法（多余部分是扩展字段/MAC），必须正常解析而不是判为畸形。
TEST(NtpCodecTest, AcceptsOversizedPacket) {
    const auto origin = stamp_at(kNtpAnchorSec, 0);
    const auto base = make_usable_packet(origin, origin, origin);
    auto oversized = std::vector<uint8_t>(base.begin(), base.end());
    oversized.resize(68, 0xAB);  // 追加 20 字节扩展字段
    const auto response = parse_response(oversized);
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->stratum, 2);
    EXPECT_EQ(response->origin.fraction, origin.fraction);
}

// era 展开的半周期边界：|差值| < 2^31 秒按原方向，> 2^31 秒按「最近表示」换 era。
// 恰好 2^31 秒两向等距（无唯一最近表示），实现按「不换 era」处理，此处不断言该歧义点。
TEST(NtpCodecTest, DifferenceEraBoundary) {
    const Timestamp base{/*seconds=*/0U, /*fraction=*/0U};
    const Timestamp just_below{/*seconds=*/(1U << 31) - 1U, /*fraction=*/0U};
    EXPECT_EQ(difference_nanoseconds(just_below, base),
              static_cast<int64_t>(just_below.seconds) * 1'000'000'000LL)
        << "just below half an era keeps the forward direction";
    // 2^31 + 1 秒：超过半周期 => 反方向的最近表示 = -(2^31 - 1) 秒
    const Timestamp past_half{/*seconds=*/(1U << 31) + 1U, /*fraction=*/0U};
    EXPECT_EQ(difference_nanoseconds(past_half, base), -((1LL << 31) - 1) * 1'000'000'000LL)
        << "past half an era flips to the nearest representation";
}

// nanoseconds_to_fraction 的负输入必须钳制到 0（契约 §2.7「分数部分用整数运算」，
// 负分数是非法值，不得回绕成巨大的分数）。
TEST(NtpCodecTest, NegativeNanosecondsClampToZeroFraction) {
    EXPECT_EQ(nanoseconds_to_fraction(-1), 0U);
    EXPECT_EQ(nanoseconds_to_fraction(-1'000'000'000), 0U);
    EXPECT_EQ(nanoseconds_to_fraction(std::numeric_limits<int64_t>::min()), 0U);
}

// 负 Unix 纳秒属调用方违约（precondition >= 0）：返回纪元原点，不回绕出看似合法的 era 值。
TEST(NtpCodecTest, FromUnixNanosecondsRejectsNegative) {
    const auto stamp = from_unix_nanoseconds(-1);
    EXPECT_EQ(stamp.seconds, 0U) << "negative input must not wrap into a plausible era value";
    EXPECT_EQ(stamp.fraction, 0U);
}

// 解析失败时必须给出失败原因（编排层据此产出 bad_echo 明细），默认参数可省略。
TEST(NtpCodecTest, ParseErrorReasonsReported) {
    EXPECT_FALSE(parse_response(std::span<const uint8_t>{}).has_value());
    const std::vector<uint8_t> truncated(20, 0);
    ParseError error = ParseError::None;
    EXPECT_FALSE(parse_response(truncated, &error).has_value());
    EXPECT_EQ(error, ParseError::TooShort);
}

}  // namespace dztrader::sysmon::ntp
