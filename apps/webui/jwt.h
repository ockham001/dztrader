#ifndef DZTRADER_WEBUI_JWT_H_
#define DZTRADER_WEBUI_JWT_H_

#include <string>
#include <cstdint>

namespace dztrader::webui {

/// 签发 JWT（HS256）
/// @param sub 用户标识
/// @param ttl_sec 有效期秒数
/// @param secret HMAC 密钥
/// @return JWT 字符串 "header.payload.signature"
std::string jwt_sign(const std::string& sub, uint32_t ttl_sec, const std::string& secret);

/// 校验 JWT
/// @param token JWT 字符串
/// @param secret HMAC 密钥
/// @param out_sub 输出用户标识（校验成功时填充）
/// @return true=校验成功，false=签名错误/过期/格式错误
bool jwt_verify(const std::string& token, const std::string& secret, std::string& out_sub);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_JWT_H_
