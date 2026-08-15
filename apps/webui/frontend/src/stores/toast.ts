import { defineStore } from 'pinia'
import { ref } from 'vue'

export type ToastLevel = 'success' | 'info' | 'warning' | 'error'

export interface ToastItem {
  id: number
  level: ToastLevel
  message: string
  duration: number
  timer: ReturnType<typeof setTimeout> | null
}

let nextId = 1

export const useToastStore = defineStore('toast', () => {
  const items = ref<ToastItem[]>([])

  function show(message: string, level: ToastLevel = 'success', duration = 3000): number {
    const id = nextId++
    const timer = duration > 0 ? setTimeout(() => dismiss(id), duration) : null
    items.value.push({ id, level, message, duration, timer })
    return id
  }

  function success(message: string, duration?: number): number {
    return show(message, 'success', duration)
  }

  // Task 9: setNotifyUi 需要 info level toast (notify_ui level='info' 时调用)
  function info(message: string, duration?: number): number {
    return show(message, 'info', duration)
  }

  function warning(message: string, duration?: number): number {
    return show(message, 'warning', duration)
  }

  function error(message: string, duration?: number): number {
    return show(message, 'error', duration ?? 5000)
  }

  function dismiss(id: number): void {
    const idx = items.value.findIndex(t => t.id === id)
    if (idx === -1) return
    const item = items.value[idx]
    if (item.timer) clearTimeout(item.timer)
    items.value.splice(idx, 1)
  }

  function clear(): void {
    items.value.forEach(t => { if (t.timer) clearTimeout(t.timer) })
    items.value = []
  }

  return { items, show, success, info, warning, error, dismiss, clear }
})
