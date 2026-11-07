#ifndef DZTRADER_SYSMON_NTP_CODEC_H_
#define DZTRADER_SYSMON_NTP_CODEC_H_

/**
 * @file ntp_codec.h
 * @brief NTPv4 客户端模式的报文编解码与偏差派生纯函数（契约 sysmon §2.7、§2.9）。
 *
 * 本文件只做「字节数组 ↔ 结构体 ↔ 派生量」，不做任何 I/O、不读时钟、不打日志：
 * - 请求固定 48 字节（LI=0、VN=4、Mode=3），transmit timestamp 填入我方发送时刻
 * - 响应解析出 LI/VN/Mode、stratum、root delay/dispersion 与 origin/receive/transmit 时间戳
 * - 时间戳为 64 位（32 位秒 + 32 位分数，纪元 1900-01-01）；差值按 era 展开处理
 *   2036-02-07 回绕；分数与定点换算全部用整数运算（保亚微秒精度，不用浮点）
 * - 偏差 `offset_ms` 的正负按 **payload 语义**：正值 = 本地快（契约 §2.9 / GLOSSARY）。
 *   RFC 5905 的 θ 按其定义（服务端时间 − 本地时间）为正表示本地慢，故此处取负号，
 *   见 `derive_sample()` 的注释
 *
 * 报文校验（契约 §2.7）：origin timestamp 必须回显我方 T1，否则丢弃（防伪造与迟到包）；
 * `stratum = 0` 是 KoD，时间戳无定义、绝不参与计算；`stratum >= 16` 表示参考源自身未同步，
 * 同样不采用；长度不足、版本不符、mode 不是服务端模式一律拒绝。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dztrader::sysmon::ntp {

/// 报文长度：NTPv4 / SNTP 固定 48 字节（不含可选的扩展字段与 MAC）。
inline constexpr std::size_t kPacketSize = 48;

/// 客户端在报文里声明的协议版本（请求恒为 4；响应接受 3 或 4——报文布局相同，v3 服务器仍常见）。
inline constexpr uint8_t kClientVersion = 4;
inline constexpr uint8_t kMinServerVersion = 3;
inline constexpr uint8_t kMaxServerVersion = 4;

/// 报文的 mode 取值（RFC 5905 §7.3）：客户端 3、服务端 4。
inline constexpr uint8_t kModeClient = 3;
inline constexpr uint8_t kModeServer = 4;

/// Unix 纪元（1970-01-01）到 NTP 纪元（1900-01-01）的秒数。
inline constexpr int64_t kUnixEpochOffsetSec = 2'208'988'800;

/// NTP 时间戳：64 位（32 位秒 + 32 位分数，纪元 1900-01-01，era 回绕周期 2^32 秒）。
struct Timestamp {
    uint32_t seconds = 0;
    uint32_t fraction = 0;
};

/// 把 Unix 纪元纳秒换算为 NTP 时间戳（秒按 32 位 era 回绕；分数四舍五入到最近值）。
///
/// @param unix_ns 自 Unix 纪元起的纳秒数（要求 >= 0；本模块只处理 1970 年之后的时刻）
[[nodiscard]] Timestamp from_unix_nanoseconds(int64_t unix_ns) noexcept;

/// 把 NTP 时间戳的 32 位分数部分换算为纳秒（整数运算，四舍五入；结果钳制到 [0, 999999999]）。
[[nodiscard]] int64_t fraction_to_nanoseconds(uint32_t fraction) noexcept;

/// 把纳秒换算为 NTP 时间戳的 32 位分数部分（整数运算，四舍五入；输入钳制到 [0, 999999999]）。
[[nodiscard]] uint32_t nanoseconds_to_fraction(int64_t nanos) noexcept;

/// 把 16.16 定点秒换算为纳秒（root delay 取有符号、root dispersion
/// 取无符号，由调用方按协议位宽读出）。
[[nodiscard]] int64_t fixed_point_nanoseconds(int64_t fixed) noexcept;

/// 两个 NTP 时间戳之差 `later - earlier`（纳秒），秒字段按 32 位 era 展开。
///
/// 适用前提：|later − earlier| < 2^31 秒（约 68 年，即 2036 回绕前后各一个 era），
/// 探测往返远小于该范围。两个分数部分各自舍入到纳秒，因此结果误差 <= 2 ns。
[[nodiscard]] int64_t difference_nanoseconds(Timestamp later, Timestamp earlier) noexcept;

/// 构造 48 字节客户端请求：LI=0、VN=4、Mode=3，transmit timestamp = @p transmit，
/// 其余时间戳、参考标识与 stratum/poll/precision 全为 0（RFC 5905 客户端请求的常规形态）。
[[nodiscard]] std::array<uint8_t, kPacketSize> build_request(Timestamp transmit) noexcept;

/// 报文解析失败的原因（编排层统一归入 `bad_echo`：契约 §2.9 的五种失败原因里没有更细的取值）。
enum class ParseError {
    None,        ///< 解析成功
    TooShort,    ///< 不足 48 字节（截断包）
    BadVersion,  ///< 版本字段既不是 3 也不是 4
    BadMode,     ///< 不是服务端模式（mode 4）
};

/// 解析后的服务端响应（只保留本模块消费的字段）。
///
/// `root_delay` / `root_dispersion` 保持协议字段名（便于与抓包/契约对照），单位已换算为
/// **纳秒**：协议里它们是 16.16 定点秒（root delay 有符号、root dispersion 无符号）。
struct Response {
    uint8_t leap = 0;             ///< LI（闰秒标志），本模块不参与判定
    uint8_t version = 0;          ///< VN
    uint8_t mode = 0;             ///< Mode（解析成功时恒为 4）
    uint8_t stratum = 0;          ///< 参考源层级；0 = KoD、>= 16 = 参考源自身未同步
    uint8_t poll = 0;             ///< Poll
    int8_t precision = 0;         ///< Precision
    int64_t root_delay = 0;       ///< 根延迟（纳秒；协议为有符号 16.16 定点秒）
    int64_t root_dispersion = 0;  ///< 根色散（纳秒；协议为无符号 16.16 定点秒）
    uint32_t reference_id = 0;    ///< 参考标识；stratum = 0 时为 KoD 的四个 ASCII 字符
    Timestamp origin;             ///< origin timestamp（应为服务端回显的我方 T1）
    Timestamp receive;            ///< receive timestamp（T2）
    Timestamp transmit;           ///< transmit timestamp（T3）
};

/**
 * @brief 解析服务端响应报文。
 *
 * 只做形状与版本/模式的校验；origin 回显、KoD 与 stratum 判定在 `classify_response()`
 * 与 `kiss_code()` 里，便于编排层按失败原因分别处理（KoD 要退避/拉黑，回显不符只记失败）。
 *
 * @param packet 收到的字节（长度可大于 48：多余部分属于扩展字段，忽略）
 * @param error  失败原因输出（可为 nullptr）
 * @return 解析成功返回响应，失败返回 std::nullopt
 */
[[nodiscard]] std::optional<Response> parse_response(std::span<const uint8_t> packet,
                                                     ParseError* error = nullptr) noexcept;

/// 响应可用性判定（契约 sysmon §2.7 报文校验）。
enum class Verdict {
    Usable,          ///< 可用于计算
    BadEcho,         ///< origin 回显不等于我方 T1（伪造或迟到包）
    KissOfDeath,     ///< stratum = 0：KoD，时间戳无定义
    Unsynchronised,  ///< stratum >= 16：参考源自身未同步
};

/**
 * @brief 判定响应能否用于计算。
 *
 * 判定顺序是语义的一部分：KoD（stratum = 0）先于回显校验——KoD 包的时间戳无定义，
 * 但它携带的 KoD 代码决定退避/拉黑，必须先让编排层看到；回显不符的包在计算前丢弃。
 *
 * @param response        解析后的响应
 * @param expected_origin 我方 T1（即请求里的 transmit timestamp）
 */
[[nodiscard]] Verdict classify_response(const Response& response,
                                        Timestamp expected_origin) noexcept;

/// KoD（kiss-o'-death）代码：stratum = 0 时 reference identifier 的四个 ASCII 字符。
enum class KissCode {
    None,      ///< 不是 KoD（stratum != 0）
    Rate,      ///< RATE：对该服务器退避
    Deny,      ///< DENY：拉黑该服务器
    Restrict,  ///< RSTR：拉黑该服务器（与 DENY 同处置，契约 §2.7）
    Other,     ///< 其余 KoD：忽略（不改变退避/拉黑状态）
};

/// 读取 KoD 代码；非 KoD 返回 `KissCode::None`。
[[nodiscard]] KissCode kiss_code(const Response& response) noexcept;

/// 一次探测的四个时间戳（契约 sysmon §2.7：T1 发送前 / T2 服务端收到 / T3 服务端发出 / T4
/// 接收后）。
struct Timestamps {
    Timestamp t1;  ///< 我方发送时刻（已编码进请求的 transmit timestamp）
    Timestamp t2;  ///< 服务端收到时刻（响应的 receive timestamp）
    Timestamp t3;  ///< 服务端发出时刻（响应的 transmit timestamp）
    Timestamp t4;  ///< 我方接收时刻（收到响应的墙钟）
};

/// 一次成功探测的派生量（契约 sysmon §2.7）。
struct Sample {
    int64_t offset_ns = 0;  ///< 有符号偏差，**正 = 本地快**
    int64_t rtt_ns = 0;     ///< 往返时延（网络往返，已扣除服务端处理时间）
    int64_t bound_ns = 0;   ///< 误差上界 = ½·rtt + 服务端 root dispersion
};

/**
 * @brief 由四个时间戳、单调时钟往返与服务端 root dispersion 派生偏差/往返/误差上界。
 *
 * - 偏差：`offset = ½[(T1−T2) + (T4−T3)]`。RFC 5905 §8 的 θ = ½[(T2−T1)+(T3−T4)] 按其定义
 *   （服务端时间 − 本地时间）**正值表示本地慢**，与契约 §2.9 / GLOSSARY 的「正 = 本地快」
 *   相反，故此处取负。
 * - 往返：`rtt = 单调往返 − (T3−T2)`。墙钟测往返在 NTP 调钟期间可能为负（契约 §2.7），
 *   因此往返由调用方用单调时钟测量，这里只扣服务端处理时间。
 * - 误差上界：`bound = ½·rtt + root_dispersion`（两者同时产出，不存在只有偏差没有界的样本）。
 *
 * 偏差按整数纳秒求平均（除 2 向零截断），舍入误差 <= 1 ns，远低于亚微秒精度目标。
 *
 * @return 拒收时返回 std::nullopt：`T3 < T2`（服务端处理时间为负，时间戳自相矛盾）
 *         或 `rtt < 0`（单调往返小于服务端处理时间，样本不可信）
 */
[[nodiscard]] std::optional<Sample> derive_sample(const Timestamps& timestamps,
                                                  int64_t steady_rtt_ns,
                                                  int64_t root_dispersion_ns) noexcept;

}  // namespace dztrader::sysmon::ntp

#endif  // DZTRADER_SYSMON_NTP_CODEC_H_
