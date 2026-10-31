#ifndef DZTRADER_WEBUI_SESSION_GUARD_H_
#define DZTRADER_WEBUI_SESSION_GUARD_H_

#include <cstdint>
#include <string>

#include "repository.h"

namespace dztrader::webui {

/// 会话有效性判定结果（契约 rest §1「会话吊销」/「强制改密」）。
enum class SessionState {
    Valid,         ///< 可正常使用
    InvalidToken,  ///< 用户不存在 / 已禁用 / token_version 不匹配 → 401（WS: auth failed）
    PasswordChangeRequired,  ///< must_change_password=1 → 仅允许改密与 /api/user/me
};

/// 判定某 token 对应的会话是否仍有效。
///
/// JWT 无状态，签名与 exp 只能证明「签发时有效」；改密 / 管理员重置 / 禁用后的即时失效
/// 依赖 `ver` claim 与 DB `users.token_version` 的比对（契约 rest §1「会话吊销」）。
/// 调用方必须先经 `jwt_verify` 验签（本函数不做签名校验，只做 DB 侧状态比对）。
///
/// @param repo 用户数据源
/// @param username JWT 的 `sub`
/// @param token_version JWT 的 `ver` claim
/// @return 会话状态；任何 DB 异常按 InvalidToken 处理（保守拒绝，不 500）
SessionState check_session(Repository& repo, const std::string& username, int64_t token_version);

/// `must_change_password=1` 账户在改密前仍可访问的 REST 路径白名单
/// （契约 rest §1「强制改密」）。其余 `/api/*` 一律 403 `password_change_required`。
bool is_path_allowed_before_password_change(const std::string& path);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_SESSION_GUARD_H_
