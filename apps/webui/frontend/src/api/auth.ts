import { api } from './client'
import type { LoginRequest, LoginResponse } from '@/types/api'

export interface ChangePasswordResponse {
  ok: boolean
}

export const authApi = {
  login: (data: LoginRequest) => api.post<LoginResponse>('/api/login', data),
  /**
   * 自助改密（契约 rest §2.1）：必须同时提交 `old_password` 与新口令。
   * 契约 rest §1「会话吊销」：成功后本账户既有会话（含当前会话）全部失效，
   * 前端须引导重新登录，不得继续使用旧 token。
   */
  changePassword: (oldPassword: string, newPassword: string) =>
    api.post<ChangePasswordResponse>('/api/auth/change-password', {
      old_password: oldPassword,
      new_password: newPassword,
    }),
  ackDefaultPassword: () =>
    api.post<{ ok: boolean }>('/api/security/ack-default-password'),
}
