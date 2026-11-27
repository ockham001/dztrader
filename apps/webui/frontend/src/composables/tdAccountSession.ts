/**
 * 交易账户会话状态判定（契约 progress「网关会话进度骨架」/ 契约 td-connection「前端义务」/ ADR 0051）。
 *
 * 统一规则（md/td 相同）：
 * - 登录按钮仅在 `current <= min`（完全未登录）可用；
 * - 登出按钮在 `current > min` 的任意状态可用（连接中/登录中/重连中/失败态/就绪）= 强制中断；
 * - 判定只看进度端点，`desc` 只作展示、`td_status.state` 只在进度未到达时兜底。
 */
import type { TdState } from '@/types/api'
import type { ProgressView } from '@/stores/progress'
import { sessionStateOfProgress, type SessionState } from '@/composables/sessionProgress'

// 端点判定与 md 共用一份实现（composables/sessionProgress.ts）；本文件只保留交易侧差异：
// `td_status.state` 兜底与账户行推导。sessionStateOfProgress 重新导出，保持调用方稳定。
export { sessionStateOfProgress } from '@/composables/sessionProgress'

/** 会话状态：未登录 / 建立中（含重连中、失败停在失败阶段）/ 就绪 */
export type TdSessionState = SessionState

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

/** 交易登录链的四个失败态（ADR 0059 决策 1：失败停在失败阶段，不回退到上一格） */
const TD_FAILED_STATES: readonly string[] = [
  'AuthFailed',
  'LoginFailed',
  'SettlementConfirmFailed',
  'InstrumentsLoadFailed',
]

/** 细状态是否属于四个登录链失败态（ADR 0059 决策 1；`null`/未到达/未知值都返回 false） */
export function isTdFailureState(state: TdState | string | null | undefined): boolean {
  return state !== null && state !== undefined && TD_FAILED_STATES.includes(state)
}

/**
 * 是否处于「失败」态（#509 状态列）：`last_error` 非空 **或** 细状态是四个失败态之一。
 *
 * 为什么不从 `current/max` 推：失败态停在 3/5/7/8（ADR 0059 决策 1），数值映射只会说
 * 「进行中」；失败结论必须来自状态帧（`state`）与 `last_error`。判定放这里而不是组件里：
 * 交易账户页与仪表盘资金面板共用同一结论（与 canLoginTd / canLogoutTd 同处）。
 *
 * 覆盖 `connect_timeout`：它没有失败态（账户回到 `Idle`，ADR 0059 决策 7），但 `last_error`
 * 保留了这次失败——状态列显示「失败」+「上次失败：连接超时」，不说成「登录失败」。
 */
export function isTdAccountFailed(row: {
  lastError: object | null
  status: { state?: TdState | string } | null
}): boolean {
  if (row.lastError !== null && row.lastError !== undefined) return true
  return isTdFailureState(row.status?.state)
}

/** 登录按钮可用：仅完全未登录（这是「一个按钮只有一个意思」的落点） */
export function canLoginTd(row: Parameters<typeof tdSessionState>[0]): boolean {
  return tdSessionState(row) === 'absent'
}

/** 登出按钮可用：任意非未登录状态（= 取消/强制中断） */
export function canLogoutTd(row: Parameters<typeof tdSessionState>[0]): boolean {
  return tdSessionState(row) !== 'absent'
}
