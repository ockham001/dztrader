import type { MarketSource, Schedule, BrokerEntry, ProcessState } from '@/types/api'

// ===== 行情源聚合视图类型（P4 Task 5 抽自 marketSources store）=====
// sources 聚合 computed 的形状 = 现状 MarketSourceView, 组件引用不变。
// 字段来源矩阵:
//   DB 基础字段 → baseSources; process_state → process 镜像;
//   loginState/brokers/selectedBrokerId/schedules/auto_login → md 镜像;
//   17 个 pending → usePending (source:{id}:{op}); expanded → 本地 UI 状态。
export type LoginState = 'offline' | 'pending' | 'online'

// ===== 登录态判定（RTN_PROGRESS 数值映射, 契约 05 消费者约定）=====
// dzmd_ctp 的 MdStateMachine 状态转移时推送 RTN_PROGRESS（md_state.cpp set_state）:
//   min=0, max=4, current: Idle=0, Connecting/Disconnected=1, Connected=2, LoggingIn=3, LoggedIn=4
// 前端登录态判定唯一实现（marketSources 聚合层与 mdConfig pending 清理共用）:
//   current >= max  = 登录完成 (online);  current <= min = 已登出 (offline);  其余 = 中间态 (pending)
// 禁止依赖 desc 文案做状态判定（desc 为展示文案, 契约 05 仅定义其为"简短文本说明"）。
// 该数值映射是跨进程稳定契约, md 端变更 current 语义必须同步本函数。
export function progressLoginState(
  p: { min?: unknown; max?: unknown; current?: unknown } | null | undefined,
): LoginState | null {
  const min = typeof p?.min === 'number' ? p.min : Number.NaN
  const max = typeof p?.max === 'number' ? p.max : Number.NaN
  const current = typeof p?.current === 'number' ? p.current : Number.NaN
  if (Number.isNaN(min) || Number.isNaN(max) || Number.isNaN(current)) return null
  if (max <= min) return null  // 不确定进度（契约 05）: 无状态含义
  if (current >= max) return 'online'
  if (current <= min) return 'offline'
  return 'pending'
}

export type ScheduleView = Schedule

export interface MarketSourceView extends MarketSource {
  loginState: LoginState
  loginPending: boolean
  logoutPending: boolean
  autoLoginPending: boolean
  scheduleAddPending: boolean
  scheduleRemovePending: boolean
  expanded: boolean
  tradingDay: string
  subscribeCount: number
  subscribeTotal: number
  brokers: BrokerEntry[]
  selectedBrokerId: string | null
  schedules: ScheduleView[]
  brokerAddPending: boolean
  brokerRemovePending: boolean
  brokerSelectPending: boolean
  brokerFieldEditPending: boolean
  frontendAddPending: boolean
  frontendRemovePending: boolean
  frontendEditPending: boolean
  frontendTogglePending: boolean
  process_state: ProcessState | null  // null = 未知/未启动
  startPending: boolean
  stopPending: boolean
  removePending: boolean
}

export function makeSource(
  partial: Partial<MarketSourceView> &
    Pick<MarketSourceView, 'id' | 'source_type' | 'source_name' | 'display_name'>
): MarketSourceView {
  return {
    is_added: true,
    auto_login: false,
    ui_card: '',
    created_at: new Date().toISOString(),
    updated_at: new Date().toISOString(),
    loginState: 'offline',
    loginPending: false,
    logoutPending: false,
    autoLoginPending: false,
    scheduleAddPending: false,
    scheduleRemovePending: false,
    expanded: false,
    tradingDay: '--',
    subscribeCount: 0,
    subscribeTotal: 0,
    brokers: [],
    selectedBrokerId: null,
    schedules: [],
    brokerAddPending: false,
    brokerRemovePending: false,
    brokerSelectPending: false,
    brokerFieldEditPending: false,
    frontendAddPending: false,
    frontendRemovePending: false,
    frontendEditPending: false,
    frontendTogglePending: false,
    process_state: null,
    startPending: false,
    stopPending: false,
    removePending: false,
    ...partial,
  }
}