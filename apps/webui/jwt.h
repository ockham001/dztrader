#ifndef DZTRADER_WEBUI_JWT_H_
#define DZTRADER_WEBUI_JWT_H_

#include <string>
#include <cstdint>

namespace dztrader::webui {

/// 签发 JWT（HS256）
/// @param sub 用户标识
/// @param ttl_sec 有效期秒数
/// @param secret HMAC 密钥
/// @param token_version 签发时刻该用户的 token_version，写入 `ver` claim。
///        校验方比对 DB 现值：不等即失效（改密 / 重置 / 禁用时自增，契约 rest §1「会话吊销」）
/// @return JWT 字符串 "header.payload.signature"
std::string jwt_sign(const std::string& sub,
                     uint32_t ttl_sec,
                     const std::string& secret,
                     int64_t token_version = 0);

/// 校验 JWT（签名 + exp）；成功时输出 `sub` 与 `ver`
/// @param token JWT 字符串
/// @param secret HMAC 密钥
/// @param out_sub 输出用户标识（校验成功时填充）
/// @param out_token_version 输出 `ver` claim；缺失按 0（兼容旧 token，见 provider 侧比对逻辑）
/// @return true=校验成功，false=签名错误/过期/格式错误
bool jwt_verify(const std::string& token,
                const std::string& secret,
                std::string& out_sub,
                int64_t* out_token_version = nullptr);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_JWT_H_
