#ifndef DZTRADER_PLATFORM_ACCOUNT_IDENTITY_H_
#define DZTRADER_PLATFORM_ACCOUNT_IDENTITY_H_

/**
 * @file account_identity.h
 * @brief 交易账户身份字段的平台层共享校验：`account_id` 字符集、`account_name` 硬约束
 *        （ADR 0056 决策 4/6；契约 td-config §2.2 / §7）。
 *
 * 为什么共享（单一实现）：同一组字节规则有**两个校验点**——dzweb 是平台入口的权威校验
 * （写入请求帧前拒绝，契约 td-config §5 / rest §2.9.7），td 网关在落盘前自检同一份配置
 * （契约 td-config §6 / §7「td 侧也自检」）。两处规则必须逐字节一致，否则会出现「平台放行、
 * 网关拒绝」这种只能靠手工发现的漂移；各自写一份匿名命名空间实现的必然结果就是漂移。
 *
 * **长度上限不在这里**：校验分层（ADR 0056 决策 6）——dzweb 是 31 字节（`DzAccountId`
 * 容量），CTP 是 12 字节（`TThostFtdcInvestorIDType[13]`），是各层自己知道的约束，由调用方
 * 判定。本文件只管两处共有的字节规则与唯一性比较键。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dztrader::platform {

/// `account_name` trim 后字节上限（契约 td-config §2.2 / §7）。
inline constexpr std::size_t kAccountNameMaxBytes = 64;

/// ASCII 空白口径（契约 td-config §2.2「trim 后」）。
inline constexpr std::string_view kAccountNameWhitespace = " \t\r\n\f\v";

/// 去掉首尾 ASCII 空白（不是 Unicode 空白：契约的口径就是这六个 ASCII 字符）。
///
/// @param text 原串
/// @return 去空白后的视图（指向原串，不拷贝）
[[nodiscard]] std::string_view trim_account_name(std::string_view text) noexcept;

/// `account_id` 字符集校验（契约 td-config §7 / ADR 0057）：ASCII 可见字符 `0x21`–`0x7E`，
/// 且不含 `:`、`/`、`\`、`.`。
///
/// 空串在本函数下恒为合法——"非空"是调用方的判定（两层另有各自的长度约束，ADR 0056
/// 决策 6）。`:` 被禁的理由是它曾作复合键分隔符（ADR 0056 已取消），保留禁用以避免
/// `key:account` 形态重新混进日志 / 展示层。`/`、`\`、`.` 被禁的理由：td 网关把
/// `account_id` 直接作为每账户 CTP 流文件目录名（`flow_dir_ / account_id`，契约 td-config
/// §2.3）——分隔符可穿越目录，`.` / `..` 与 Windows 尾点归一可造成目录碰撞（ADR 0057）。
/// md 侧的 `account_id` 是纯配置字段（契约 md-config），不通过本函数校验。
[[nodiscard]] bool account_id_charset_valid(std::string_view account_id) noexcept;

/// `account_name` 校验失败原因（成功 = None）。
enum class AccountNameError : std::uint8_t {
    None,         ///< 合法
    TooLong,      ///< trim 后超过 kAccountNameMaxBytes 字节
    ControlChar,  ///< 含控制字符（ASCII < 0x20 含 \t\r\n、DEL 0x7F、C1 段 U+0080–U+009F）
    InvalidUtf8,  ///< 非法 UTF-8（含内嵌 NUL）
};

/// `account_name` 校验结果（契约 td-config §2.2 / §7、ADR 0056 决策 4）。
struct AccountNameValidation {
    AccountNameError error = AccountNameError::None;
    /// 成功时的唯一性比较键 = `NFC(trim(name))`；trim 后为空 → 空串（= 未设置）。
    /// **只用于比较**：配置与回报里存原值（ADR 0056 决策 3「不做写入时规范化」）。
    std::string key;

    [[nodiscard]] bool valid() const noexcept { return error == AccountNameError::None; }
    /// 合法且 trim 后为空 = 未设置（调用方按 `account_id` 回落）。
    [[nodiscard]] bool unset() const noexcept { return valid() && key.empty(); }
};

/// 校验 `account_name` 并生成唯一性比较键：`trim` 后 ≤ 64 字节 UTF-8、无控制字符，
/// 比较前 NFC 规范化、之后字节精确比较（大小写敏感）。
///
/// 失败时 `error` 给出原因、`key` 为空——调用方负责把原因翻译成自己的错误形态
/// （dzweb：REST `invalid_account_name`；td：CRITICAL 原因串）。
///
/// @param account_name 原值（可为空串 = 未设置；不做写入时规范化）
[[nodiscard]] AccountNameValidation validate_account_name(std::string_view account_name);

/// 展平唯一性判定用的**有效身份键**（契约 td-config §2.2 / §5 / §7、ADR 0056 决策 4）：
/// 名字合法且非空 → `NFC(trim(name))`；未设置 → 回落 `account_id`。非法名（只可能来自手改
/// 配置文件的存量值）同样回落 `account_id`——与展示回落同一口径，且不会产出空键让
/// 「id vs 别人名字」这一组合漏判。
///
/// 唯一实现的理由：这个「空名按 id 参与判定」规则同时用于 td 网关的配置内查重、dzweb 的
/// REST 预检和注册表比对；三处各写一遍 ternary 必然漂移（ADR 0056 决策 4 的四组合）。
///
/// @param account_id 该账户的 ID（非空；调用方已校验）
/// @param account_name 原值（未规范化；内部 trim + NFC）
[[nodiscard]] std::string effective_account_name_key(std::string_view account_id,
                                                     std::string_view account_name);

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_ACCOUNT_IDENTITY_H_
