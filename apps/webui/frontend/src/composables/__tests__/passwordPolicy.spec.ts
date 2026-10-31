// 口令强度策略（前端提示层）。契约 rest §1「密码策略」：最小长度由服务端强制，
// 本模块只做即时提示与按钮禁用。**前端常量必须与后端 kMinPasswordLength 同值**
// （apps/webui/password_policy.h），否则用户会被前端拦住但后端其实收得下（或反之）。
import { describe, it, expect } from 'vitest'
import {
  isPasswordAcceptable,
  MIN_PASSWORD_LENGTH,
  OLD_PASSWORD_INVALID_TEXT,
  PASSWORD_MISMATCH_TEXT,
  PASSWORD_LENGTH_TEXT,
} from '../passwordPolicy'

describe('passwordPolicy', () => {
  it('最小长度与后端 kMinPasswordLength 同值（契约 rest §1 固定为 12）', () => {
    expect(MIN_PASSWORD_LENGTH).toBe(12)
  })

  it('恰好等于最小长度时通过', () => {
    expect(isPasswordAcceptable('a'.repeat(MIN_PASSWORD_LENGTH))).toBe(true)
  })

  it('短于最小长度时拒绝（含系统默认口令）', () => {
    expect(isPasswordAcceptable('a'.repeat(MIN_PASSWORD_LENGTH - 1))).toBe(false)
    expect(isPasswordAcceptable('88888888')).toBe(false)
    expect(isPasswordAcceptable('')).toBe(false)
  })

  it('纯长度策略：不强制复杂度（与后端一致）', () => {
    expect(isPasswordAcceptable('aaaaaaaaaaaa')).toBe(true)
  })

  it('提示文案含具体长度且不回显口令值', () => {
    expect(PASSWORD_LENGTH_TEXT).toContain(String(MIN_PASSWORD_LENGTH))
    // 三个文案都是固定中文提示，不拼接用户输入
    expect(PASSWORD_MISMATCH_TEXT).toBe('两次输入的密码不一致')
    expect(OLD_PASSWORD_INVALID_TEXT).toBe('旧密码不正确')
  })
})
