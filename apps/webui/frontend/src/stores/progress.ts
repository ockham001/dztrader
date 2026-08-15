import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { ProgressPayload } from '@/types/mirror'

export interface ProgressView {
  min: number
  max: number
  current: number
  desc?: string
}

// 契约 05 progress 镜像：单条完整状态，后到覆盖先到；无 SET、无持久化。
// 真实 UI 价值：行情源登录进度展示（设计 §5.2）。
export const useProgressStore = defineStore('progress', () => {
  const progress = ref<Record<string, ProgressView>>({})

  // 单条完整覆盖：payload 为 { min, max, current, desc? }，直接替换该 instanceId 条目。
  // 非法 payload（非对象 / 空 instanceId）忽略，不写不抛（前端展示层过滤）。
  function applyProgress(instanceId: string, payload: unknown): void {
    if (!instanceId) return
    const p = payload as Partial<ProgressPayload> | null | undefined
    if (!p || typeof p !== 'object') return
    progress.value[instanceId] = {
      min: typeof p.min === 'number' ? p.min : 0,
      max: typeof p.max === 'number' ? p.max : 0,
      current: typeof p.current === 'number' ? p.current : 0,
      desc: typeof p.desc === 'string' ? p.desc : undefined,
    }
  }

  return { progress, applyProgress }
})
