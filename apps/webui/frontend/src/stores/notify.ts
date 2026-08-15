import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { useToastStore } from '@/stores/toast'

export type NotifyLevel = 'info' | 'warning' | 'error'

export interface NotifyItem {
  source?: string
  message: string
  level: NotifyLevel
  timestamp: number
  popup?: boolean
}

// 缓存上限（参照后端 NotifyCache 语义）：只保留最近 N 条；0 = 禁用缓存（仅弹 toast）
const MAX_ITEMS = 100

// notify_ui 统一分发（设计 §5.2/§5.4，双 toast 修复核心）：
// push 只走 toast store，绝不写任何其他 store 的 error.value ——
// error.value 仅 HTTP 失败链路（catch → error.value → View watch 弹 toast）写入，
// 两通道互斥、职责分离。
// 契约 03 前端义务：level=error 且 popup=true 必须打断用户展示——
// 进入 popup 队列（App.vue 渲染 modal，逐条确认），其余级别仅 toast。
export const useNotifyStore = defineStore('notify', () => {
  const items = ref<NotifyItem[]>([])
  const popupQueue = ref<NotifyItem[]>([])
  const popupCurrent = computed<NotifyItem | null>(() => popupQueue.value[0] ?? null)

  function push(message: string, source?: string, level?: NotifyLevel, popup?: boolean): void {
    if (!message) return // 空消息不弹 toast 不缓存（与 marketSources.setNotifyUi 行为一致）
    const lv = level ?? 'error' // level 缺失时默认 error（与现有 handler 规范化一致）
    if (MAX_ITEMS > 0) {
      items.value.push({ source, message, level: lv, timestamp: Date.now(), popup })
      if (items.value.length > MAX_ITEMS) {
        items.value.splice(0, items.value.length - MAX_ITEMS)
      }
    }
    const toast = useToastStore()
    if (lv === 'error') toast.error(message)
    else if (lv === 'warning') toast.warning(message)
    else toast.info(message)
    // 打断展示：仅 error + popup=true 入队（契约 03）
    if (popup && lv === 'error') {
      popupQueue.value.push({ source, message, level: lv, timestamp: Date.now(), popup })
    }
  }

  // 确认当前弹窗并出队（App.vue modal 按钮调用）
  function ackPopup(): void {
    popupQueue.value.shift()
  }

  function clear(): void {
    items.value = []
    popupQueue.value = []
  }

  return { items, popupQueue, popupCurrent, push, ackPopup, clear }
})
