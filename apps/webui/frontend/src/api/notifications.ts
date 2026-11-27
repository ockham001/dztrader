import { api } from './client'

/**
 * UI 通知历史（契约 rest §2.6，契约 notify-ui）。
 *
 * 后端返回 `{notifications: [...]}`，条目形状与 WS `notify_ui` 增量**同形**
 * （`source`/`level`/`message`/`timestamp`/`popup`/`count`），且已按契约去重规则折叠
 * （ADR 0060 决策 4）——前端直接并入消息中心，不重复折叠。
 */
export const notificationsApi = {
  /** 拉最近通知（dzweb 环形缓存，条数由 webui.json 的 notify.cache_size 决定） */
  list: async (): Promise<Array<Record<string, unknown>>> => {
    const res = await api.get<{ notifications?: Array<Record<string, unknown>> }>(
      '/api/notifications',
    )
    return res.notifications ?? []
  },
}
