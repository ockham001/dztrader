import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import {
  TD_BATCH_INTERVAL_MS,
  TD_RETRY_BASE_DELAY_MS,
  TD_TRADES_PAGE_SIZE,
  TD_TRADES_WINDOW_CAPACITY,
  __resetTdBatchSchedulerForTests,
  __setTdBatchSchedulerForTests,
  useTdTradesStore,
} from '../tdTrades'
import { tdApi } from '@/api/td'
import type { TdTradeRow } from '@/types/api'

vi.mock('@/api/td', () => ({
  tdApi: { getTrades: vi.fn() },
}))

/** 构造成交行（默认 sort_key 与 trade_id 对齐，便于断言顺序） */
function trade(overrides: Partial<TdTradeRow> = {}): TdTradeRow {
  const tradeId = overrides.trade_id ?? 't1'
  return {
    account_id: 'acc_a',
    date: '20260808',
    trade_id: tradeId,
    order_id: 10231,
    strategy_id: 'stg_demo',
    instrument_id: 'rb2601',
    exchange_id: 'SHFE',
    direction: 'Long',
    position_effect: 'Open',
    price: 3512.3,
    volume: 1,
    time: 32405,
    price_decimals: 1,
    // 默认与 trade_id 无关的稳定 seq：只有显式覆盖的用例才参与「seq 大者胜 / seq 回退」判定
    seq: 1,
    sort_key: `k-${tradeId}`,
    ...overrides,
  }
}

/** 可用信封（契约 rest §2.9.2，成交与委托同形） */
function listResponse(items: TdTradeRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
    ...overrides,
  }
}

describe('useTdTradesStore · REST 基线（契约 rest §2.9.2）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdBatchSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdBatchSchedulerForTests()
  })

  it('init 用成交端点拉首页并写入行/交易日/在线账户（首屏 200 行）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([trade({ trade_id: 't2' }), trade({ trade_id: 't1' })]))
    const store = useTdTradesStore()

    await store.init()

    expect(tdApi.getTrades).toHaveBeenCalledWith({ limit: TD_TRADES_PAGE_SIZE, accounts: undefined })
    expect(store.rows.map(r => r.trade_id)).toEqual(['t2', 't1'])
    expect(store.tradingDay).toBe('20260808')
    expect(store.onlineAccounts).toEqual(['acc_a'])
    expect(store.available).toBe(true)
    // 有行 = 非空态（空态判据只看是否成功读到基线且无行）
    expect(store.isEmpty).toBe(false)
  })

  it('REST 行按 sort_key 降序维护（服务端顺序不依赖）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(
      listResponse([trade({ trade_id: 't1', sort_key: 'a' }), trade({ trade_id: 't2', sort_key: 'c' })]))
    const store = useTdTradesStore()

    await store.init()

    expect(store.rows.map(r => r.sort_key)).toEqual(['c', 'a'])
  })

  it('请求失败记错误、不冒充「无数据」空态', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([trade({ trade_id: 't1' })]))
    const store = useTdTradesStore()
    await store.init()

    vi.mocked(tdApi.getTrades).mockRejectedValue(new Error('HTTP 500'))
    await store.resetAndReload()

    expect(store.error).toBe('HTTP 500')
    expect(store.isEmpty).toBe(false)
  })
})

describe('useTdTradesStore · 分页与有界窗口', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdBatchSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdBatchSchedulerForTests()
  })

  it('游标分页：接近底部时按 next_cursor 拉下一页且不重复', async () => {
    const page1 = Array.from({ length: TD_TRADES_PAGE_SIZE }, (_, i) =>
      trade({ trade_id: `t${1000 - i}`, sort_key: `k-${String(1000 - i).padStart(6, '0')}` }))
    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(listResponse(page1, { next_cursor: 'cursor-1' }))
    const store = useTdTradesStore()
    await store.init()

    store.prefetchIfNeeded(10)
    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)

    const page2 = Array.from({ length: 10 }, (_, i) =>
      trade({ trade_id: `t${500 - i}`, sort_key: `k-${String(500 - i).padStart(6, '0')}` }))
    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(listResponse(page2, { next_cursor: null }))
    store.prefetchIfNeeded(TD_TRADES_PAGE_SIZE - 1)
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getTrades).toHaveBeenLastCalledWith(
      { limit: TD_TRADES_PAGE_SIZE, cursor: 'cursor-1', accounts: undefined })
    expect(store.rows).toHaveLength(TD_TRADES_PAGE_SIZE + 10)
    expect(new Set(store.rows.map(r => r.trade_id)).size).toBe(store.rows.length)
    expect(store.nextCursor).toBeNull()
  })

  it('有界窗口：WS 增量超出容量时丢弃最旧行并标记 windowTruncated', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([]))
    const store = useTdTradesStore()
    await store.init()

    for (let i = 1; i <= TD_TRADES_WINDOW_CAPACITY + 50; i++) {
      store.applyTradeEvent(trade({ trade_id: `t${i}`, sort_key: `k-${String(i).padStart(6, '0')}` }))
    }
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows).toHaveLength(TD_TRADES_WINDOW_CAPACITY)
    expect(store.rows[0].trade_id).toBe(`t${TD_TRADES_WINDOW_CAPACITY + 50}`)
    expect(store.rows.some(r => r.trade_id === 't1')).toBe(false)
    expect(store.windowTruncated).toBe(true)
  })
})

describe('useTdTradesStore · WS 合并（契约 webui-ws §5 第 8/9 条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdBatchSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdBatchSchedulerForTests()
  })

  async function storeWithBaseline(items: TdTradeRow[] = []): Promise<ReturnType<typeof useTdTradesStore>> {
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse(items))
    const store = useTdTradesStore()
    await store.init()
    return store
  }

  it('约 10Hz 批量合并：同批事件只在定时器到点后一次性写入', async () => {
    const store = await storeWithBaseline()

    store.applyTradeEvent(trade({ trade_id: 't1' }))
    store.applyTradeEvent(trade({ trade_id: 't2' }))
    store.applyTradeEvent(trade({ trade_id: 't3' }))
    expect(store.rows).toHaveLength(0)

    await vi.advanceTimersByTimeAsync(100)

    expect(store.rows.map(r => r.trade_id)).toEqual(['t3', 't2', 't1'])
  })

  it('业务键 (account,date,exchange,trade_id) 去重 + seq 大者胜（乱序无害）', async () => {
    const store = await storeWithBaseline()

    store.applyTradeEvent(trade({ trade_id: 't7', seq: 5, volume: 1 }))
    store.applyTradeEvent(trade({ trade_id: 't7', seq: 9, volume: 3 }))
    store.applyTradeEvent(trade({ trade_id: 't7', seq: 7, volume: 2 }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows).toHaveLength(1)
    expect(store.rows[0].seq).toBe(9)
    expect(store.rows[0].volume).toBe(3)
  })

  it('多交易所同 trade_id 是两条成交（不动点：不按 trade_id 单独去重）', async () => {
    const store = await storeWithBaseline()

    store.applyTradeEvent(trade({ trade_id: '1', exchange_id: 'SHFE', sort_key: 'k1' }))
    store.applyTradeEvent(trade({ trade_id: '1', exchange_id: 'CZCE', sort_key: 'k2' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows).toHaveLength(2)
    expect(store.rows.map(r => r.exchange_id)).toEqual(['CZCE', 'SHFE'])
  })

  it('增量行按 sort_key 插入正确位置（不是简单前插）', async () => {
    const store = await storeWithBaseline([
      trade({ trade_id: 't10', sort_key: 'b' }),
      trade({ trade_id: 't5', sort_key: 'a' }),
    ])

    store.applyTradeEvent(trade({ trade_id: 't8', sort_key: 'a5' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows.map(r => r.sort_key)).toEqual(['b', 'a5', 'a'])
  })

  it('「N 条新数据」气泡：不在顶部时新行只计数，回顶部清零', async () => {
    const store = await storeWithBaseline()
    store.setAtTop(false)

    store.applyTradeEvent(trade({ trade_id: 't1', sort_key: 'k1' }))
    store.applyTradeEvent(trade({ trade_id: 't2', sort_key: 'k2' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    expect(store.newCount).toBe(2)

    // 同业务键的更新不算新数据
    store.applyTradeEvent(trade({ trade_id: 't2', seq: 99, sort_key: 'k2' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    expect(store.newCount).toBe(2)

    store.setAtTop(true)
    expect(store.newCount).toBe(0)
  })

  it('内容相同的两笔成交只要业务键不同就都保留（不按内容去重）', async () => {
    const store = await storeWithBaseline()

    store.applyTradeEvent(trade({ trade_id: 't1', sort_key: 'k1' }))
    // 除 trade_id/sort_key 外字段完全一致：仍是两条独立成交
    store.applyTradeEvent(trade({ trade_id: 't2', sort_key: 'k2' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows).toHaveLength(2)
  })
})

describe('useTdTradesStore · 重置触发（日切 / data_changed / 重连 / seq 回退）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdBatchSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdBatchSchedulerForTests()
  })

  async function initialized(): Promise<ReturnType<typeof useTdTradesStore>> {
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([trade({ trade_id: 't1', seq: 10 })]))
    const store = useTdTradesStore()
    await store.init()
    return store
  }

  it('data_changed{scope:"td"} → 清空 + 丢游标从首页重拉', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(listResponse([trade({ trade_id: 't2', seq: 20 })]))

    await store.handleDataChanged('td')
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getTrades).toHaveBeenCalledTimes(2)
    expect(tdApi.getTrades).toHaveBeenLastCalledWith({ limit: TD_TRADES_PAGE_SIZE, accounts: undefined })
    expect(store.rows.map(r => r.trade_id)).toEqual(['t2'])
  })

  it('非 td scope 不触发成交列表重拉', async () => {
    const store = await initialized()
    await store.handleDataChanged('market_sources')
    await vi.advanceTimersByTimeAsync(0)
    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)
  })

  it('日切：收到新交易日的成交 → 清空重拉（不混入上一交易日数据）', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(
      listResponse([trade({ trade_id: 't9', seq: 30 })], { trading_day: '20260809' }))

    store.applyTradeEvent(trade({ trade_id: 't50', seq: 31, date: '20260809', sort_key: 'k50' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(tdApi.getTrades).toHaveBeenCalledTimes(2)
    expect(store.tradingDay).toBe('20260809')
    expect(store.rows.map(r => r.trade_id)).toEqual(['t9'])
  })

  it('上一交易日的迟到成交被忽略', async () => {
    const store = await initialized()
    store.applyTradeEvent(trade({ trade_id: 't50', seq: 31, date: '20260807', sort_key: 'k50' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows.map(r => r.trade_id)).toEqual(['t1'])
    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)
  })

  it('同账户 seq 回退视为数据重置：清空重拉', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(
      listResponse([trade({ trade_id: 't3', seq: 2 })]))

    store.applyTradeEvent(trade({ trade_id: 't3', seq: 2 }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(tdApi.getTrades).toHaveBeenCalledTimes(2)
    expect(store.rows.map(r => r.trade_id)).toEqual(['t3'])
  })

  it('td_recent 到达（重连信号）→ 认领成交事件并入 + 重拉基线', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(
      listResponse([
        trade({ trade_id: 't5', seq: 90, sort_key: 'k5' }),
        trade({ trade_id: 't6', seq: 60, sort_key: 'k6' }),
      ]))

    store.applyRecent({
      events: [
        // 补发环里的 t5 是旧版本（seq 30），基线已有新版本（seq 90）→ 不得写回
        { type: 'td_trade_report', data: trade({ trade_id: 't5', seq: 30, sort_key: 'k5' }) },
        // 补发有、基线首页没有的行要保留
        { type: 'td_trade_report', data: trade({ trade_id: 't7', seq: 70, sort_key: 'k7' }) },
        // 委托事件不属成交 store，忽略
        { type: 'td_order_report', data: { order_id: 1 } },
      ],
    })
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getTrades).toHaveBeenCalledTimes(2)
    const byId = new Map(store.rows.map(r => [r.trade_id, r]))
    expect(byId.get('t5')?.seq).toBe(90)
    expect(byId.has('t7')).toBe(true)
    expect(store.rows.map(r => r.trade_id)).toEqual(['t7', 't6', 't5'])
  })

  it('未初始化时 td_recent / data_changed 不发起请求（页面未打开不产生基线流量）', async () => {
    const store = useTdTradesStore()
    store.applyRecent({ events: [{ type: 'td_trade_report', data: trade() }] })
    await store.handleDataChanged('td')
    await vi.advanceTimersByTimeAsync(0)
    expect(tdApi.getTrades).not.toHaveBeenCalled()
  })

  it('账户筛选变化：重置游标从首页重拉并带 accounts 参数', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(
      listResponse([trade({ trade_id: 't1' })], { online_accounts: ['acc_a', 'acc_b'], next_cursor: 'c1' }))
    const store = useTdTradesStore()
    await store.init()

    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(
      listResponse([trade({ trade_id: 't2' })], { online_accounts: ['acc_a', 'acc_b'] }))
    await store.setAccountFilter(['acc_b'])

    expect(tdApi.getTrades).toHaveBeenLastCalledWith(
      { limit: TD_TRADES_PAGE_SIZE, accounts: ['acc_b'] })
    expect(store.allAccountsSelected).toBe(false)
    expect(store.nextCursor).toBeNull()
  })

  it('筛选到空集合时按缺省语义请求全部（契约 rest §2.9.2：空 = 全部在线账户）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(
      listResponse([trade({ trade_id: 't1' })], { online_accounts: ['acc_a', 'acc_b'] }))
    const store = useTdTradesStore()
    await store.init()

    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(
      listResponse([trade({ trade_id: 't2' })], { online_accounts: ['acc_a', 'acc_b'] }))
    await store.selectAllAccounts()
    await store.setAccountFilter([])

    expect(tdApi.getTrades).toHaveBeenLastCalledWith({ limit: TD_TRADES_PAGE_SIZE, accounts: undefined })
    expect(store.allAccountsSelected).toBe(true)
  })
})

describe('useTdTradesStore · 降级（契约 rest §2.9.8 / webui-ws §5 第 13 条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdBatchSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdBatchSchedulerForTests()
  })

  it('available:false → 降级态 + 原因，不显示 0 冒充数据', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue({ available: false, reason: 'not_found' })
    const store = useTdTradesStore()

    await store.init()

    expect(store.available).toBe(false)
    expect(store.degradedReason).toBe('not_found')
    expect(store.rows).toEqual([])
    expect(store.isEmpty).toBe(false)
    expect(store.degradedText).toContain('交易数据')
  })

  it('降级后按退避重试，恢复后回到可用态', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdTradesStore()
    await store.init()
    expect(store.available).toBe(false)

    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([trade({ trade_id: 't1' })]))
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)

    expect(store.available).toBe(true)
    expect(store.degradedReason).toBeNull()
    expect(store.rows).toHaveLength(1)
    expect(store.degradedText).toBe('')
  })

  it('降级期间 WS 事件照常合并（不丢增量）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdTradesStore()
    await store.init()

    store.applyTradeEvent(trade({ trade_id: 't42' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows.map(r => r.trade_id)).toEqual(['t42'])
    expect(store.available).toBe(false)
  })

  it('卸载再挂载（dispose → init）后重新武装退避并从首档重试', async () => {
    // 首轮挂载即降级：重试一次后 attempt 升到 1，下一档退避 = 2×base
    vi.mocked(tdApi.getTrades).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdTradesStore()
    await store.init()
    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)
    expect(tdApi.getTrades).toHaveBeenCalledTimes(2)
    // 第二档是 2×base：只推进 base 不应再触发（证明 attempt 确实已递增）
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)
    expect(tdApi.getTrades).toHaveBeenCalledTimes(2)

    // 卸载再挂载：新生命周期必须重新武装定时器，并把退避档位复位到首档
    store.dispose()
    await store.init()
    expect(tdApi.getTrades).toHaveBeenCalledTimes(3)
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([trade({ trade_id: 't2' })]))
    // 若沿用卸载前的档位（2×base），此处只推进 base 不会重试
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)

    expect(tdApi.getTrades).toHaveBeenCalledTimes(4)
    expect(store.available).toBe(true)
    expect(store.rows.map(r => r.trade_id)).toEqual(['t2'])
  })

  it('dispose 清空定时器（卸载页面后不再请求）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdTradesStore()
    await store.init()

    store.dispose()
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS * 10)

    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)
  })
})

describe('useTdTradesStore · 批量定时器可注入', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdBatchSchedulerForTests()
  })

  it('注入的调度器承接批量刷新（与委托共用同一注入点）', async () => {
    const callbacks: Array<() => void> = []
    __setTdBatchSchedulerForTests({
      set: (callback) => { callbacks.push(callback); return callbacks.length },
      clear: () => {},
    })
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([]))
    const store = useTdTradesStore()
    await store.init()

    store.applyTradeEvent(trade({ trade_id: 't1' }))
    expect(callbacks).toHaveLength(1)
    expect(store.rows).toHaveLength(0)

    callbacks[0]()
    expect(store.rows.map(r => r.trade_id)).toEqual(['t1'])
  })
})
