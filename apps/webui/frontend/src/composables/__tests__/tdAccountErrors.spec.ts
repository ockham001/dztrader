import { describe, it, expect } from 'vitest'
import {
  TD_ACCOUNT_ERROR_TEXT,
  TD_ACCOUNT_ERROR_FALLBACK,
  TD_ACCOUNT_NETWORK_ERROR_TEXT,
  tdAccountErrorCodeOf,
  tdAccountErrorStatusOf,
  tdAccountErrorText,
  tdAccountServerErrorText,
} from '../tdAccountErrors'

/**
 * 交易账户写端点失败码 → 中文文案（#251；契约 rest §2.9.7）。
 *
 * 口径与 loginErrors 一致：`code` 是唯一分支依据，`error`（英文人类可读消息）不回显。
 * 这里固定三件事：五码全映射、码间文案互不重复、未知码/无码回落且绝不回显 snake_case。
 */
describe('tdAccountErrors（契约 rest §2.9.7 预检 code 全集）', () => {
  it('五个预检码全部有中文文案，且文案互不相同', () => {
    const codes = ['admin_required', 'invalid_gateway', 'gateway_not_running', 'account_exists', 'account_not_found'] as const
    const texts = codes.map(code => TD_ACCOUNT_ERROR_TEXT[code])
    for (const [index, text] of texts.entries()) {
      expect(text, `${codes[index]} 缺文案`).toBeTruthy()
      expect(text).not.toBe(codes[index])  // 不能把机器码原样当文案
    }
    expect(new Set(texts).size).toBe(codes.length)
  })

  it('按码取文案：account_exists 明确说明「已被占用」（用户才知道该换 ID）', () => {
    expect(tdAccountErrorText('account_exists')).toContain('已被占用')
    expect(tdAccountErrorText('account_exists')).toContain('账户 ID')
    expect(tdAccountErrorText('account_not_found')).toContain('不存在')
    expect(tdAccountErrorText('gateway_not_running')).toContain('未运行')
    expect(tdAccountErrorText('invalid_gateway')).toContain('网关')
    expect(tdAccountErrorText('admin_required')).toContain('管理员')
  })

  it('未知码 / 非字符串 / 缺失 → 兜底文案，绝不回显 code 本身', () => {
    expect(tdAccountErrorText('some_new_code')).toBe(TD_ACCOUNT_ERROR_FALLBACK)
    expect(tdAccountErrorText(undefined)).toBe(TD_ACCOUNT_ERROR_FALLBACK)
    expect(tdAccountErrorText(null)).toBe(TD_ACCOUNT_ERROR_FALLBACK)
    expect(tdAccountErrorText(42)).toBe(TD_ACCOUNT_ERROR_FALLBACK)
    // 兜底文案里不得出现 snake_case 机器码（会泄漏内部协议到 UI）
    expect(TD_ACCOUNT_ERROR_FALLBACK).not.toMatch(/[a-z]+_[a-z]+/)
    expect(tdAccountErrorText('some_new_code')).not.toContain('some_new_code')
  })

  it('不继承原型链上的键（防 __proto__ / toString 之类被当成合法码）', () => {
    expect(tdAccountErrorText('toString')).toBe(TD_ACCOUNT_ERROR_FALLBACK)
    expect(tdAccountErrorText('__proto__')).toBe(TD_ACCOUNT_ERROR_FALLBACK)
    expect(tdAccountErrorText('constructor')).toBe(TD_ACCOUNT_ERROR_FALLBACK)
  })

  it('网络错误文案独立于 API 兜底（HTTP 层失败没有 code，不能冒充业务失败）', () => {
    expect(TD_ACCOUNT_NETWORK_ERROR_TEXT).not.toBe(TD_ACCOUNT_ERROR_FALLBACK)
    expect(TD_ACCOUNT_NETWORK_ERROR_TEXT).toContain('无法连接')
  })
})

describe('tdAccountErrorCodeOf（从 ApiError 形状取机器码）', () => {
  it('取 body.code；非字符串忽略', () => {
    expect(tdAccountErrorCodeOf({ body: { error: 'x', code: 'account_exists' } })).toBe('account_exists')
    expect(tdAccountErrorCodeOf({ body: { code: 123 } })).toBeUndefined()
    expect(tdAccountErrorCodeOf(new Error('network'))).toBeUndefined()
    expect(tdAccountErrorCodeOf(null)).toBeUndefined()
    expect(tdAccountErrorCodeOf(undefined)).toBeUndefined()
  })
})

describe('tdAccountErrorStatusOf（区分「服务端拒绝」与「没连上」）', () => {
  it('取 ApiError.status；非数字/缺失返回 undefined', () => {
    expect(tdAccountErrorStatusOf({ status: 503 })).toBe(503)
    expect(tdAccountErrorStatusOf({ status: undefined })).toBeUndefined()
    expect(tdAccountErrorStatusOf(new Error('Failed to fetch'))).toBeUndefined()
    expect(tdAccountErrorStatusOf(null)).toBeUndefined()
  })
})

describe('tdAccountServerErrorText（有响应但不带 code 的基础设施失败）', () => {
  it('503（事件通道未就绪）→ 基础设施文案，不套任何预检码文案', () => {
    const text = tdAccountServerErrorText(503)
    expect(text).toContain('暂不可用')
    // 关键：不能说成"账户已被占用"/"网关未运行"（用户会去改错地方）
    expect(text).not.toBe(TD_ACCOUNT_ERROR_TEXT.account_exists)
    expect(text).not.toBe(TD_ACCOUNT_ERROR_TEXT.gateway_not_running)
    // 也不能说成"无法连接服务器"（服务端明明回应了）
    expect(text).not.toBe(TD_ACCOUNT_NETWORK_ERROR_TEXT)
  })

  it('403 回落管理员文案；未知状态码回落通用兜底', () => {
    expect(tdAccountServerErrorText(403)).toBe(TD_ACCOUNT_ERROR_TEXT.admin_required)
    expect(tdAccountServerErrorText(418)).toBe(TD_ACCOUNT_ERROR_FALLBACK)
    expect(tdAccountServerErrorText(undefined)).toBe(TD_ACCOUNT_ERROR_FALLBACK)
  })
})
