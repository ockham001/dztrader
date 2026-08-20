import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useSettingsStore, EVENT_SHM_KEY, EVENT_SHM_TIMEOUT_MS } from '../settings'
import { usePending, __resetForTests, PENDING_TIMEOUT } from '@/composables/usePending'
import { useToastStore } from '@/stores/toast'
import { settingsApi } from '@/api/settings'

// Mock API module（settings store 的所有操作走 settingsApi）
vi.mock('@/api/settings', () => ({
  settingsApi: {
    setEventShmConfig: vi.fn(),
    getMaster: vi.fn(),
    getWebui: vi.fn(),
    setWebui: vi.fn(),
  },
}))

const validPayload = {
  page_size_mb: 32,
  preload_points: { '08:45': { pages: 1, bytes: 0 } },
  check_interval_min: 5,
  check_pages: 1,
  check_bytes: 0,
}

// usePending 是模块级状态（pending + timers Map），每个用例前重置；
// 超时路径会调用 useToastStore，需要 active pinia
describe('useSettingsStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetForTests()
    vi.clearAllMocks()
    vi.useFakeTimers()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  describe('applyEventShmConfig', () => {
    it('合法 payload 全量写入镜像（toEqual 断言）', () => {
      const store = useSettingsStore()
      store.applyEventShmConfig(validPayload)
      expect(store.eventShmConfig).toEqual(validPayload)
    })

    it('非法 payload 忽略不写', () => {
      const store = useSettingsStore()
      store.applyEventShmConfig({ page_size_mb: 'x', check_pages: 1 })
      expect(store.eventShmConfig).toBeNull()
    })

    it('合法 payload 到达清 pending（用导出的 EVENT_SHM_KEY）', () => {
      const { pending } = usePending()
      const store = useSettingsStore()
      pending[EVENT_SHM_KEY] = true
      store.applyEventShmConfig(validPayload)
      expect(pending[EVENT_SHM_KEY]).toBe(false)
    })

    it('先写合法镜像 → 再喂非法 payload → 镜像不变', () => {
      const store = useSettingsStore()
      store.applyEventShmConfig(validPayload)
      store.applyEventShmConfig({ page_size_mb: 'x' })
      store.applyEventShmConfig(null)
      expect(store.eventShmConfig).toEqual(validPayload)
    })

    it('非法 payload 不清 pending', () => {
      const { pending } = usePending()
      const store = useSettingsStore()
      pending[EVENT_SHM_KEY] = true
      store.applyEventShmConfig({ page_size_mb: 'x', check_pages: 1 })
      expect(pending[EVENT_SHM_KEY]).toBe(true)
      expect(store.eventShmConfig).toBeNull()
    })
  })

  describe('setEventShmConfig', () => {
    it('HTTP 成功 → pending 保持 true、返回 true；合法 apply 清 pending', async () => {
      vi.mocked(settingsApi.setEventShmConfig).mockResolvedValue({ ok: true })
      const store = useSettingsStore()
      const r = await store.setEventShmConfig({ check_interval_min: 10 })
      expect(r).toBe(true)
      expect(settingsApi.setEventShmConfig).toHaveBeenCalledWith({ check_interval_min: 10 })
      // keepPendingOnSuccess 默认 true: 成功后 pending 仍挂起, 等 WS RTN (applyEventShmConfig) 清
      expect(usePending().pending[EVENT_SHM_KEY]).toBe(true)

      store.applyEventShmConfig(validPayload)
      expect(usePending().pending[EVENT_SHM_KEY]).toBe(false)
    })

    it('HTTP reject → 返回 false、pending 清 false', async () => {
      vi.mocked(settingsApi.setEventShmConfig).mockRejectedValue(new Error('503'))
      const store = useSettingsStore()
      const r = await store.setEventShmConfig({ check_interval_min: 10 })
      expect(r).toBe(false)
      expect(usePending().pending[EVENT_SHM_KEY]).toBe(false)
    })

    it('超时（mock api 挂起, advance 超时+1ms）→ PENDING_TIMEOUT、pending 清 false、toast 一次', async () => {
      vi.mocked(settingsApi.setEventShmConfig).mockImplementation(() => new Promise(() => {}))
      const store = useSettingsStore()
      const p = store.setEventShmConfig({ check_interval_min: 10 })
      expect(usePending().pending[EVENT_SHM_KEY]).toBe(true)

      await vi.advanceTimersByTimeAsync(EVENT_SHM_TIMEOUT_MS + 1)
      expect(usePending().pending[EVENT_SHM_KEY]).toBe(false)
      const result = await p
      expect(result).toBe(PENDING_TIMEOUT)  // 超时供调用方识别去重双 toast (§7.2)

      const toast = useToastStore()
      expect(toast.items).toHaveLength(1)
      expect(toast.items[0].level).toBe('error')
      expect(toast.items[0].message).toBe('事件通道配置下发超时')
    })

    it('防重入: 已有 pending 时返回 false 且不发重复 HTTP', async () => {
      vi.mocked(settingsApi.setEventShmConfig).mockResolvedValue({ ok: true })
      const store = useSettingsStore()
      // 首次提交 HTTP 成功, keepPendingOnSuccess 保持 pending 挂起
      const r1 = await store.setEventShmConfig({ check_interval_min: 10 })
      expect(r1).toBe(true)
      expect(usePending().pending[EVENT_SHM_KEY]).toBe(true)

      const r2 = await store.setEventShmConfig({ check_pages: 2 })
      expect(r2).toBe(false)
      expect(settingsApi.setEventShmConfig).toHaveBeenCalledTimes(1)
    })

    it('空 patch 幂等成功且不下发', async () => {
      const store = useSettingsStore()
      const r = await store.setEventShmConfig({})
      expect(r).toBe(true)
      expect(settingsApi.setEventShmConfig).not.toHaveBeenCalled()
      expect(usePending().pending[EVENT_SHM_KEY]).toBeUndefined()
    })
  })
})
