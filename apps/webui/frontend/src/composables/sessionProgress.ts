/**
 * 网关会话进度判定的**唯一实现**（ADR 0051 / 契约 progress「网关会话进度骨架」）。
 *
 * 端点语义跨接口类型固定：`current <= min` = 未登录、`current >= max` = 就绪、
 * 中间 = 建立中/重连中/失败回滚；数值不固定（行情网关 max=4、交易网关 max=10）。
 * 行情侧 `progressLoginState`、交易侧 `tdSessionState` 与两个 store 的「回退清登录 pending」
 * 都委托到这里——同一事实只允许一处定义（#409 审查 B1）。
 */
import type { ProgressView } from '@/stores/progress'

/** 会话状态：未登录 / 建立中（含重连中、失败回滚）/ 就绪 */
export type SessionState = 'absent' | 'forming' | 'ready'

export interface ProgressLike {
  min?: unknown
  max?: unknown
  current?: unknown
}

/** 确定进度判定（契约 progress）：`max > min`，非数值/不确定进度返回 false。 */
export function isDeterminateSessionProgress(
  p: ProgressLike | null | undefined,
): boolean {
  return typeof p?.min === 'number' && typeof p?.max === 'number'
    && !Number.isNaN(p.min) && !Number.isNaN(p.max) && p.max > p.min
}

/** 进度端点判定；非确定进度（`max <= min`）或字段缺失返回 null（契约 progress：无状态含义）。 */
export function sessionStateOfProgress(
  progress: ProgressLike | ProgressView | null | undefined,
): SessionState | null {
  if (progress === null || progress === undefined) return null
  if (!isDeterminateSessionProgress(progress)) return null
  const min = progress.min as number
  const max = progress.max as number
  const current = typeof progress.current === 'number' ? progress.current : Number.NaN
  if (Number.isNaN(current)) return null
  if (current >= max) return 'ready'
  if (current <= min) return 'absent'
  return 'forming'
}

/** 一次进度变化对在途会话请求的建议清除范围（所有网关同规则）。 */
export type SessionPendingClear = 'none' | 'login' | 'both'

/**
 * 一次进度变化应清除哪些 pending（契约 td-connection 前端义务 / 契约 md-status 义务①）：
 * - 到达 `ready`：一次登录尝试成功 → 清 login；
 * - 到达 `absent`：回到未登录 → 清 login + logout（登录在途时点登出也走这里）；
 * - 中间态且 `current` 相对上次回退：一次登录尝试失败/中止（失败态不是端点）→ 清 login。
 * 除此之外不清：端点未到达、无回退的中间态都表示尝试仍在进行。
 */
export function sessionPendingClear(
  previousCurrent: number | undefined,
  state: SessionState,
  current: number,
): SessionPendingClear {
  if (state === 'ready') return 'login'
  if (state === 'absent') return 'both'
  if (previousCurrent !== undefined && current < previousCurrent) return 'login'
  return 'none'
}
