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
