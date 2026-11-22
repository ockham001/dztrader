import { describe, it, expect } from 'vitest'
import {
  sessionStateOfProgress,
  sessionStateOfTdState,
  tdSessionState,
  canLoginTd,
  canLogoutTd,
} from '../tdAccountSession'

/**
 * 会话状态判定（ADR 0051 / 契约 progress「网关会话进度骨架」/ 契约 td-connection「前端义务」）。
 *
 * 固定「一个按钮只有一个意思」的判据：
 * - `current <= min` = 未登录（唯一可登录态）；
 * - `current >= max` = 就绪（可登出、不可登录）；
 * - 其余 = 建立中/重连中/失败回滚（可登出 = 强制中断、不可登录）。
 */
describe('sessionStateOfProgress（契约 progress 端点判定）', () => {
  it('端点到 min = 未登录，到 max = 就绪，中间 = 建立中', () => {
    expect(sessionStateOfProgress({ min: 0, max: 10, current: 0 })).toBe('absent')
    expect(sessionStateOfProgress({ min: 0, max: 10, current: 10 })).toBe('ready')
    expect(sessionStateOfProgress({ min: 0, max: 10, current: 1 })).toBe('forming')
    expect(sessionStateOfProgress({ min: 0, max: 10, current: 5 })).toBe('forming')
    expect(sessionStateOfProgress({ min: 0, max: 10, current: 2 })).toBe('forming')
  })

  it('current 越界按端点语义收敛（>=max 就绪、<=min 未登录）', () => {
    expect(sessionStateOfProgress({ min: 0, max: 10, current: 11 })).toBe('ready')
    expect(sessionStateOfProgress({ min: 0, max: 10, current: -1 })).toBe('absent')
  })

  it('非确定进度（max<=min，契约 progress）= null（无状态含义）', () => {
    expect(sessionStateOfProgress({ min: 0, max: 0, current: 0 })).toBeNull()
    expect(sessionStateOfProgress({ min: 2, max: 2, current: 2 })).toBeNull()
  })

  it('非法/缺失字段 = null（不猜状态）', () => {
    expect(sessionStateOfProgress(null)).toBeNull()
    expect(sessionStateOfProgress(undefined)).toBeNull()
    expect(sessionStateOfProgress({ min: '0', max: 10, current: 0 } as never)).toBeNull()
    expect(sessionStateOfProgress({ min: 0, max: 10 })).toBeNull()
  })
})

describe('sessionStateOfTdState（td_status.state 兜底）', () => {
  it('Idle = 未登录、Ready = 就绪、其余 = 建立中', () => {
    expect(sessionStateOfTdState('Idle')).toBe('absent')
    expect(sessionStateOfTdState('Ready')).toBe('ready')
    for (const state of ['Connecting', 'Connected', 'Authenticating', 'Authenticated',
      'LoggingIn', 'LoggedIn', 'Confirming', 'LoadingInstruments', 'Disconnected'] as const) {
      expect(sessionStateOfTdState(state)).toBe('forming')
    }
  })

  it('未知/缺失 = null（不吞掉事实、不默认就绪）', () => {
    expect(sessionStateOfTdState(null)).toBeNull()
    expect(sessionStateOfTdState(undefined)).toBeNull()
    expect(sessionStateOfTdState('')).toBeNull()
  })
})

describe('tdSessionState / canLoginTd / canLogoutTd（账户行）', () => {
  const row = (patch: { progress?: { min: number; max: number; current: number } | null; state?: string | null }) => ({
    progress: patch.progress === undefined
      ? null
      : (patch.progress === null ? null : { ...patch.progress, desc: undefined }),
    status: patch.state === undefined || patch.state === null ? null : { state: patch.state },
  })

  it('progress 优先于 td_status.state（两者冲突时按进度端点）', () => {
    // 进度到 max = 就绪；细状态仍停在中间态（帧到达顺序差异）→ 以进度为准
    expect(tdSessionState(row({ progress: { min: 0, max: 10, current: 10 }, state: 'LoggingIn' }))).toBe('ready')
  })

  it('progress 缺失时用细状态兜底', () => {
    expect(tdSessionState(row({ progress: null, state: 'Idle' }))).toBe('absent')
    expect(tdSessionState(row({ progress: null, state: 'Disconnected' }))).toBe('forming')
  })

  it('两者都缺失按未登录处理（允许尝试登录，最终由 dzweb/网关裁决）', () => {
    expect(tdSessionState(row({ progress: null, state: null }))).toBe('absent')
  })

  it('登录仅完全未登录可用；登出在任意非未登录态可用（= 强制中断）', () => {
    const absent = row({ progress: { min: 0, max: 10, current: 0 } })
    const ready = row({ progress: { min: 0, max: 10, current: 10 } })
    const forming = row({ progress: { min: 0, max: 10, current: 2 } })

    expect(canLoginTd(absent)).toBe(true)
    expect(canLogoutTd(absent)).toBe(false)

    expect(canLoginTd(forming)).toBe(false)
    expect(canLogoutTd(forming)).toBe(true)

    expect(canLoginTd(ready)).toBe(false)
    expect(canLogoutTd(ready)).toBe(true)
  })
})
