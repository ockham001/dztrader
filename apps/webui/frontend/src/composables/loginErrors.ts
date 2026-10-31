// 登录失败错误码 → 用户可读文案（单一真相源）。
//
// 契约来源：契约 rest §1/§2.1 —— 登录失败（POST /api/login）错误体为
// `{"error": <human message>, "code": <snake_case_code>}`；`error` 是人类可读消息（英文），
// `code` 是机器码，也是前端**唯一**的分支依据（不解析 `error` 文本）。
// HTTP 状态码表达类别（401 凭据/状态/封禁类，423 锁定），machine code 清单：
//   invalid_credentials / account_disabled / ip_banned → 401
//   account_locked → 423 且额外携带 `locked_until`（绝对 Unix 秒级时间戳，
//   与 GET /api/user* 的 `locked_until` 同语义；剩余分钟 = locked_until - now）。
//
// 用户管理页的登录历史 reason 与本映射同源（同一系统不得两套文案），故本模块被
// LoginView（登录页错误提示）与 UserManagementView（登录历史失败原因）共用。

export type LoginErrorCode =
  | 'invalid_credentials'
  | 'account_disabled'
  | 'account_locked'
  | 'ip_banned'

/** 机器码 → 中文文案（契约 rest §2.1） */
export const LOGIN_ERROR_TEXT: Record<LoginErrorCode, string> = {
  invalid_credentials: '用户名或密码错误',
  account_disabled: '账户已禁用',
  account_locked: '账户已锁定',
  ip_banned: 'IP 已被封禁',
}

/**
 * 登录失败机器码 → 用户可读文案；未知/非法码统一「登录失败」（绝不回显 snake_case）。
 * @param code 后端错误体的 `code` 字段（运行时不可信，故入参为 unknown）
 */
export function loginErrorText(code: unknown): string {
  if (typeof code === 'string' && Object.prototype.hasOwnProperty.call(LOGIN_ERROR_TEXT, code)) {
    return LOGIN_ERROR_TEXT[code as LoginErrorCode]
  }
  return '登录失败'
}

/**
 * 账户锁定剩余分钟（契约 rest §1/§2.1：`locked_until` 为**绝对** Unix 秒级时间戳）。
 * 向上取整（不足 1 分钟也算 1 分钟）；非法值/已过期返回 0（最小 0）。
 * @param lockedUntil 错误体中的 `locked_until`（运行时不可信，故入参为 unknown）
 * @param nowMs 当前时刻（毫秒），测试可注入固定时钟
 */
export function lockedRemainingMinutes(lockedUntil: unknown, nowMs: number = Date.now()): number {
  if (typeof lockedUntil !== 'number' || !Number.isFinite(lockedUntil)) return 0
  const remainingSec = lockedUntil - Math.floor(nowMs / 1000)
  if (remainingSec <= 0) return 0
  return Math.ceil(remainingSec / 60)
}
