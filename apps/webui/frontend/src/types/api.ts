// ===== Authentication =====
export interface LoginRequest {
  username: string
  password: string
}

// ===== System Info =====
/** 后端进程信息（/api/system/info），用于前端比对 logger 名禁用自我 tail */
export interface SystemInfo {
  process_name: string
}

export interface LoginResponse {
  token: string
  expires_in: number
  user: User
  is_default_password?: boolean
}

export type LoginErrorCode = 'invalid_credentials' | 'account_disabled' | 'account_locked' | 'ip_banned' | 'network_error'

export interface LoginError {
  error: string
  code?: LoginErrorCode
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

export interface Permission {
  type: 'account' | 'strategy'
  id: string
  granted: boolean
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

// 经纪商前置地址: 由 address 唯一标识 (broker 内部), 无独立 id
export interface BrokerFrontend {
  address: string
  label: string
  enabled: boolean
}

// 经纪商条目: 由 name 唯一标识 (source 内部), 无独立 id
// 对应后端 dzmd_ctp MdConfig::BrokerEntry (Wave 1B commit 1f35e61)
export interface BrokerEntry {
  name: string
  broker_id: string
  user_id: string
  password: string
  product_info: string
  frontends: BrokerFrontend[]
}

// ===== Log Monitoring =====
export type LogLevel = 'trace' | 'debug' | 'info' | 'warning' | 'error' | 'critical' | 'off'

export interface LogFile {
  name: string
  logger: string
  size: number
  mtime: string
  path: string
}

export interface LogLine {
  n: number
  ts: string
  level: string
  logger: string
  func: string
  file: string
  line: number
  pid: string
  tid: string
  msg: string
  raw: string
  parsed: boolean
}

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
export type ProcessState = 'Starting' | 'Running' | 'Stopping' | 'Stopped' | 'Crashed'

// 进程操作结果事件（契约 03）：仅 RTN_PROCESS_STATUS 响应 REQUEST_PROCESS_CONTROL 时出现
export type ProcessEvent =
  | 'StartSucceeded' | 'StartFailed'
  | 'StopSucceeded' | 'StopFailed'
  | 'RemoveSucceeded' | 'RemoveFailed'

export interface ProcessStatusPayload {
  name: string
  state: ProcessState
  pid: number
  message?: string
  display_name?: string
  event?: ProcessEvent   // 缺失 = 自发状态变化（崩溃/重启/快照），不清 pending
}

// 行情源配置载荷 (md_rtn_config 帧 data.config / GET /market-sources/{id}/config)
// 字段与 stores/marketSources.ts setMdConfig 的实际解析对齐 (P3 Task 1)
// 契约 08: brokers/current_broker_name + 订阅参数；排程/自动登录已迁移契约 04 (auto_login 帧)
export interface MdConfigPayload {
  brokers: BrokerEntry[]
  current_broker_name: string
  subscribe_batch_size?: number
  subscribe_batch_delay_ms?: number
  sub_check_interval_ms?: number
  sub_max_retry?: number
}

export interface MdRtnConfigPayload {
  source: string
  config: MdConfigPayload
}

export interface MdRtnStatusPayload {
  source: string
  status: Record<string, unknown>
}
