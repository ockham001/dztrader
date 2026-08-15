import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useProcessStore } from '../process'
import { usePending, __resetForTests } from '@/composables/usePending'
import { useToastStore } from '@/stores/toast'
import { marketSourcesApi } from '@/api/marketSources'

// Mock API module（process store 的 start/stop/removeSource 走 marketSourcesApi）
vi.mock('@/api/marketSources', () => ({
  marketSourcesApi: {
    start: vi.fn(),
    stop: vi.fn(),
    remove: vi.fn(),
  },
}))

// usePending 是模块级状态（pending + timers Map），每个用例前重置；
// 超时路径会调用 useToastStore，需要 active pinia

describe('useProcessStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetForTests()
    vi.clearAllMocks()
    vi.useFakeTimers()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  describe('applyProcessStatus', () => {
    it('写入完整条目（name → ProcessStatusPayload）', () => {
      const store = useProcessStore()
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 1234 })
      expect(store.statuses['dzmd_ctp']).toEqual({ name: 'dzmd_ctp', state: 'Running', pid: 1234 })
    })

    it('后到完整覆盖先到（含 state 归 Stopped / pid 归 0）', () => {
      const store = useProcessStore()
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 1234, display_name: 'CTP' })
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Stopped', pid: 0 })
      expect(store.statuses['dzmd_ctp']).toEqual({ name: 'dzmd_ctp', state: 'Stopped', pid: 0 })
    })

    it('不同 name 互不影响', () => {
      const store = useProcessStore()
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 1 })
      store.applyProcessStatus({ name: 'dzmd_other', state: 'Stopped', pid: 0 })
      expect(Object.keys(store.statuses)).toEqual(['dzmd_ctp', 'dzmd_other'])
      expect(store.statuses['dzmd_ctp'].state).toBe('Running')
    })

    it('非法 payload（缺 name / 非对象）忽略不写', () => {
      const store = useProcessStore()
      store.applyProcessStatus({} as never)
      store.applyProcessStatus({ state: 'Running', pid: 1 } as never)
      store.applyProcessStatus(null as never)
      expect(Object.keys(store.statuses)).toHaveLength(0)
    })

    // 契约 04: 已移除进程（configs 有条目且不含该进程）的晚到 status 忽略（不重建镜像）
    it('configs 非空且不含该进程时忽略（已移除进程不重建镜像）', () => {
      const store = useProcessStore()
      store.applyProcessConfig({ dzmd_ctp: { a: 1 } })
      store.applyProcessStatus({ name: 'dzmd_removed', state: 'Stopped', pid: 0 })
      expect(store.statuses['dzmd_removed']).toBeUndefined()
    })

    // 顺序竞态兜底：快照/启动初期 configs 尚空时放行，由后续全量覆盖收敛
    it('configs 为空时放行（process_status 先于 process_config 到达的竞态）', () => {
      const store = useProcessStore()
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 1234 })
      expect(store.statuses['dzmd_ctp']).toEqual({ name: 'dzmd_ctp', state: 'Running', pid: 1234 })
    })
  })

  describe('applyProcessConfig', () => {
    it('全量覆盖写入（key = 进程名）', () => {
      const store = useProcessStore()
      store.applyProcessConfig({
        dzmd_ctp: { restart: { enabled: true, max_attempts: 5 } },
        dzmd_other: { restart: { enabled: false } },
      })
      expect(store.configs['dzmd_ctp']).toEqual({ restart: { enabled: true, max_attempts: 5 } })
      expect(store.configs['dzmd_other']).toEqual({ restart: { enabled: false } })
    })

    it('覆盖天然含删除：后到全量不含的条目消失（契约 03: 条目消失 = 进程已移除）', () => {
      const store = useProcessStore()
      store.applyProcessConfig({ dzmd_ctp: { a: 1 }, dzmd_other: { b: 2 } })
      store.applyProcessConfig({ dzmd_ctp: { a: 2 } })
      expect(store.configs['dzmd_ctp']).toEqual({ a: 2 })
      expect(store.configs['dzmd_other']).toBeUndefined()
      expect(Object.keys(store.configs)).toEqual(['dzmd_ctp'])
    })

    it('非法 payload 忽略不写', () => {
      const store = useProcessStore()
      store.applyProcessConfig(null as never)
      store.applyProcessConfig('bad' as never)
      store.applyProcessConfig([] as never)      // 数组 payload 忽略
      store.applyProcessConfig([{ a: 1 }] as never)
      expect(Object.keys(store.configs)).toHaveLength(0)
    })
  })

  describe('start', () => {
    it('HTTP 成功不清 pending（等 rtn_process_control 清，keepPendingOnSuccess 语义）', async () => {
      vi.mocked(marketSourcesApi.start).mockResolvedValue({ ok: true, source: 'dzmd_ctp' })
      const store = useProcessStore()
      const result = await store.start(1, 'dzmd_ctp')
      expect(result).toBe(true)
      const { pending } = usePending()
      expect(pending['source:1:start']).toBe(true)  // 成功保持挂起

      // process_status 带 event 到达 → resolve 清 pending（契约 03）
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 1, event: 'StartSucceeded' })
      await vi.advanceTimersByTimeAsync(0)
      expect(pending['source:1:start']).toBe(false)
    })

    it('HTTP 失败清 pending 并返回 false', async () => {
      vi.mocked(marketSourcesApi.start).mockRejectedValue(new Error('start failed'))
      const store = useProcessStore()
      const result = await store.start(1, 'dzmd_ctp')
      expect(result).toBe(false)
      expect(usePending().pending['source:1:start']).toBe(false)
    })

    it('超时兜底（30s）清 pending 并返回 false', async () => {
      vi.mocked(marketSourcesApi.start).mockImplementation(() => new Promise(() => {}))
      const store = useProcessStore()
      const p = store.start(1, 'dzmd_ctp')
      expect(usePending().pending['source:1:start']).toBe(true)
      await vi.advanceTimersByTimeAsync(30_001)
      expect(usePending().pending['source:1:start']).toBe(false)
      const result = await p
      expect(result).toBe(false)
    })

    it('同 key 已 pending 时防重入：不重复发 HTTP 并返回 false', async () => {
      vi.mocked(marketSourcesApi.start).mockImplementation(() => new Promise(() => {}))
      const store = useProcessStore()
      const p1 = store.start(1, 'dzmd_ctp')
      const p2 = store.start(1, 'dzmd_ctp')
      await vi.advanceTimersByTimeAsync(0)  // 冲刷微任务，让 p1 的 HTTP 调用发出
      expect(marketSourcesApi.start).toHaveBeenCalledTimes(1)
      expect(await p2).toBe(false)
      // 收尾：推进 30s 让 p1 走超时兜底 settle，不残留挂起 promise/timer
      await vi.advanceTimersByTimeAsync(30_001)
      expect(await p1).toBe(false)
      expect(usePending().pending['source:1:start']).toBe(false)
    })
  })

  describe('stop', () => {
    it('HTTP 成功不清 pending，RTN Stop resolve 清除', async () => {
      vi.mocked(marketSourcesApi.stop).mockResolvedValue({ ok: true })
      const store = useProcessStore()
      const result = await store.stop(1, 'dzmd_ctp')
      expect(result).toBe(true)
      const { pending } = usePending()
      expect(pending['source:1:stop']).toBe(true)
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Stopping', pid: 1, event: 'StopSucceeded' })
      await vi.advanceTimersByTimeAsync(0)
      expect(pending['source:1:stop']).toBe(false)
    })

    it('HTTP 失败清 pending 并返回 false', async () => {
      vi.mocked(marketSourcesApi.stop).mockRejectedValue(new Error('stop failed'))
      const store = useProcessStore()
      expect(await store.stop(1, 'dzmd_ctp')).toBe(false)
      expect(usePending().pending['source:1:stop']).toBe(false)
    })
  })

  describe('removeSource', () => {
    it('HTTP 成功不清 pending，RTN Remove resolve 清除', async () => {
      vi.mocked(marketSourcesApi.remove).mockResolvedValue({ ok: true, id: 1 })
      const store = useProcessStore()
      const result = await store.removeSource(1, 'dzmd_ctp')
      expect(result).toBe(true)
      const { pending } = usePending()
      expect(pending['source:1:remove']).toBe(true)
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Stopped', pid: 0, event: 'RemoveSucceeded' })
      await vi.advanceTimersByTimeAsync(0)
      expect(pending['source:1:remove']).toBe(false)
    })

    it('HTTP 失败清 pending 并返回 false', async () => {
      vi.mocked(marketSourcesApi.remove).mockRejectedValue(new Error('remove failed'))
      const store = useProcessStore()
      expect(await store.removeSource(1, 'dzmd_ctp')).toBe(false)
      expect(usePending().pending['source:1:remove']).toBe(false)
    })
  })

  describe('applyProcessStatus event 处理（契约 03）', () => {
    it('失败路径（event=*Failed）fail 清 pending, 不弹 toast（反馈由 NOTIFY_UI 弹窗承载）', async () => {
      vi.mocked(marketSourcesApi.start).mockResolvedValue({ ok: true, source: 'dzmd_ctp' })
      const store = useProcessStore()
      await store.start(1, 'dzmd_ctp')
      expect(usePending().pending['source:1:start']).toBe(true)

      const toast = useToastStore()
      const errorSpy = vi.spyOn(toast, 'error')
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Crashed', pid: 0, message: 'process start failed', event: 'StartFailed' })
      await vi.advanceTimersByTimeAsync(0)
      expect(usePending().pending['source:1:start']).toBe(false)
      expect(errorSpy).not.toHaveBeenCalled()  // 契约 03: 失败弹窗由 NOTIFY_UI 帧承载
    })

    it('event 缺失（自发状态变化）不清 pending', async () => {
      vi.mocked(marketSourcesApi.start).mockResolvedValue({ ok: true, source: 'dzmd_ctp' })
      const store = useProcessStore()
      await store.start(1, 'dzmd_ctp')
      expect(usePending().pending['source:1:start']).toBe(true)
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Crashed', pid: 0, message: 'exit_code=1' })
      await vi.advanceTimersByTimeAsync(0)
      expect(usePending().pending['source:1:start']).toBe(true)  // 自发变化不清 pending
    })

    it('未知 event 忽略（不抛不写）', async () => {
      const store = useProcessStore()
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 1, event: 'Unknown' as never })
      expect(Object.keys(usePending().pending)).toHaveLength(0)
    })

    it('未经过本 store 的 target 忽略（无 name→id 记录，pending 不存在）', async () => {
      const store = useProcessStore()
      store.applyProcessStatus({ name: 'never-ran', state: 'Running', pid: 1, event: 'StartSucceeded' })
      expect(Object.keys(usePending().pending)).toHaveLength(0)
    })

    // 回归（Remove 流程）：configs 已无该进程（RTN_PROCESS_CONFIG 先到）时，
    // RemoveSucceeded 仍必须清 remove pending（镜像不重建，副作用保留）
    it('已移除进程的 RemoveSucceeded 清 pending 但不重建镜像', async () => {
      vi.mocked(marketSourcesApi.remove).mockResolvedValue({ ok: true, id: 1 })
      const store = useProcessStore()
      store.applyProcessConfig({ dzmd_ctp: { a: 1 } })
      await store.removeSource(1, 'dzmd_ctp')
      expect(usePending().pending['source:1:remove']).toBe(true)
      // 模拟 Remove 帧序：先 118（条目消失）再 116（RemoveSucceeded）
      store.applyProcessConfig({})
      store.applyProcessStatus({ name: 'dzmd_ctp', state: 'Stopping', pid: 1234, event: 'RemoveSucceeded' })
      await vi.advanceTimersByTimeAsync(0)
      expect(usePending().pending['source:1:remove']).toBe(false)  // pending 已清
      expect(store.statuses['dzmd_ctp']).toBeUndefined()           // 镜像不重建
    })
  })
})
