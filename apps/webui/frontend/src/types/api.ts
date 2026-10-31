// 契约单源生成的 WS 领域类型：由 schema → generated.ts 提供，此处 import 提供本地绑定（供
// LogContent 等内部引用）并转发对外；勿在此手写重复定义（避免与生成物同名冲突）
import type { BrokerFrontend, BrokerEntry, LogLine, MdConfigPayload, MdRtnConfigPayload, MdRtnStatusPayload } from './generated'
export type { BrokerFrontend, BrokerEntry, LogLine, MdConfigPayload, MdRtnConfigPayload, MdRtnStatusPayload }

// ===== Authentication =====
export interface LoginRequest {
  username: string
  password: string
}

// ===== System Info =====
/**
 * 后端进程自述（GET /api/system/info，契约 rest §2.1，免认证）
 * - `process_name`：dzweb 可执行文件名（不含扩展名），用于前端比对 logger 名禁用自我 tail
 * - `version`：工程版本串（`DZ_VERSION_STRING`，单一真相源根 CMakeLists.txt 的 project(VERSION)），
 *   「关于」页显示此值，前端不得硬编码版本（#207 A3）
 */
export interface SystemInfo {
  process_name: string
  version: string
}

export interface LoginResponse {
  token: string
  expires_in: number
  user: User
  is_default_password?: boolean
}

/**
 * POST /api/login 失败错误体（契约 rest §1/§2.1）。
 * - `error`：机器码本身（invalid_credentials / account_disabled / ip_banned / account_locked），
 *   **无 code 字段**；文案映射见 composables/loginErrors.ts 的 LOGIN_ERROR_TEXT。
 * - `locked_until`：仅 account_locked / 423 携带，**绝对 Unix 秒级时间戳**（与 User.locked_until 同语义），
 *   剩余分钟须按 locked_until - now 计算，不得解释为剩余秒数。
 */
export interface LoginError {
  error: string
  locked_until?: number
}

// ===== User =====
export interface User {
  id: number
  username: string
  display_name: string
  email: string
  role: 'admin' | 'user'
  status: 'online' | 'offline' | 'disabled' | 'locked'
  locked_until?: number
  last_login_at?: string
  last_login_ip?: string
  created_at: string
}

export interface UserListResponse {
  users: User[]
  total: number
}

// ===== Security =====
export interface SecurityConfig {
  login_lockout_enabled: boolean
  access_mode: 'auto' | 'whitelist' | 'blacklist'
  max_failed_attempts: number
  lockout_duration_sec: number
}

export interface IpEntry {
  id: number
  ip: string
  reason?: string
  source?: string
  created_at: string
}

export interface LoginHistoryEntry {
  id: number
  username: string
  ip: string
  device?: string
  user_agent?: string
  success: boolean
  created_at: string
  reason?: string
}

export interface LoginHistoryResponse {
  entries: LoginHistoryEntry[]
  total: number
}

// ===== Market Sources =====
export interface MarketSource {
  id: number
  source_type: string
  source_name: string
  display_name: string
  ui_card: string
  is_added: boolean
  auto_login: boolean
  created_at: string
  updated_at: string
}

export interface Schedule {
  id: number
  source_id: number
  login_time: string
  logout_time: string
}

// BrokerFrontend / BrokerEntry 已由契约单源（generated）提供，见文件顶部转发

// ===== Log Monitoring =====
export type LogLevel = 'trace' | 'debug' | 'info' | 'warning' | 'error' | 'critical' | 'off'

export interface LogFile {
  name: string
  logger: string
  size: number
  mtime: string
  path: string
}

// LogLine 已由契约单源（generated）提供，见文件顶部转发

export interface LogContent {
  lines: LogLine[]
  total: number
}

export interface LogStats {
  by_level: Record<string, number>
  total: number
  timespan: string
}

export interface LogAggregate {
  msg_pattern: string
  count: number
  first_ts: string
  last_ts: string
  samples: string[]
}

export interface TimelineBucket {
  ts: string
  counts: Record<string, number>
}

export interface LogProcess {
  name: string
  type: string
  level: string
}

export interface SetLevelResult {
  name: string
  ok: boolean
  old: string | null
  new: string
}

export interface FlushResult {
  name: string
  ok: boolean
}

// ===== Process Control =====
// 进程状态/事件/状态载荷由契约单源生成（schema/domain-payloads.schema.json → src/types/generated.ts），
// 此处仅转发，勿手写重复定义（避免与生成物同名冲突）
export type { ProcessState, ProcessEvent, ProcessStatusPayload } from './generated'

// MdConfigPayload / MdRtnConfigPayload / MdRtnStatusPayload 已由契约单源（generated）提供，见文件顶部转发

// ===== 系统设置 =====
// 注: 事件通道配置展示/编辑复用 types/mirror.ts 的 ShmConfigView (契约 shm), 不在 api.ts 重复定义
export interface MasterSettingsView {
  single_stop_timeout_sec: number
  cleanup_max_page_count: number
  cleanup_max_page_age_hours: number
  meta_file_size: number
}

export interface WebuiSettingsView {
  server_listen: string
  server_port: number
  token_ttl_sec: number
  jwt_secret_set: boolean
  notify_cache_size: number
}

export interface WebuiSettingsUpdateBody {
  token_ttl_sec?: number
  notify_cache_size?: number
}

// ===== 系统监控历史查询（《帧契约：系统监控》§6.1、契约 rest §2.8）=====
// 历史查询走 REST（dzweb 只读打开历史库），不是 WS 领域消息，故类型定义在此而非
// schema/domain-payloads.schema.json（后者只承载 WS 领域载荷）。

/** 历史查询的桶层级（`auto` 由服务端按范围宽度选择，回显实际层级） */
export type SysmonHistoryLevel = 'auto' | 'raw' | 'minute'

/** 历史查询的不可用原因码（与 libs/sysmonstore 的 HistoryAvailability 一一对应） */
export type SysmonHistoryUnavailableReason = 'not_found' | 'schema_mismatch' | 'open_failed'

/** 历史查询的标记 kind（《帧契约：系统监控》§6.1.4） */
export type SysmonHistoryMarkKind = 'sample_gap' | 'restart' | 'clock_jump' | 'unmarked'

/** 一个历史桶的聚合量；该指标在该桶内无有效样本时四值与 n 全为 null（不得用 0 冒充） */
export interface SysmonHistoryPoint {
  ts: number
  min: number | null
  max: number | null
  avg: number | null
  last: number | null
  n: number | null
}

/** 一条历史序列（本版本恒 1 条；数组形状保留以便将来多维度叠加是 additive 扩展） */
export interface SysmonHistorySeries {
  dimension: string
  points: SysmonHistoryPoint[]
}

/** 一条历史标记（事实清单，供图上标记与 tooltip 文案） */
export interface SysmonHistoryMark {
  ts: number
  kind: SysmonHistoryMarkKind
  detail: string
}

/** 序列内一段不连续区间（缺口底纹）；kind 解释原因，unmarked = 缺了但没标记 */
export interface SysmonHistoryGap {
  from: number
  to: number
  kind: SysmonHistoryMarkKind
}

/** 历史库可用时的响应（available=true） */
export interface SysmonHistoryAvailable {
  available: true
  level: Exclude<SysmonHistoryLevel, 'auto'>
  metric: string
  from: number
  to: number
  /** 桶宽（秒）；范围内无任何桶时为 null（无桶即无桶宽，不得猜一个值） */
  interval_sec: number | null
  /** true = 因 max_points 上限窗口前移，from 是实际返回首点的桶时刻 */
  truncated: boolean
  series: SysmonHistorySeries[]
  marks: SysmonHistoryMark[]
  gaps: SysmonHistoryGap[]
}

/** 历史库不可用时的降级响应（available=false；不含任何数据字段） */
export interface SysmonHistoryUnavailable {
  available: false
  reason: SysmonHistoryUnavailableReason
}

export type SysmonHistoryResponse = SysmonHistoryAvailable | SysmonHistoryUnavailable

/** 历史查询参数（《帧契约：系统监控》§6.1；除 metric 外均可选） */
export interface SysmonHistoryQueryParams {
  metric?: string
  /** 维度指标必填、标量指标必须省略 */
  dimension?: string
  from?: number
  to?: number
  level?: SysmonHistoryLevel
  max_points?: number
}
