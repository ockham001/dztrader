import { api } from './client'
import type {
  MasterSettingsView,
  WebuiSettingsView,
  WebuiSettingsUpdateBody,
} from '@/types/api'

// 事件通道 SHM 配置请求体 (契约 shm SET_EVENT_SHM_CONFIG): RFC 7386 递归合并 patch,
// 与 marketSources.ts 的 ShmConfigPatch 同构。preload_points 内 key 值 null = 删除该时间点
// (契约 shm 唯一合法 null 位置); page_size_mb 不可变, master 跳过 (前端不下发该字段)
export interface EventShmConfigPatch {
  preload_points?: Record<string, { pages: number; bytes: number } | null>
  check_interval_min?: number
  check_pages?: number
  check_bytes?: number
}

export const settingsApi = {
  // 事件通道配置: RFC 7386 合并 patch 直发 SET_EVENT_SHM_CONFIG, 最终状态以 WS event_shm_config 为准
  setEventShmConfig: (patch: EventShmConfigPatch) =>
    api.put<{ ok: boolean }>('/api/settings/event-shm-config', patch),
  // 只读: 主进程配置
  getMaster: () => api.get<MasterSettingsView>('/api/settings/master'),
  // webui 自身配置
  getWebui: () => api.get<WebuiSettingsView>('/api/settings/webui'),
  setWebui: (data: WebuiSettingsUpdateBody) =>
    api.put<{ ok: boolean }>('/api/settings/webui', data),
}
