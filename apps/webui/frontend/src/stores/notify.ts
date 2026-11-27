import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { useToastStore } from '@/stores/toast'

export type NotifyLevel = 'info' | 'warning' | 'error'

export interface NotifyItem {
  /** 稳定标识：用于折叠更新与已读判定（不上行，本地生成） */
  id: string
  source?: string
  message: string
  level: NotifyLevel
  /** 毫秒时间戳（契约 payload 是 Unix 秒，入 store 后统一换算） */
  timestamp: number
  /** 是否常驻呈现（契约 notify-ui 的 popup；ADR 0060 决策 3 起语义为「常驻」不是「打断」） */
  popup?: boolean
  /** 去重折叠次数，dzweb 填（契约 notify-ui）；1 = 未折叠 */
  count: number
}

/** 缓存上限（与后端 NotifyCache 默认一致；后端可配，前端不读配置） */
const MAX_ITEMS = 100

/** 仪表盘面板显示条数 */
export const NOTIFY_PANEL_LIMIT = 50

/** 折叠指纹：source + level + 文本（与 dzweb 的指纹口径同构；前端只做「就地更新」不做窗口判定） */
function foldKey(item: { source?: string; level: NotifyLevel; message: string }): string {
  return `${item.source ?? ''}\x1f${item.level}\x1f${item.message}`
}

/**
 * notify_ui 统一分发（契约 notify-ui / ADR 0060）。
 *
 * 职责分工（ADR 0060 决策 4/6）：
 * - **去重折叠在 dzweb**：后端已按指纹窗口合并并填 `count`，前端收到的每一条
 *   都已是折叠结果。前端的任务只是：若该指纹已在本地列表里（同一事件的新一轮），
 *   **就地更新** `count`/`timestamp`，不新增一行；
 * - **两个呈现面**：消息中心（本 store 的 `items`，留档）+ 即时提示（toast store）。
 *   `popup=true` 的条目走常驻 toast（不自动消失、不遮挡）。
 *
 * 已读状态纯前端内存（刷新后清零）：已读不是运维刚需，不为它加后端字段。
 */
export const useNotifyStore = defineStore('notify', () => {
  const items = ref<NotifyItem[]>([])
  /**
   * 已读条目的 id 集合（纯前端内存，刷新后清零）。
   *
   * 为什么不用「时间戳水位」：折叠会把**旧条目**的 timestamp 更新成比列表末元素更新，
   * 水位式判定会让这条老消息重新变成未读、徽标卡在 1 且「全部已读」清不掉（审查实测）。
   * 按 id 记已读对折叠天然正确——折叠只改已有条目的 count/timestamp，不改 id。
   */
  const readIds = ref(new Set<string>())

  const unreadCount = computed(
    () => items.value.filter(i => !readIds.value.has(i.id)).length,
  )

  /** 面板显示用：最近 NOTIFY_PANEL_LIMIT 条，新的在前 */
  const recent = computed(() => items.value.slice(-NOTIFY_PANEL_LIMIT).reverse())

  function nextId(): string {
    return `${Date.now()}-${Math.random().toString(36).slice(2, 9)}`
  }

  /**
   * 收一条通知（WS 增量或历史基线）。
   * @param message 正文
   * @param source 来源
   * @param level 级别（缺省 error，与既有 handler 规范化一致）
   * @param popup 是否常驻（契约 notify-ui）
   * @param timestamp 契约是 Unix 秒；缺省用当前时间
   * @param count 后端折叠计数（缺省 1）
   */
  function push(
    message: string,
    source?: string,
    level?: NotifyLevel,
    popup?: boolean,
    timestamp?: number,
    count?: number,
  ): void {
    if (!message) return // 空消息不提示不缓存
    const lv = level ?? 'error'
    const ts = timestamp === undefined ? Date.now() : timestamp * 1000
    const foldedCount = count && count > 0 ? count : 1

    // 折叠：同一指纹已在列表里 → 就地更新，不新增行（后端已折叠，这里只对齐本地视图）
    const key = foldKey({ source, level: lv, message })
    const existing = items.value.find(i => foldKey(i) === key)
    if (existing) {
      // count 取较大者：后端冷却到点后会以 count=1 重新发一条同文案的新事件，
      // 若直接覆盖会把已累计的次数抹掉（审查实测：REST 基线刷新后同一批数据变两行、
      // 前端却仍是一行 count=1，前后视图不一致）。取 max 保持单调。
      existing.count = Math.max(existing.count, foldedCount)
      existing.timestamp = ts
      existing.popup = popup ?? existing.popup
      // 折叠命中不重复弹 toast：用户已经看过这条（ADR 0060：窗口内不重复提示）
      return
    }

    items.value.push({ id: nextId(), source, message, level: lv, timestamp: ts, popup, count: foldedCount })
    if (items.value.length > MAX_ITEMS) {
      items.value.splice(0, items.value.length - MAX_ITEMS)
    }

    // 即时提示：popup=true → 常驻（不自动消失、不遮挡）；否则按级别短暂显示
    const toast = useToastStore()
    if (popup) {
      toast.persistent(message, lv)
    } else if (lv === 'error') {
      toast.error(message)
    } else if (lv === 'warning') {
      toast.warning(message)
    } else {
      toast.info(message)
    }
  }

  /** 用 GET /api/notifications 的返回值重建列表（历史基线，刷新后仍有内容） */
  function applyBaseline(payloads: Array<Record<string, unknown>>): void {
    items.value = []
    for (const p of payloads) {
      const rawLevel = p.level
      const level: NotifyLevel =
        rawLevel === 'info' ? 'info' : rawLevel === 'warning' ? 'warning' : 'error'
      const ts = typeof p.timestamp === 'number' ? p.timestamp * 1000 : Date.now()
      items.value.push({
        id: nextId(),
        source: typeof p.source === 'string' ? p.source : undefined,
        message: typeof p.message === 'string' ? p.message : '',
        level,
        timestamp: ts,
        popup: p.popup === true,
        count: typeof p.count === 'number' && p.count > 0 ? p.count : 1,
      })
    }
    if (items.value.length > MAX_ITEMS) {
      items.value.splice(0, items.value.length - MAX_ITEMS)
    }
    // 基线视为已读：用户刚打开页面，不把历史全算成未读
    readIds.value = new Set(items.value.map(i => i.id))
  }

  /** 全部标为已读 */
  function markAllRead(): void {
    readIds.value = new Set(items.value.map(i => i.id))
  }

  function clear(): void {
    items.value = []
    readIds.value = new Set()
  }

  return { items, recent, unreadCount, push, applyBaseline, markAllRead, clear }
})
