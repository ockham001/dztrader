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

/**
 * 细状态是否属于四个登录链失败态（ADR 0059 决策 1；`null`/未到达/未知值都返回 false）。
 *
 * 失败结论**只**从这里来，不看 `last_error`：`connect_timeout` 也写 `last_error`，但它没有
 * 失败态、账户回 `Idle`（ADR 0059 决策 7 / 契约 td-status §2.2「可观察后果」：前端看到的是
 * 「未启动 / 进度 0」+「上次失败：连接超时」）；登出后 `last_error` 也不清空（契约 §2.5），
 * 用 `last_error` 当失败判据会把「未登录」永久说成「失败」。
 */
export function isTdFailureState(state: TdState | string | null | undefined): boolean {
  return state !== null && state !== undefined && TD_FAILED_STATES.includes(state)
}

/** 状态列渲染数据（StatusIndicator 直连，#509） */
export interface TdAccountStatusView {
  current: number
  max: number
  min: number
  /** 仅会话建立中传（阶段文案）；端点态由徽章说端点词 */
  desc?: string
  /** 失败结论 = 行上的 `isFailed`（store 里由 {@link isTdFailureState} 算一次） */
  failed: boolean
  /** idle 徽章文案：有进度 = 未登录；无进度 / 网关未跑 = 未启动 */
  idleText: string
}

/** {@link tdAccountStatusView} 需要的行字段（结构类型，不绑定 store 具体类型） */
export interface TdAccountStatusRow {
  progress: ProgressView | null
  status: { state?: TdState | string } | null
  /** 网关进程是否 Running（进程镜像；进程没跑就不可能有可用会话） */
  gatewayRunning: boolean
  isFailed: boolean
}

/**
 * 状态列渲染数据：**交易账户页与仪表盘资金面板的唯一推导**（#509）。两边写各一份时，
 * 「什么时候算失败」「desc 什么时候传」这类判定必然漂移，同一账户会在两处说两种话。
 *
 * 三条规则（契约 td-status / progress）：
 * - 网关进程非 Running：一律「未启动」——进程没跑不可能有可用会话，残留的进度镜像
 *   不许冒充「已登录」，失败徽章也无从谈起（失败是会话结论）；
 * - 从未收到进度镜像：也是「未启动」（没有数据不等于「未登录」）；有进度但停在端点才是未登录；
 * - `desc` 只在会话建立中传：端点态的徽章要说「已登录/未登录」，而 td 的端点 desc 是
 *   「就绪」/「未启动」——组件里 desc 优先，传进去会把端点词顶掉。
 */
export function tdAccountStatusView(row: TdAccountStatusRow): TdAccountStatusView {
  if (!row.gatewayRunning) {
    return { current: 0, max: 1, min: 0, failed: false, idleText: '未启动' }
  }
  return {
    current: row.progress?.current ?? 0,
    max: row.progress?.max ?? 1,
    min: row.progress?.min ?? 0,
    desc: tdSessionState(row) === 'forming' ? row.progress?.desc : undefined,
    failed: row.isFailed,
    idleText: row.progress ? '未登录' : '未启动',
  }
}

/** 登录按钮可用：仅完全未登录（这是「一个按钮只有一个意思」的落点） */
export function canLoginTd(row: Parameters<typeof tdSessionState>[0]): boolean {
  return tdSessionState(row) === 'absent'
}

/** 登出按钮可用：任意非未登录状态（= 取消/强制中断） */
export function canLogoutTd(row: Parameters<typeof tdSessionState>[0]): boolean {
  return tdSessionState(row) !== 'absent'
}
