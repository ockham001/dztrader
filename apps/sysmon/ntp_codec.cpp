#include "ntp_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dztrader::sysmon::ntp {
namespace {

/// 2^32：NTP 分数部分的分母，同时是秒字段的 era 回绕周期。
constexpr int64_t kFractionScale = 1LL << 32;

/// NTP era 的秒数（2^32 秒 ≈ 136 年）：秒字段回绕周期。
constexpr int64_t kEraSeconds = 1LL << 32;

/// 把「差值」按 era 展开时的半周期（2^31 秒 ≈ 68 年，远大于探测往返）。
constexpr int64_t kHalfEraSeconds = kEraSeconds / 2;

/// 2^16：16.16 定点秒的小数分母。
constexpr int64_t kFixedPointScale = 1LL << 16;

/// 纳秒/秒。
constexpr int64_t kNanosecondsPerSecond = 1'000'000'000LL;

/// 最大可表示的分数纳秒值（钳制上界：分数部分必须是 [0, 1) 秒）。
constexpr int64_t kMaxFractionNanoseconds = kNanosecondsPerSecond - 1;

/// 读大端 32 位。
uint32_t read_be32(std::span<const uint8_t> packet, std::size_t offset) noexcept {
    return (static_cast<uint32_t>(packet[offset]) << 24) |
           (static_cast<uint32_t>(packet[offset + 1]) << 16) |
           (static_cast<uint32_t>(packet[offset + 2]) << 8) |
           static_cast<uint32_t>(packet[offset + 3]);
}

/// 读大端 64 位时间戳（秒在前、分数在后）。
Timestamp read_timestamp(std::span<const uint8_t> packet, std::size_t offset) noexcept {
    Timestamp stamp;
    stamp.seconds = read_be32(packet, offset);
    stamp.fraction = read_be32(packet, offset + 4);
    return stamp;
}

/// 四舍五入的整数除法（被除数非负，除数为正）。
int64_t round_div(int64_t numerator, int64_t denominator) noexcept {
    return (numerator + denominator / 2) / denominator;
}

}  // namespace

Timestamp from_unix_nanoseconds(int64_t unix_ns) noexcept {
    if (unix_ns < 0) {
        // 1970 年之前的时刻不在本模块的重心上（方案基线内不可能出现）；
        // 归零而不是回绕出一个无意义的 era 值。
        return Timestamp{};
    }
    const int64_t seconds = unix_ns / kNanosecondsPerSecond;
    const int64_t nanos = unix_ns % kNanosecondsPerSecond;
    Timestamp stamp;
    // 秒字段按 32 位 era 回绕（2^32 秒 ≈ 136 年，2036-02-07 之后自然回到 0 起算）。
    stamp.seconds = static_cast<uint32_t>((seconds + kUnixEpochOffsetSec) & 0xFFFF'FFFFLL);
    stamp.fraction = nanoseconds_to_fraction(nanos);
    return stamp;
}

int64_t fraction_to_nanoseconds(uint32_t fraction) noexcept {
    // 整数运算：fraction * 1e9 / 2^32，四舍五入（加半个分母）。乘积用 64 位足够：
    // 2^32 * 1e9 ≈ 4.3e18 < 9.2e18。
    const int64_t nanos =
        round_div(static_cast<int64_t>(fraction) * kNanosecondsPerSecond, kFractionScale);
    return std::min(nanos, kMaxFractionNanoseconds);
}

uint32_t nanoseconds_to_fraction(int64_t nanos) noexcept {
    const int64_t clamped = std::clamp<int64_t>(nanos, 0, kMaxFractionNanoseconds);
    // 整数运算：nanos * 2^32 / 1e9，四舍五入。结果 <= 0xFFFFFFFC，不会进位到 2^32。
    const int64_t fraction = round_div(clamped * kFractionScale, kNanosecondsPerSecond);
    return static_cast<uint32_t>(std::min<int64_t>(fraction, 0xFFFF'FFFFLL));
}

int64_t fixed_point_nanoseconds(int64_t fixed) noexcept {
    // 16.16 定点秒 -> 纳秒，四舍五入；这里同时承担 root delay 的负值（有符号）
    // 与 root dispersion 的负值不可能出现（无符号读法由调用方保证）。
    const int64_t numerator = fixed * kNanosecondsPerSecond;
    if (numerator >= 0) {
        return round_div(numerator, kFixedPointScale);
    }
    return -round_div(-numerator, kFixedPointScale);
}

int64_t difference_nanoseconds(Timestamp later, Timestamp earlier) noexcept {
    // 秒字段按 era 展开：把 32 位有符号解释成「以半周期为界的最近差值」。
    int64_t seconds = static_cast<int64_t>(later.seconds) - static_cast<int64_t>(earlier.seconds);
    if (seconds > kHalfEraSeconds) {
        seconds -= kEraSeconds;
    } else if (seconds < -kHalfEraSeconds) {
        seconds += kEraSeconds;
    }
    const int64_t fraction =
        fraction_to_nanoseconds(later.fraction) - fraction_to_nanoseconds(earlier.fraction);
    return seconds * kNanosecondsPerSecond + fraction;
}

std::array<uint8_t, kPacketSize> build_request(Timestamp transmit) noexcept {
    std::array<uint8_t, kPacketSize> packet{};
    // LI = 0（无闰秒预告）、VN = 4、Mode = 3（客户端）。
    packet[0] = static_cast<uint8_t>((kClientVersion << 3) | kModeClient);
    const auto put32 = [&packet](std::size_t offset, uint32_t value) {
        packet[offset] = static_cast<uint8_t>((value >> 24) & 0xFFU);
        packet[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFFU);
        packet[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFFU);
        packet[offset + 3] = static_cast<uint8_t>(value & 0xFFU);
    };
    // 其余字段（stratum / poll / precision / root delay / root dispersion / reference id /
    // reference / origin / receive）在客户端请求里不承载语义，保持 0（RFC 5905 §7.3）。
    put32(40, transmit.seconds);
    put32(44, transmit.fraction);
    return packet;
}

std::optional<Response> parse_response(std::span<const uint8_t> packet,
                                       ParseError* error) noexcept {
    const auto fail = [error](ParseError reason) -> std::optional<Response> {
        if (error != nullptr) {
            *error = reason;
        }
        return std::nullopt;
    };
    if (error != nullptr) {
        *error = ParseError::None;
    }
    if (packet.size() < kPacketSize) {
        return fail(ParseError::TooShort);
    }
    const uint8_t version = static_cast<uint8_t>((packet[0] >> 3) & 0x7U);
    if (version < kMinServerVersion || version > kMaxServerVersion) {
        return fail(ParseError::BadVersion);
    }
    const uint8_t mode = static_cast<uint8_t>(packet[0] & 0x7U);
    if (mode != kModeServer) {
        return fail(ParseError::BadMode);
    }
    Response response;
    response.leap = static_cast<uint8_t>((packet[0] >> 6) & 0x3U);
    response.version = version;
    response.mode = mode;
    response.stratum = packet[1];
    response.poll = packet[2];
    response.precision = static_cast<int8_t>(packet[3]);
    // root delay 是有符号 16.16 定点秒；root dispersion 是无符号 16.16 定点秒（RFC 5905 §7.3）。
    response.root_delay = fixed_point_nanoseconds(static_cast<int32_t>(read_be32(packet, 4)));
    response.root_dispersion = fixed_point_nanoseconds(static_cast<int64_t>(read_be32(packet, 8)));
    response.reference_id = read_be32(packet, 12);
    response.origin = read_timestamp(packet, 24);
    response.receive = read_timestamp(packet, 32);
    response.transmit = read_timestamp(packet, 40);
    return response;
}

Verdict classify_response(const Response& response, Timestamp expected_origin) noexcept {
    // 顺序是语义：KoD 先判——它携带的 KoD 代码决定退避/拉黑，而它的时间戳本身无定义。
    if (response.stratum == 0) {
        return Verdict::KissOfDeath;
    }
    if (response.stratum >= 16) {
        return Verdict::Unsynchronised;
    }
    if (response.origin.seconds != expected_origin.seconds ||
        response.origin.fraction != expected_origin.fraction) {
        return Verdict::BadEcho;
    }
    return Verdict::Usable;
}

KissCode kiss_code(const Response& response) noexcept {
    if (response.stratum != 0) {
        return KissCode::None;
    }
    std::array<char, 4> code{};
    for (std::size_t i = 0; i < code.size(); ++i) {
        code[i] = static_cast<char>((response.reference_id >> ((3 - i) * 8)) & 0xFFU);
    }
    if (code == std::array<char, 4>{'R', 'A', 'T', 'E'}) {
        return KissCode::Rate;
    }
    if (code == std::array<char, 4>{'D', 'E', 'N', 'Y'}) {
        return KissCode::Deny;
    }
    if (code == std::array<char, 4>{'R', 'S', 'T', 'R'}) {
        return KissCode::Restrict;
    }
    return KissCode::Other;
}

std::optional<Sample> derive_sample(const Timestamps& timestamps,
                                    int64_t steady_rtt_ns,
                                    int64_t root_dispersion_ns) noexcept {
    const int64_t server_processing_ns = difference_nanoseconds(timestamps.t3, timestamps.t2);
    if (server_processing_ns < 0) {
        // T3 < T2：服务端自己的两个时间戳自相矛盾（坏实现或被篡改），样本不可信。
        return std::nullopt;
    }
    const int64_t rtt_ns = steady_rtt_ns - server_processing_ns;
    if (rtt_ns < 0) {
        // 单调时钟测得的往返小于服务端处理时间：时间戳与被测窗口不属于同一次交互。
        return std::nullopt;
    }
    const int64_t t1_minus_t2_ns = difference_nanoseconds(timestamps.t1, timestamps.t2);
    const int64_t t4_minus_t3_ns = difference_nanoseconds(timestamps.t4, timestamps.t3);
    Sample sample;
    // 契约 §2.9 / GLOSSARY：offset_ms 正 = 本地快。RFC 5905 的 θ = ½[(T2−T1)+(T3−T4)] 按
    // 其定义（服务端时间 − 本地时间）正值表示本地慢，因此这里用等价的取负形式：
    //   offset = −θ = ½[(T1−T2) + (T4−T3)]
    sample.offset_ns = (t1_minus_t2_ns + t4_minus_t3_ns) / 2;
    sample.rtt_ns = rtt_ns;
    // 误差上界 = ½·RTT + 服务端 root dispersion；偏差与误差界同时产出。
    sample.bound_ns = rtt_ns / 2 + root_dispersion_ns;
    return sample;
}

}  // namespace dztrader::sysmon::ntp
