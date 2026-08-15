import { api } from './client'
import type { User, UserListResponse, Permission } from '@/types/api'

export interface UserListParams {
  search?: string
  role?: string
  status?: string
  page?: number
  page_size?: number
}

export interface CreateUserBody {
  username: string
  display_name: string
  email?: string
  password: string
  role: string
}

export interface UpdateUserBody {
  display_name: string
  email?: string
  role: string
}

export const usersApi = {
  list: (params: UserListParams) => {
    const query = new URLSearchParams()
    Object.entries(params).forEach(([k, v]) => {
      if (v !== undefined && v !== null && v !== '') query.set(k, String(v))
    })
    return api.get<UserListResponse>(`/api/user?${query.toString()}`)
  },
  get: (id: number) => api.get<User>(`/api/user/${id}`),
  create: (data: CreateUserBody) => api.post<User>('/api/user', data),
  update: (id: number, data: UpdateUserBody) => api.put<User>(`/api/user/${id}`, data),
  remove: (id: number) => api.del<void>(`/api/user/${id}`),
  updateStatus: (id: number, status: 'disabled' | 'enabled' | 'locked' | 'unlocked') =>
    api.put<User>(`/api/user/${id}/status`, { status }),
  resetPassword: (id: number, newPassword: string) =>
    api.put<void>(`/api/user/${id}/password`, { new_password: newPassword }),
  getPermissions: (id: number) => api.get<Permission[]>(`/api/user/${id}/permissions`),
  updatePermissions: (id: number, permissions: Permission[]) =>
    api.put<Permission[]>(`/api/user/${id}/permissions`, { permissions }),
}
