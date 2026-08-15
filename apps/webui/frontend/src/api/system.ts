import { api } from './client'
import type { SystemInfo } from '@/types/api'

export const systemApi = {
  info: () => api.get<SystemInfo>('/api/system/info'),
}
