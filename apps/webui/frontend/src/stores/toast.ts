import { defineStore } from 'pinia'
import { ref } from 'vue'

export type ToastLevel = 'success' | 'info' | 'warning' | 'error'

export interface ToastItem {
  id: number
  level: ToastLevel
  message: string
  /** 自动消失时长（ms）；0 = 常驻（不自动消失，仅手动关闭） */
  duration: number
  timer: ReturnType<typeof setTimeout> | null
  /** 自本条目进入可见区起的剩余时长（ms），hover 暂停时保留 */
  remaining: number
  /** 本条目开始计时的时刻（performance.now()），用于 hover 暂停时算剩余 */
  startedAt: number
  /** 是否正被 hover（暂停倒计时） */
  paused: boolean
}

/** 同时可见的条数上限（ADR 0060：超出排队，不丢弃） */
export const TOAST_MAX_VISIBLE = 3

/** 分级自动消失时长（ADR 0060 决策：info 4s / warning 6s / error 10s） */
export const TOAST_DURATION: Record<ToastLevel, number> = {
  success: 3000,
  info: 4000,
  warning: 6000,
  error: 10000,
}

let nextId = 1

export const useToastStore = defineStore('toast', () => {
  /** 正在显示的条目（最多 TOAST_MAX_VISIBLE 条） */
  const items = ref<ToastItem[]>([])
  /** 等待补位的条目（可见区满时暂存，队首关闭后依次补位） */
  const queue = ref<ToastItem[]>([])

  function scheduleDismiss(item: ToastItem): void {
    if (item.duration <= 0) return // 常驻：不排定时器
    item.startedAt = performance.now()
    item.timer = setTimeout(() => dismiss(item.id), item.remaining)
  }

  /** 从等待队列补位到可见区（调用方保证有空位） */
  function promoteFromQueue(): void {
    while (items.value.length < TOAST_MAX_VISIBLE && queue.value.length > 0) {
      const next = queue.value.shift()!
      items.value.push(next)
      scheduleDismiss(next)
    }
  }

  function show(message: string, level: ToastLevel = 'success', duration?: number): number {
    const id = nextId++
    const effectiveDuration = duration ?? TOAST_DURATION[level]
    const item: ToastItem = {
      id,
      level,
      message,
      duration: effectiveDuration,
      timer: null,
      remaining: effectiveDuration,
      startedAt: 0,
      paused: false,
    }
    if (items.value.length < TOAST_MAX_VISIBLE) {
      items.value.push(item)
      scheduleDismiss(item)
    } else {
      // 可见区已满：排队等待补位（不丢弃，ADR 0060「超出排队」）
      queue.value.push(item)
    }
    return id
  }

  function success(message: string, duration?: number): number {
    return show(message, 'success', duration)
  }

  function info(message: string, duration?: number): number {
    return show(message, 'info', duration)
  }

  function warning(message: string, duration?: number): number {
    return show(message, 'warning', duration)
  }

  function error(message: string, duration?: number): number {
    return show(message, 'error', duration)
  }

  /** 常驻提示：不自动消失，仅手动关闭（契约 notify-ui 的 popup=true） */
  function persistent(message: string, level: ToastLevel = 'error'): number {
    return show(message, level, 0)
  }

  /** hover 暂停倒计时：记录剩余时长并清定时器 */
  function pause(id: number): void {
    const item = items.value.find(t => t.id === id)
    if (!item || item.duration <= 0 || item.paused) return
    item.paused = true
    item.remaining = Math.max(0, item.remaining - (performance.now() - item.startedAt))
    if (item.timer) {
      clearTimeout(item.timer)
      item.timer = null
    }
  }

  /** hover 离开恢复倒计时 */
  function resume(id: number): void {
    const item = items.value.find(t => t.id === id)
    if (!item || item.duration <= 0 || !item.paused) return
    item.paused = false
    scheduleDismiss(item)
  }

  function dismiss(id: number): void {
    const idx = items.value.findIndex(t => t.id === id)
    if (idx !== -1) {
      const item = items.value[idx]
      if (item.timer) clearTimeout(item.timer)
      items.value.splice(idx, 1)
      promoteFromQueue()
      return
    }
    // 还在排队区：直接从队列移除
    const qIdx = queue.value.findIndex(t => t.id === id)
    if (qIdx !== -1) queue.value.splice(qIdx, 1)
  }

  function clear(): void {
    items.value.forEach(t => { if (t.timer) clearTimeout(t.timer) })
    items.value = []
    queue.value = []
  }

  return { items, queue, show, success, info, warning, error, persistent, pause, resume, dismiss, clear }
})
