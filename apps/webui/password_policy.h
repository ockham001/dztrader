#ifndef DZTRADER_WEBUI_PASSWORD_POLICY_H_
#define DZTRADER_WEBUI_PASSWORD_POLICY_H_

#include <cstddef>
#include <string>

namespace dztrader::webui {

/// 口令最小长度（契约 rest §1「密码策略」的唯一常量）。
///
/// 由服务端强制：`POST /api/user`、`PUT /api/user/{id}/password`、
/// `POST /api/auth/change-password` 三条设置口令的路径全部经 `is_password_acceptable`。
/// 前端提示同值（`composables/passwordPolicy.ts`），但前端**不是安全边界**。
constexpr std::size_t kMinPasswordLength = 12;

/// 口令是否满足强度策略。
///
/// 当前策略只有最小长度（维护者 2026-10-31 决策）：复杂度规则（大小写/数字/符号）
/// 对单人运维系统的收益低于其对口令管理的摩擦，且长度是对抗离线爆破的主要因子。
/// @param password 候选口令（按 UTF-8 字节计长度；与 hash_password 的输入口径一致）
/// @return true=通过
bool is_password_acceptable(const std::string& password);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_PASSWORD_POLICY_H_
