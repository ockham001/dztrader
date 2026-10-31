// 口令强度策略（前端侧的**提示**用，不是安全边界）。
//
// 契约来源：契约 rest §1「密码策略」——最小长度由**服务端**强制
// （`apps/webui/password_policy.h` 的 `kMinPasswordLength`），不合格后端回
// 400 `{"error":"password too weak","code":"password_too_weak"}`。
// 本模块只用于表单即时提示与按钮禁用，绕过它（curl/改 JS）也只能换到后端的同一个 400。
//
// 两处常量必须同值：改这里要同时改 `apps/webui/password_policy.h`
// （`scripts/preflight.sh` 的日志/格式门禁不覆盖此项，靠单测与审查保证）。

/** 最小口令长度（与后端 `kMinPasswordLength` 同值，契约 rest §1） */
export const MIN_PASSWORD_LENGTH = 12

/** 口令是否满足强度策略（前端提示用；服务端仍会独立校验） */
export function isPasswordAcceptable(password: string): boolean {
  return password.length >= MIN_PASSWORD_LENGTH
}

/**
 * 口令长度提示文案的**唯一真相源**（错误提示与字段下方常驻提示共用同一句，
 * 避免模板里各处手写中文）。
 */
export const PASSWORD_LENGTH_TEXT = `密码长度至少 ${MIN_PASSWORD_LENGTH} 位`

/** 两次输入不一致的提示文案（改密类表单共用） */
export const PASSWORD_MISMATCH_TEXT = '两次输入的密码不一致'

/** 旧密码错误的提示文案（契约 rest §1：code=invalid_old_password） */
export const OLD_PASSWORD_INVALID_TEXT = '旧密码不正确'
