import { api } from './client'
import type { LoginRequest, LoginResponse } from '@/types/api'

export interface ChangePasswordResponse {
  ok: boolean
}

export const authApi = {
  login: (data: LoginRequest) => api.post<LoginResponse>('/api/login', data),
  changePassword: (newPassword: string) =>
    api.post<ChangePasswordResponse>('/api/auth/change-password', { new_password: newPassword }),
  ackDefaultPassword: () =>
    api.post<{ ok: boolean }>('/api/security/ack-default-password'),
}
