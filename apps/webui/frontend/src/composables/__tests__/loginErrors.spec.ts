import { describe, it, expect } from 'vitest'
import { LOGIN_ERROR_TEXT, loginErrorText, lockedRemainingMinutes } from '../loginErrors'

// 契约来源：《帧契约：REST》§1/§2.1 —— 登录失败错误体 {"error": <code>}（无 code 字段）；
// 423 account_locked 附 locked_until（绝对 Unix 秒级时间戳，按 locked_until - now 计算剩余分钟）。
describe('loginErrors', () => {
  it('四个机器码映射为中文文案（契约 rest §2.1）', () => {
    expect(LOGIN_ERROR_TEXT).toEqual({
      invalid_credentials: '用户名或密码错误',
      account_disabled: '账户已禁用',
      account_locked: '账户已锁定',
      ip_banned: 'IP 已被封禁',
    })
  })

  it('loginErrorText: 已知码返回中文文案', () => {
    expect(loginErrorText('invalid_credentials')).toBe('用户名或密码错误')
    expect(loginErrorText('account_disabled')).toBe('账户已禁用')
    expect(loginErrorText('account_locked')).toBe('账户已锁定')
    expect(loginErrorText('ip_banned')).toBe('IP 已被封禁')
  })

  it('loginErrorText: 未知/非法码统一《登录失败》，不得回显 snake_case', () => {
    expect(loginErrorText('network_error')).toBe('登录失败')
    expect(loginErrorText('some_unknown_code')).toBe('登录失败')
    expect(loginErrorText(undefined)).toBe('登录失败')
    expect(loginErrorText(null)).toBe('登录失败')
    expect(loginErrorText(401)).toBe('登录失败')
  })

  describe('lockedRemainingMinutes（locked_until 为绝对 Unix 秒）', () => {
    const nowMs = 1_700_000_000_000 // 固定基准时刻，避免依赖当前时钟

    it('未来时刻按剩余秒向上取整为分钟', () => {
      expect(lockedRemainingMinutes(1_700_000_300, nowMs)).toBe(5)
      expect(lockedRemainingMinutes(1_700_000_061, nowMs)).toBe(2) // 61s → 向上取整 2 分钟
      expect(lockedRemainingMinutes(1_700_000_001, nowMs)).toBe(1)
    })

    it('已过期或恰好当前时刻 → 0（最小 0）', () => {
      expect(lockedRemainingMinutes(1_699_999_999, nowMs)).toBe(0)
      expect(lockedRemainingMinutes(1_700_000_000, nowMs)).toBe(0)
    })

    it('非法值（非数字/NaN/Infinity/字符串）→ 0', () => {
      expect(lockedRemainingMinutes(undefined, nowMs)).toBe(0)
      expect(lockedRemainingMinutes(null, nowMs)).toBe(0)
      expect(lockedRemainingMinutes('1700000300', nowMs)).toBe(0)
      expect(lockedRemainingMinutes(Number.NaN, nowMs)).toBe(0)
      expect(lockedRemainingMinutes(Number.POSITIVE_INFINITY, nowMs)).toBe(0)
    })
  })
})
