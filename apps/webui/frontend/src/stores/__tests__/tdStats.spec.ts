import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import {
  TD_STATS_POLL_INTERVAL_MS,
  __resetTdStatsSchedulerForTests,
  __setTdStatsSchedulerForTests,
  useTdStatsStore,
} from '../tdStats'
import { tdApi } from '@/api/td'
import type { TdStatsValues } from '@/types/api'

vi.mock('@/api/td', () => ({
  tdApi: { getStats: vi.fn() },
}))

/** 4 值夹具：期望值全部来自契约 rest §2.9.5 口径手算，不与被测实现互相推导 */
function values(overrides: Partial<TdStatsValues> = {}): TdStatsValues {
  return {
    total_orders: 1240,
    total_volume: 2480,
    unfilled_orders: 12,
    unfilled_volume: 21,
    ...overrides,
  }
}

/** 可用信封（契约 rest §2.9.5） */
function statsResponse(overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    stats: values(),
    ...overrides,
  }
}

/**
 * 可控轮询调度器：记录回调与延迟，由测试显式 tick。
 * 与受测常量解耦——断言轮询间隔用 spec 的字面量语义（≤1Hz）在「注入调度器承接轮询」用例里判，
 * 而不是用被测常量自证。
 */
function controllableScheduler(): {
  scheduler: { set: (cb: () => void, delayMs: number) => unknown; clear: (h: unknown) => void }
  callbacks: Array<() => void>
  delays: number[]
  cleared: unknown[]
} {
  const callbacks: Array<() => void> = []
  const delays: number[] = []
  const cleared: unknown[] = []
  return {
    callbacks,
    delays,
    cleared,
    scheduler: {
      set: (callback, delayMs) => { callbacks.push(callback); delays.push(delayMs); return callbacks.length },
      clear: (handle) => { cleared.push(handle) },
    },
  }
}

describe('useTdStatsStore · REST 基线与轮询（契约 rest §2.9.5 / webui-ws §5 第 10 条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdStatsSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdStatsSchedulerForTests()
  })

  it('init 拉一次统计并写入 4 值/交易日/在线账户', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const store = useTdStatsStore()

    await store.init()

    expect(tdApi.getStats).toHaveBeenCalledTimes(1)
    expect(tdApi.getStats).toHaveBeenCalledWith()
    expect(store.values).toEqual({
      total_orders: 1240, total_volume: 2480, unfilled_orders: 12, unfilled_volume: 21 })
    expect(store.tradingDay).toBe('20260808')
    expect(store.available).toBe(true)
    expect(store.degradedReason).toBeNull()
    expect(store.initialized).toBe(true)
    expect(store.error).toBeNull()
  })

  it('init 后按注入调度器的间隔反复轮询（≤1Hz，不依赖真实定时器）', async () => {
    const { scheduler, callbacks, delays } = controllableScheduler()
    __setTdStatsSchedulerForTests(scheduler)
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const store = useTdStatsStore()

    await store.init()
    expect(callbacks).toHaveLength(1)
    // 契约 webui-ws §5 第 10 条：与列表批量刷新同节流 ≤1Hz
    expect(delays[0]).toBe(1_000)

    vi.mocked(tdApi.getStats).mockResolvedValueOnce(statsResponse({
      stats: values({ total_orders: 1241, unfilled_orders: 13 }),
    }))
    callbacks[0]()
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getStats).toHaveBeenCalledTimes(2)
    expect(store.values?.total_orders).toBe(1241)
    expect(store.values?.unfilled_orders).toBe(13)
    // 每次轮询后重新排下一次（单句柄，不叠加）
    expect(callbacks).toHaveLength(2)
  })

  it('轮询间隔常量 = 1s（契约 webui-ws §5 第 10 条「≤1Hz」的落地值）', () => {
    expect(TD_STATS_POLL_INTERVAL_MS).toBe(1_000)
  })

  it('重叠请求：晚到的旧响应不覆盖新基线（日切后不把上一交易日写回）', async () => {
    // 第一次请求（慢）挂起；第二次请求（日切重拉）先返回。旧响应晚到必须被丢弃。
    const pending: Array<(value: unknown) => void> = []
    vi.mocked(tdApi.getStats).mockImplementation(() => new Promise((resolve) => {
      pending.push(resolve as (value: unknown) => void)
    }) as never)
    const store = useTdStatsStore()

    const slow = store.refresh() // 旧基线（20260808）
    const fresh = store.refresh() // 日切后的新基线（20260809）

    pending[1](statsResponse({
      trading_day: '20260809',
      stats: values({ total_orders: 2, total_volume: 2, unfilled_orders: 0, unfilled_volume: 0 }),
    }))
    await fresh
    expect(store.tradingDay).toBe('20260809')
    expect(store.values?.total_orders).toBe(2)

    // 旧请求晚到：不得覆盖已写入的新基线
    pending[0](statsResponse())
    await slow

    expect(store.tradingDay).toBe('20260809')
    expect(store.values?.total_orders).toBe(2)
  })

  it('dispose 停止轮询（卸载页面后不再请求）', async () => {
    const { scheduler, callbacks, cleared } = controllableScheduler()
    __setTdStatsSchedulerForTests(scheduler)
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const store = useTdStatsStore()

    await store.init()
    expect(callbacks).toHaveLength(1)

    store.dispose()

    expect(cleared).toHaveLength(1)
    callbacks[0]()
    await vi.advanceTimersByTimeAsync(0)
    expect(tdApi.getStats).toHaveBeenCalledTimes(1)
  })

  it('dispose 后重新挂载（离开页面再进入）重新拉基线并恢复轮询', async () => {
    const { scheduler, callbacks } = controllableScheduler()
    __setTdStatsSchedulerForTests(scheduler)
    vi.mocked(tdApi.getStats).mockResolvedValueOnce(statsResponse())
    const store = useTdStatsStore()
    await store.init()
    store.dispose()

    vi.mocked(tdApi.getStats).mockResolvedValueOnce(statsResponse({
      trading_day: '20260901',
      stats: values({ total_orders: 5, total_volume: 5, unfilled_orders: 0, unfilled_volume: 0 }),
    }))
    await store.init()
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getStats).toHaveBeenCalledTimes(2)
    expect(store.tradingDay).toBe('20260901')
    expect(store.values?.total_orders).toBe(5)
    // 两次 init 各排一次轮询（dispose 只清句柄，调度器记录仍保留）
    expect(callbacks).toHaveLength(2)
  })

  it('请求在途时卸载：丢弃晚到的响应（不在无视图时写状态）', async () => {    let resolveRequest: (value: unknown) => void = () => {}
    vi.mocked(tdApi.getStats).mockReturnValue(new Promise((resolve) => {
      resolveRequest = resolve as (value: unknown) => void
    }) as never)
    const store = useTdStatsStore()

    const pending = store.init()
    store.dispose()
    resolveRequest(statsResponse())
    await pending

    expect(store.values).toBeNull()
    expect(store.tradingDay).toBeNull()
  })
})

describe('useTdStatsStore · 降级（契约 rest §2.9.5 / webui-ws §5 第 13 条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdStatsSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdStatsSchedulerForTests()
  })

  it('available:false → 降级态 + reason=not_ready，且 4 个值不清零（保留旧值）', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const store = useTdStatsStore()
    await store.init()

    vi.mocked(tdApi.getStats).mockResolvedValueOnce({ available: false, reason: 'not_ready' })
    await store.refresh()

    expect(store.available).toBe(false)
    expect(store.degradedReason).toBe('not_ready')
    // 关键语义：降级 = 计数器未就绪，不是「当日 0 委托」
    expect(store.values).toEqual({
      total_orders: 1240, total_volume: 2480, unfilled_orders: 12, unfilled_volume: 21 })
    expect(store.unfilledText).toBe('12')
    expect(store.totalText).toBe('1240')
    expect(store.degradedText).toBe('统计暂不可用，正在自动重试')
  })

  it('首拉即降级时没有旧值：values 保持 null（组件显示占位而不是 0）', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue({ available: false, reason: 'not_ready' })
    const store = useTdStatsStore()

    await store.init()

    expect(store.available).toBe(false)
    expect(store.degradedReason).toBe('not_ready')
    expect(store.values).toBeNull()
    expect(store.unfilledText).toBe('—')
    expect(store.totalText).toBe('—')
  })

  it('降级期间继续轮询，恢复后回到可用态并清除降级文案', async () => {
    const { scheduler, callbacks } = controllableScheduler()
    __setTdStatsSchedulerForTests(scheduler)
    vi.mocked(tdApi.getStats).mockResolvedValue({ available: false, reason: 'not_ready' })
    const store = useTdStatsStore()
    await store.init()
    expect(store.available).toBe(false)
    expect(callbacks).toHaveLength(1)

    vi.mocked(tdApi.getStats).mockResolvedValueOnce(statsResponse())
    callbacks[0]()
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getStats).toHaveBeenCalledTimes(2)
    expect(store.available).toBe(true)
    expect(store.degradedReason).toBeNull()
    expect(store.degradedText).toBe('')
    expect(store.values?.total_orders).toBe(1240)
  })

  it('HTTP 失败保留旧值与旧 available，只记 error（不清零、不编造降级原因）', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const store = useTdStatsStore()
    await store.init()

    vi.mocked(tdApi.getStats).mockRejectedValueOnce(new Error('HTTP 500'))
    await store.refresh()

    expect(store.error).toBe('HTTP 500')
    expect(store.available).toBe(true)
    expect(store.degradedReason).toBeNull()
    expect(store.values?.total_orders).toBe(1240)
    expect(store.values?.unfilled_volume).toBe(21)
  })
})

describe('useTdStatsStore · 重置与口径（契约 webui-ws §5 第 10 条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdStatsSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdStatsSchedulerForTests()
  })

  it('单位切换：笔数 ↔ 手数（纯展示，不发请求）', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const store = useTdStatsStore()
    await store.init()
    expect(store.unit).toBe('orders')
    expect(store.unitLabel).toBe('笔')
    expect(store.unfilledText).toBe('12')
    expect(store.totalText).toBe('1240')

    store.setUnit('volume')

    expect(store.unit).toBe('volume')
    expect(store.unitLabel).toBe('手')
    expect(store.unfilledText).toBe('21')
    expect(store.totalText).toBe('2480')
    expect(tdApi.getStats).toHaveBeenCalledTimes(1) // 切换口径不触发请求

    store.setUnit('orders')
    expect(store.unfilledText).toBe('12')
    expect(store.totalText).toBe('1240')
  })

  it('handleDataChanged：清空旧值并立即重拉（不把失效基线留在屏幕）', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const store = useTdStatsStore()
    await store.init()

    // 重拉在途时读取 store.values：旧值必须已被清空（不能把上一基线的数字留在屏幕）
    let clearedWhileRefreshing: TdStatsValues | null | undefined
    vi.mocked(tdApi.getStats).mockImplementationOnce(async () => {
      clearedWhileRefreshing = store.values
      return statsResponse({
        trading_day: '20260809',
        stats: values({ total_orders: 3, total_volume: 6, unfilled_orders: 1, unfilled_volume: 2 }),
      })
    })

    await store.handleDataChanged()

    expect(clearedWhileRefreshing).toBeNull() // 重拉发起前旧值已清空
    expect(tdApi.getStats).toHaveBeenCalledTimes(2)
    expect(store.tradingDay).toBe('20260809')
    expect(store.values).toEqual({
      total_orders: 3, total_volume: 6, unfilled_orders: 1, unfilled_volume: 2 })
  })

  it('日切：trading_day 变化时整对象替换，不混入上一交易日数值', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const store = useTdStatsStore()
    await store.init()
    expect(store.tradingDay).toBe('20260808')

    vi.mocked(tdApi.getStats).mockResolvedValueOnce(statsResponse({
      trading_day: '20260809',
      stats: values({ total_orders: 2, total_volume: 4, unfilled_orders: 0, unfilled_volume: 0 }),
    }))
    await store.refresh()

    expect(store.tradingDay).toBe('20260809')
    expect(store.values).toEqual({
      total_orders: 2, total_volume: 4, unfilled_orders: 0, unfilled_volume: 0 })
  })

  it('无在线账户（trading_day:null）：available 仍为 true，值是当日全 0', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse({
      trading_day: null,
      online_accounts: [],
      stats: values({ total_orders: 0, total_volume: 0, unfilled_orders: 0, unfilled_volume: 0 }),
    }))
    const store = useTdStatsStore()

    await store.init()

    expect(store.available).toBe(true)
    expect(store.tradingDay).toBeNull()
    expect(store.unfilledText).toBe('0')
    expect(store.totalText).toBe('0')
  })

  it('未初始化时 handleDataChanged 不产生请求（页面未打开不该有流量）', async () => {
    const store = useTdStatsStore()

    await store.handleDataChanged()

    expect(tdApi.getStats).not.toHaveBeenCalled()
  })
})
