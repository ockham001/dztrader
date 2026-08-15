import { api } from './client'
import type { SecurityConfig, IpEntry, LoginHistoryResponse } from '@/types/api'

export interface UpdateSecurityConfigBody {
  login_lockout_enabled: boolean
  access_mode: 'auto' | 'whitelist' | 'blacklist'
  max_failed_attempts: number
  lockout_duration_sec: number
}

export interface AddIpBody {
  ip: string
  reason?: string
}

export const securityApi = {
  getConfig: () => api.get<SecurityConfig>('/api/security/config'),
  setConfig: (data: UpdateSecurityConfigBody) => api.put<SecurityConfig>('/api/security/config', data),
  listBlacklist: () => api.get<IpEntry[]>('/api/security/blacklist'),
  addBlacklist: (data: AddIpBody) => api.post<IpEntry>('/api/security/blacklist', data),
  removeBlacklist: (id: number) => api.del<void>(`/api/security/blacklist/${id}`),
  listWhitelist: () => api.get<IpEntry[]>('/api/security/whitelist'),
  addWhitelist: (data: AddIpBody) => api.post<IpEntry>('/api/security/whitelist', data),
  removeWhitelist: (id: number) => api.del<void>(`/api/security/whitelist/${id}`),
  loginHistory: (page: number, pageSize: number, days?: number) =>
    api.get<LoginHistoryResponse>(`/api/security/login-history?page=${page}&page_size=${pageSize}${days ? `&days=${days}` : ''}`),
}
