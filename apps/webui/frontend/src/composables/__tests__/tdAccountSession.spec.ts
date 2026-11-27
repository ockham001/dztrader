import { describe, it, expect } from 'vitest'
import {
  sessionStateOfProgress,
  sessionStateOfTdState,
  tdSessionState,
  canLoginTd,
  canLogoutTd,
  isTdFailureState,
  tdAccountStatusView,
} from '../tdAccountSession'
import { sessionPendingClear } from '../sessionProgress'

/**
 * 会话状态判定（ADR 0051 / 契约 progress「网关会话进度骨架」/ 契约 td-connection「前端义务」）。
 *
 * 固定「一个按钮只有一个意思」的判据：
 * - `current <= min` = 未登录（唯一可登录态）；
 * - `current >= max` = 就绪（可登出、不可登录）；
 * - 其余 = 建立中/重连中/失败停在失败阶段（可登出 = 强制中断、不可登录）。
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
    for (const state of ['Connecting', 'Connected', 'Authenticating', 'AuthFailed',
      'Authenticated', 'LoggingIn', 'LoginFailed', 'LoggedIn', 'Confirming',
      'SettlementConfirmFailed', 'LoadingInstruments', 'InstrumentsLoadFailed',
      'Disconnected'] as const) {
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

describe('isTdFailureState / tdAccountStatusView（#509 状态列）', () => {
  it('四个登录链失败态为 true；正常态/中间态/未知/缺失为 false', () => {
    for (const state of ['AuthFailed', 'LoginFailed', 'SettlementConfirmFailed',
      'InstrumentsLoadFailed'] as const) {
      expect(isTdFailureState(state)).toBe(true)
    }
    for (const state of ['Idle', 'Connecting', 'Authenticating', 'LoggingIn', 'LoggedIn',
      'Ready', 'Disconnected', 'UnknownFutureState'] as const) {
      expect(isTdFailureState(state)).toBe(false)
    }
    expect(isTdFailureState(null)).toBe(false)
    expect(isTdFailureState(undefined)).toBe(false)
  })

  const statusRow = (patch: {
    progress?: { min: number; max: number; current: number; desc?: string } | null
    state?: string | null
    gatewayRunning?: boolean
  }) => ({
    progress: patch.progress ?? null,
    status: patch.state === undefined || patch.state === null ? null : { state: patch.state },
    gatewayRunning: patch.gatewayRunning ?? true,
    isFailed: isTdFailureState(patch.state),
  })

  it('网关进程非 Running：一律「未启动」，残留进度/失败态都不许冒充会话状态', () => {
    const stale = tdAccountStatusView(statusRow({
      progress: { min: 0, max: 10, current: 10 },
      state: 'LoginFailed',
      gatewayRunning: false,
    }))
    expect(stale).toEqual({ current: 0, max: 1, min: 0, failed: false, idleText: '未启动' })
  })

  it('从未收到进度镜像 = 「未启动」；有进度且停在端点 = 「未登录」', () => {
    expect(tdAccountStatusView(statusRow({ state: null })).idleText).toBe('未启动')
    const idle = tdAccountStatusView(statusRow({ progress: { min: 0, max: 10, current: 0 } }))
    expect(idle.idleText).toBe('未登录')
    expect(idle.failed).toBe(false)
  })

  it('desc 只在会话建立中传（端点态徽章要说「已登录/未登录」，不被「就绪」顶掉）', () => {
    const forming = tdAccountStatusView(statusRow({
      progress: { min: 0, max: 10, current: 5, desc: '用户登录...' },
    }))
    expect(forming.desc).toBe('用户登录...')
    const ready = tdAccountStatusView(statusRow({
      progress: { min: 0, max: 10, current: 10, desc: '就绪' },
    }))
    expect(ready.desc).toBeUndefined()
  })

  it('失败结论取行上的 isFailed（四个失败态）；connect_timeout 这类 last_error 不算失败', () => {
    expect(tdAccountStatusView(statusRow({
      progress: { min: 0, max: 10, current: 3, desc: '认证失败' },
      state: 'AuthFailed',
    })).failed).toBe(true)
    // connect_timeout：state 回 Idle + last_error 保留 → 不是失败态（契约 td-status §2.2）
    expect(tdAccountStatusView(statusRow({
      progress: { min: 0, max: 10, current: 0, desc: '未启动' },
      state: 'Idle',
    })).failed).toBe(false)
  })
})

describe('sessionPendingClear（跨网关共用：一次进度变化清哪些 pending）', () => {
  it('到 ready 清 login；到 absent 清 login + logout', () => {
    expect(sessionPendingClear(9, 'ready', 10)).toBe('login')
    expect(sessionPendingClear(undefined, 'ready', 10)).toBe('login')
    expect(sessionPendingClear(2, 'absent', 0)).toBe('both')
    expect(sessionPendingClear(undefined, 'absent', 0)).toBe('both')
  })

  it('中间态且 current 回退 = 一次尝试失败/中止 → 清 login；无回退不清', () => {
    expect(sessionPendingClear(5, 'forming', 2)).toBe('login')
    expect(sessionPendingClear(2, 'forming', 2)).toBe('none')
    expect(sessionPendingClear(2, 'forming', 5)).toBe('none')
    expect(sessionPendingClear(undefined, 'forming', 3)).toBe('none')
  })
})
