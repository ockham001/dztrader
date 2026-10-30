import type { MarketSource, Schedule, BrokerEntry, ProcessState } from '@/types/api'

// ===== 行情源聚合视图类型（P4 Task 5 抽自 marketSources store）=====
// sources 聚合 computed 的形状 = 现状 MarketSourceView, 组件引用不变。
// 字段来源矩阵:
//   DB 基础字段 → baseSources; process_state → process 镜像;
//   loginState/brokers/selectedBrokerId/schedules/auto_login → md 镜像;
//   18 个 pending → usePending (source:{id}:{op}); expanded → 本地 UI 状态。
export type LoginState = 'offline' | 'pending' | 'online'

/// 确定进度判定（契约 progress）：max > min。
/// 非数值 / max<=min（不确定进度）返回 false。登录态映射与展示钳制共用此判定，
/// 避免「确定进度」语义在两处漂移。
export function isDeterminateProgress(
  p: { min?: unknown; max?: unknown } | null | undefined,
): boolean {
  return typeof p?.min === 'number' && typeof p?.max === 'number'
    && !Number.isNaN(p.min) && !Number.isNaN(p.max) && p.max > p.min
}

// ===== 登录态判定（RTN_PROGRESS 数值映射, 契约 progress 消费者约定）=====
// dzmd_ctp 的 MdStateMachine 状态转移时推送 RTN_PROGRESS（md_state.cpp set_state）:
//   min=0, max=4, current: Idle=0, Connecting/Disconnected=1, Connected=2, LoggingIn=3, LoggedIn=4
// 前端登录态判定唯一实现（marketSources 聚合层与 mdConfig pending 清理共用）:
//   current >= max  = 登录完成 (online);  current <= min = 已登出 (offline);  其余 = 中间态 (pending)
// 禁止依赖 desc 文案做状态判定（desc 为展示文案, 契约 progress 仅定义其为"简短文本说明"）。
// 该数值映射是跨进程稳定契约, md 端变更 current 语义必须同步本函数。
export function progressLoginState(
  p: { min?: unknown; max?: unknown; current?: unknown } | null | undefined,
): LoginState | null {
  const min = typeof p?.min === 'number' ? p.min : Number.NaN
  const max = typeof p?.max === 'number' ? p.max : Number.NaN
  const current = typeof p?.current === 'number' ? p.current : Number.NaN
  if (Number.isNaN(min) || Number.isNaN(max) || Number.isNaN(current)) return null
  if (!isDeterminateProgress(p)) return null  // 不确定进度（契约 progress）: 无状态含义
  if (current >= max) return 'online'
  if (current <= min) return 'offline'
  return 'pending'
}

// ===== StatusIndicator 直连数据（P3 接线）=====
// 有合法 progress 镜像时直连 {min,max,current,desc}（真实进度条），
// 无镜像时回落 loginState 三值映射 {0,0.5,1}（保证组件仍有合理显示）。
// 形状对齐 stores/progress.ts 的 ProgressView（applyProgress 已做类型防御）。
export interface ProgressIndicatorView {
  current: number
  min: number
  max: number
  desc?: string
}

export type ScheduleView = Schedule

export interface MarketSourceView extends MarketSource {
  loginState: LoginState
  progressView: ProgressIndicatorView  // StatusIndicator 直连数据（P3）
  loginPending: boolean
  logoutPending: boolean
  autoLoginPending: boolean
  scheduleAddPending: boolean
  scheduleRemovePending: boolean
  expanded: boolean
  // 以下 7 字段来自契约 md-status MdStatus（CTP 类型 schema，interface_type=ctp）。
  // 放通用视图模型是务实取舍：聚合层单一 computed 是现有架构约定，组件不直接读镜像。
  // xtp 等其他接口类型无对应概念时恒为默认值（''/--/0/null），UI 无感知、无害；
  // 届时若 xtp 状态 schema 差异大，由其 ui_card 组件定义独立解析（见 parseMdStatus 注释）。
  apiVersion: string      // 契约 md-status api_version，镜像未到达为 ''
  sysVersion: string      // 契约 md-status sys_version，镜像未到达为 ''
  loginTime: string       // 契约 md-status login_time，镜像未到达为 ''
  tradingDay: string
  subscribeCount: number
  subscribeTotal: number
  lastLoginError: MdLastErrorView | null  // 契约 md-status 失败语义 last_error；null = 无持久失败原因
  subscribeBatchSize: number | null       // 契约 md-config 订阅参数（镜像未到达为 null）
  subscribeBatchDelayMs: number | null
  subCheckIntervalMs: number | null
  subMaxRetry: number | null
  subscribeParamsPending: boolean
  shmConfigPending: boolean  // 契约 shm SHM 配置下发 pending
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
    progressView: { current: 0, min: 0, max: 1 },
    loginPending: false,
    logoutPending: false,
    autoLoginPending: false,
    scheduleAddPending: false,
    scheduleRemovePending: false,
    expanded: false,
    apiVersion: '',
    sysVersion: '',
    loginTime: '',
    tradingDay: '--',
    subscribeCount: 0,
    subscribeTotal: 0,
    lastLoginError: null,
    subscribeBatchSize: null,
    subscribeBatchDelayMs: null,
    subCheckIntervalMs: null,
    subMaxRetry: null,
    subscribeParamsPending: false,
    shmConfigPending: false,
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

// ===== md_status 镜像解析（契约 md-status CTP 类型，6 字段全量 + last_error）=====
// 层级归属：契约 md-status 的 MdStatus schema 定义于「CTP 类型（interface_type = ctp）」章节，
// 本解析器为 CTP 接口专属（dzmd_ctp*）。dzmd_* 通用层不感知字段细节；
// 将来 xtp 等其他 interface_type 由其独立 ui_card 组件 + 独立解析函数承接，不复用本函数。
// statuses[source].status 为不透明 Record<string, unknown>（dzweb 透传）。
// 逐字段独立 typeof 校验：类型非法的字段单独回落默认值，其余字段正常采用——
// 契约 md-status 清空语义是分层的（进 Idle 全清空、登录失败仅清 login_time 其余保留），
// 空串是合法值不是非法帧，禁止整帧全有或全无判定（否则登录失败场景会误丢 trading_day）。

/// 一次性登录/连接失败（契约 md-status「失败语义（last_error）」）。
/// 展示层 camelCase；进程/SHM 边界为 snake_case（code/message/error_id/timestamp），
/// 转换只发生在 parseMdStatus，组件不接触后端字段形状。
export interface MdLastErrorView {
  code: string       // connect_timeout | login_rejected | login_timeout | login_parse_error（未知码也保留原文，仅用于映射兜底）
  message: string    // 中文用户可读原因；空串 = 无详情，展示时按 code 映射固定文案
  errorId: number    // CTP ErrorID；仅 login_rejected 非 0，其余 0
  timestamp: number  // 失败发生时间，Unix 秒
}

export interface MdStatusView {
  apiVersion: string
  sysVersion: string
  tradingDay: string
  loginTime: string
  subscribeCount: number
  subscribeTotal: number
  lastError: MdLastErrorView | null  // null = 无未清空失败（契约 md-status「失败语义」）
}

/// last_error 解析：对象且 code 为 string 才采纳；其余字段逐字段防御回落
/// （message 非 string → ''，error_id/timestamp 非有限 number → 0）。
/// last_error 为 null/非法 → null（不构成一次失败，不清 pending、不展示）。
function parseLastError(raw: unknown): MdLastErrorView | null {
  if (!raw || typeof raw !== 'object') return null
  const e = raw as { code?: unknown; message?: unknown; error_id?: unknown; timestamp?: unknown }
  if (typeof e.code !== 'string') return null
  const num = (v: unknown): number => (typeof v === 'number' && Number.isFinite(v) ? v : 0)
  return {
    code: e.code,
    message: typeof e.message === 'string' ? e.message : '',
    errorId: num(e.error_id),
    timestamp: num(e.timestamp),
  }
}

/// 失败原因展示文案（契约 md-status 前端义务②）：message 优先；空则按 code 映射固定中文。
/// 未知 code 统一「登录失败」——绝不把 snake_case 机器码直接展示给用户。
const MD_LAST_ERROR_TEXT: Record<string, string> = {
  connect_timeout: '连接超时（30 秒内未连接上前置）',
  login_rejected: '登录被拒绝',
  login_timeout: '登录超时（10 秒内未收到响应）',
  login_parse_error: '登录响应解析失败',
}

export function mdLastErrorText(e: MdLastErrorView | null): string {
  if (!e) return ''
  if (e.message) return e.message
  return MD_LAST_ERROR_TEXT[e.code] || '登录失败'
}

export function parseMdStatus(raw: unknown): MdStatusView | null {
  const s = raw as Partial<{
    api_version: unknown
    sys_version: unknown
    trading_day: unknown
    login_time: unknown
    expected_subscribe_count: unknown
    subscribed_count: unknown
    last_error: unknown
  }> | null | undefined
  if (!s || typeof s !== 'object') return null  // 非对象: 镜像未到达/非法帧
  const str = (v: unknown): string => (typeof v === 'string' ? v : '')
  const num = (v: unknown): number => (typeof v === 'number' && Number.isFinite(v) ? v : 0)
  return {
    apiVersion: str(s.api_version),
    sysVersion: str(s.sys_version),
    tradingDay: str(s.trading_day),
    loginTime: str(s.login_time),
    subscribeCount: num(s.subscribed_count),
    subscribeTotal: num(s.expected_subscribe_count),
    lastError: parseLastError(s.last_error),
  }
}
