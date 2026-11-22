/**
 * 交易账户会话状态判定（契约 progress「网关会话进度骨架」/ 契约 td-connection「前端义务」/ ADR 0051）。
 *
 * 统一规则（md/td 相同）：
 * - 登录按钮仅在 `current <= min`（完全未登录）可用；
 * - 登出按钮在 `current > min` 的任意状态可用（连接中/登录中/重连中/失败回滚/就绪）= 强制中断；
 * - 判定只看进度端点，`desc` 只作展示、`td_status.state` 只在进度未到达时兜底。
 */
import type { TdState } from '@/types/api'
import type { ProgressView } from '@/stores/progress'

/** 会话状态：未登录 / 建立中（含重连中、失败回滚）/ 就绪 */
export type TdSessionState = 'absent' | 'forming' | 'ready'

export interface TdProgressLike {
  min?: unknown
  max?: unknown
  current?: unknown
}

/** 进度端点判定；非确定进度（`max <= min`）返回 null（契约 progress：无状态含义） */
export function sessionStateOfProgress(
  progress: TdProgressLike | null | undefined,
): TdSessionState | null {
  const min = typeof progress?.min === 'number' ? progress.min : Number.NaN
  const max = typeof progress?.max === 'number' ? progress.max : Number.NaN
  const current = typeof progress?.current === 'number' ? progress.current : Number.NaN
  if (Number.isNaN(min) || Number.isNaN(max) || Number.isNaN(current)) return null
  if (!(max > min)) return null
  if (current >= max) return 'ready'
  if (current <= min) return 'absent'
  return 'forming'
}

/** 细状态兜底（进度镜像未到达时）：Idle = 未登录、Ready = 就绪、其余 = 建立中 */
export function sessionStateOfTdState(
  state: TdState | string | null | undefined,
): TdSessionState | null {
  if (state === null || state === undefined || state === '') return null
  if (state === 'Idle') return 'absent'
  if (state === 'Ready') return 'ready'
  return 'forming'
}

/** 账户行 → 会话状态（进度优先、细状态兜底；都没有时按未登录处理，允许尝试登录） */
export function tdSessionState(row: {
  progress: ProgressView | null
  status: { state?: TdState | string } | null
}): TdSessionState {
  return (
    sessionStateOfProgress(row.progress)
    ?? sessionStateOfTdState(row.status?.state)
    ?? 'absent'
  )
}

/** 登录按钮可用：仅完全未登录（这是「一个按钮只有一个意思」的落点） */
export function canLoginTd(row: Parameters<typeof tdSessionState>[0]): boolean {
  return tdSessionState(row) === 'absent'
}

/** 登出按钮可用：任意非未登录状态（= 取消/强制中断） */
export function canLogoutTd(row: Parameters<typeof tdSessionState>[0]): boolean {
  return tdSessionState(row) !== 'absent'
}
