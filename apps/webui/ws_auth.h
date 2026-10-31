#ifndef DZTRADER_WEBUI_WS_AUTH_H_
#define DZTRADER_WEBUI_WS_AUTH_H_

#include <optional>
#include <string>

#include "jwt.h"

namespace dztrader::webui {

/// `/ws` 连接鉴权（契约 webui-ws §1）：token 提取（query `token` 优先，
/// 其次 `Authorization: Bearer <jwt>`）并校验 JWT 签名/有效期。
/// 从 handleNewConnection 提取为纯函数以便单测（WsController 依赖 drogon
/// 连接对象无法脱离框架实例化）。返回 nullopt = 鉴权失败。
///
/// **不含**会话吊销与强制改密判定：那两步需要 DB 与 Repository，由调用方在
/// 拿到 username 后经 `session_guard.h` 的 `check_session` 完成（同一函数也被 REST 预检使用）。
inline std::optional<std::string> ws_authenticate(const std::string& query_token,
                                                  const std::string& authorization_header,
                                                  const std::string& jwt_secret,
                                                  int64_t* out_token_version = nullptr) {
    std::string token = query_token;
    if (token.empty() && authorization_header.size() >= 7 &&
        authorization_header.starts_with("Bearer ")) {
        token = authorization_header.substr(7);
    }
    std::string user_id;
    if (!jwt_verify(token, jwt_secret, user_id, out_token_version)) {
        return std::nullopt;
    }
    return user_id;
}

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_WS_AUTH_H_
