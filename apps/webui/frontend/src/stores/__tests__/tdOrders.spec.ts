import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { nextTick } from 'vue'
import {
  TD_BATCH_INTERVAL_MS,
  TD_ORDERS_PAGE_SIZE,
  TD_ORDERS_WINDOW_CAPACITY,
  TD_RETRY_BASE_DELAY_MS,
  __resetTdBatchSchedulerForTests,
  __setTdBatchSchedulerForTests,
  useTdOrdersStore,
} from '../tdOrders'
import { tdApi } from '@/api/td'
import type { TdOrderRow } from '@/types/api'

vi.mock('@/api/td', () => ({
  tdApi: { getOrders: vi.fn() },
}))

/** 构造委托行（默认给可与 order_id 对齐的 sort_key，便于断言顺序） */
function row(overrides: Partial<TdOrderRow> = {}): TdOrderRow {
  const orderId = overrides.order_id ?? 1
  return {
    account_id: 'acc_a',
    date: '20260808',
    order_id: orderId,
    strategy_id: 'stg_demo',
    instrument_id: 'rb2601',
    exchange_id: 'SHFE',
    direction: 'Long',
    position_effect: 'Open',
    price_type: 'Limit',
    status: 'PartTraded',
    price: 3512.5,
    volume: 2,
    volume_traded: 1,
    remark: '',
    time: 32400,
    avg_price: 3512.3,
    price_decimals: 1,
    avg_price_decimals: 3,
    seq: orderId,
    sort_key: `k-${String(orderId).padStart(6, '0')}`,
    ...overrides,
  }
}

/** 可用信封（契约 rest §2.9.2） */
function listResponse(items: TdOrderRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
    ...overrides,
  }
}

describe('useTdOrdersStore · REST 基线', () => {
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

  it('init 拉首页并写入行/交易日/在线账户（首屏 200 行）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([row({ order_id: 2 }), row({ order_id: 1 })]))
    const store = useTdOrdersStore()

    await store.init()

    expect(tdApi.getOrders).toHaveBeenCalledWith({ limit: TD_ORDERS_PAGE_SIZE, accounts: undefined })
    expect(store.rows.map(r => r.order_id)).toEqual([2, 1])
    expect(store.tradingDay).toBe('20260808')
    expect(store.onlineAccounts).toEqual(['acc_a'])
    expect(store.available).toBe(true)
    expect(store.degradedReason).toBeNull()
    expect(store.loading).toBe(false)
  })

  it('账户筛选默认全选（选项来自信封 online_accounts）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(
      listResponse([], { online_accounts: ['acc_b', 'acc_a'] }))
    const store = useTdOrdersStore()

    await store.init()

    expect([...store.accountFilter].sort()).toEqual(['acc_a', 'acc_b'])
    expect(store.allAccountsSelected).toBe(true)
  })

  it('REST 行按 sort_key 降序维护（服务端顺序不依赖）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(
      listResponse([row({ order_id: 1, sort_key: 'a' }), row({ order_id: 2, sort_key: 'c' })]))
    const store = useTdOrdersStore()

    await store.init()

    expect(store.rows.map(r => r.sort_key)).toEqual(['c', 'a'])
  })

  it('首页 items 空且 trading_day 为 null = 无数据空态（不是错误、不降级）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(
      listResponse([], { trading_day: null, online_accounts: [] }))
    const store = useTdOrdersStore()

    await store.init()

    expect(store.rows).toEqual([])
    expect(store.tradingDay).toBeNull()
    expect(store.available).toBe(true)
    expect(store.isEmpty).toBe(true)
  })

  it('请求失败不冒充「无数据」空态（记错误，isEmpty 为 false）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([row({ order_id: 1 })]))
    const store = useTdOrdersStore()
    await store.init()

    vi.mocked(tdApi.getOrders).mockRejectedValue(new Error('HTTP 500'))
    await store.resetAndReload()

    expect(store.error).toBe('HTTP 500')
    expect(store.isEmpty).toBe(false)
  })
})

describe('useTdOrdersStore · 有界窗口与分页', () => {
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

  it('游标分页：prefetch 接近底部时按 next_cursor 拉下一页且不重复', async () => {
    const page1 = Array.from({ length: TD_ORDERS_PAGE_SIZE }, (_, i) =>
      row({ order_id: 1000 - i, sort_key: `k-${String(1000 - i).padStart(6, '0')}` }))
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(listResponse(page1, { next_cursor: 'cursor-1' }))
    const store = useTdOrdersStore()
    await store.init()

    // 可见区末尾离列表底部还远：不预取
    store.prefetchIfNeeded(10)
    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)

    // 可见区末尾接近列表底部：按游标预取下一页
    const page2 = Array.from({ length: 10 }, (_, i) =>
      row({ order_id: 500 - i, sort_key: `k-${String(500 - i).padStart(6, '0')}` }))
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(listResponse(page2, { next_cursor: null }))
    store.prefetchIfNeeded(TD_ORDERS_PAGE_SIZE - 1)
    await nextTick()
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getOrders).toHaveBeenLastCalledWith(
      { limit: TD_ORDERS_PAGE_SIZE, cursor: 'cursor-1', accounts: undefined })
    expect(store.rows).toHaveLength(TD_ORDERS_PAGE_SIZE + 10)
    expect(new Set(store.rows.map(r => r.order_id)).size).toBe(store.rows.length)
    expect(store.nextCursor).toBeNull()
  })

  it('next_cursor 为 null 时不再预取', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(listResponse([row({ order_id: 1 })]))
    const store = useTdOrdersStore()
    await store.init()

    store.prefetchIfNeeded(0)
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
  })

  it('有界窗口 2000 行：WS 增量超出容量时丢弃最旧行', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([]))
    const store = useTdOrdersStore()
    await store.init()

    for (let i = 1; i <= TD_ORDERS_WINDOW_CAPACITY + 50; i++) {
      store.applyOrderEvent(row({ order_id: i }))
    }
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows).toHaveLength(TD_ORDERS_WINDOW_CAPACITY)
    // 丢的是最旧（sort_key 最小）的 50 行
    expect(store.rows[0].order_id).toBe(TD_ORDERS_WINDOW_CAPACITY + 50)
    expect(store.rows.some(r => r.order_id <= 50)).toBe(false)
    expect(store.windowTruncated).toBe(true)
  })

  it('窗口淘汰后向上滚动从首页重拉基线', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([]))
    const store = useTdOrdersStore()
    await store.init()
    for (let i = 1; i <= TD_ORDERS_WINDOW_CAPACITY + 1; i++) store.applyOrderEvent(row({ order_id: i }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    expect(store.windowTruncated).toBe(true)

    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 99999, sort_key: 'k-99999' })], { next_cursor: 'next' }))
    store.prefetchNearTop(0)
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)
    expect(store.windowTruncated).toBe(false)
    expect(store.rows.map(r => r.order_id)).toEqual([99999])
  })

  it('未发生淘汰时向上滚动不重拉（避免来回滚动反复打库）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(listResponse([row({ order_id: 1 })]))
    const store = useTdOrdersStore()
    await store.init()

    store.prefetchNearTop(0)
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
  })

  it('dispose 后重新挂载（离开页面再进入）重新拉基线并恢复可用', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(listResponse([row({ order_id: 1 })]))
    const store = useTdOrdersStore()
    await store.init()
    store.dispose()

    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(listResponse([row({ order_id: 2, seq: 2 })], { trading_day: '20260901' }))
    await store.init()

    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)
    expect(store.rows.map(r => r.order_id)).toEqual([2])
    expect(store.tradingDay).toBe('20260901')
  })
})

describe('useTdOrdersStore · WS 合并（契约 webui-ws §5 第 8/9 条）', () => {
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

  async function storeWithBaseline(items: TdOrderRow[] = []): Promise<ReturnType<typeof useTdOrdersStore>> {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse(items))
    const store = useTdOrdersStore()
    await store.init()
    return store
  }

  it('约 10Hz 批量合并：同一批事件只在定时器到点后一次性写入', async () => {
    const store = await storeWithBaseline()

    store.applyOrderEvent(row({ order_id: 1 }))
    store.applyOrderEvent(row({ order_id: 2 }))
    store.applyOrderEvent(row({ order_id: 3 }))

    // 事件刚入队：列表尚未变化（避免逐条触发重渲染）
    expect(store.rows).toHaveLength(0)

    // 频率 oracle 用 spec §4.9 的字面量「约 10Hz」（= 100ms），**不**用被测常量本身：
    // 推进字面量 100ms 必须已经刷新，否则刷新频率慢于契约语义。
    await vi.advanceTimersByTimeAsync(100)

    expect(store.rows.map(r => r.order_id)).toEqual([3, 2, 1])
  })

  it('业务键 order_id 去重 + 同键 seq 大者胜（重复/乱序无害）', async () => {
    const store = await storeWithBaseline()

    store.applyOrderEvent(row({ order_id: 7, seq: 5, status: 'NotTraded', sort_key: 's7' }))
    store.applyOrderEvent(row({ order_id: 7, seq: 9, status: 'AllTraded', sort_key: 's7' }))
    store.applyOrderEvent(row({ order_id: 7, seq: 7, status: 'PartTraded', sort_key: 's7' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows).toHaveLength(1)
    expect(store.rows[0].seq).toBe(9)
    expect(store.rows[0].status).toBe('AllTraded')
  })

  it('增量行按 sort_key 插入正确位置（不是简单前插）', async () => {
    const store = await storeWithBaseline([
      row({ order_id: 10, seq: 10, sort_key: 'b' }),
      row({ order_id: 5, seq: 5, sort_key: 'a' }),
    ])

    store.applyOrderEvent(row({ order_id: 8, seq: 11, sort_key: 'a5' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows.map(r => r.sort_key)).toEqual(['b', 'a5', 'a'])
  })

  it('翻页回来的行不与 WS 新行互相覆盖：同键 seq 大者胜（契约 webui-ws §5 第 8 条）', async () => {
    // 满页基线（order 1 最新），WS 把 order 1 推进到 seq 12 / AllTraded
    const page1 = Array.from({ length: TD_ORDERS_PAGE_SIZE }, (_, i) =>
      row({
        order_id: TD_ORDERS_PAGE_SIZE - i,
        seq: TD_ORDERS_PAGE_SIZE - i,
        status: i === 0 ? 'PartTraded' : 'NotTraded',
        sort_key: `k-${String(TD_ORDERS_PAGE_SIZE - i).padStart(6, '0')}`,
      }))
    // 基线还有下一页游标（翻页场景才成立）
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(listResponse(page1, { next_cursor: 'c1' }))
    const store = useTdOrdersStore()
    await store.init()
    store.applyOrderEvent(row({ order_id: 1, seq: 12, status: 'AllTraded' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    expect(store.rows.find(r => r.order_id === 1)?.status).toBe('AllTraded')

    // 下一页里 order 1 的旧版本（seq 6）不得覆盖 WS 的新版本
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(listResponse([
      row({ order_id: 1, seq: 6, status: 'NotTraded', sort_key: 'k-000001' }),
      row({ order_id: 0, seq: 0, sort_key: 'k-000000' }),
    ], { next_cursor: 'c2' }))
    store.prefetchIfNeeded(TD_ORDERS_PAGE_SIZE - 1)
    await vi.advanceTimersByTimeAsync(0)

    const order1 = store.rows.find(r => r.order_id === 1)
    expect(order1?.seq).toBe(12)
    expect(order1?.status).toBe('AllTraded')
    expect(store.rows).toHaveLength(TD_ORDERS_PAGE_SIZE + 1)
  })

  it('「N 条新数据」气泡：用户不在顶部时新行只计数，回顶部后清零', async () => {
    const store = await storeWithBaseline()
    store.setAtTop(false)

    store.applyOrderEvent(row({ order_id: 1, seq: 10 }))
    store.applyOrderEvent(row({ order_id: 2, seq: 11 }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    expect(store.newCount).toBe(2)

    // 同一业务键的更新不算新数据
    store.applyOrderEvent(row({ order_id: 2, seq: 99 }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    expect(store.newCount).toBe(2)

    store.setAtTop(true)
    expect(store.newCount).toBe(0)

    store.setAtTop(false)
    store.applyOrderEvent(row({ order_id: 3, seq: 100 }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    expect(store.newCount).toBe(1)
    store.clearNewCount()
    expect(store.newCount).toBe(0)
  })

  it('用户在顶部时新行不累计气泡计数', async () => {
    const store = await storeWithBaseline()
    store.setAtTop(true)

    store.applyOrderEvent(row({ order_id: 1 }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.newCount).toBe(0)
  })
})

describe('useTdOrdersStore · 重置触发（日切 / data_changed / 重连 / seq 回退）', () => {
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

  async function initialized(): Promise<ReturnType<typeof useTdOrdersStore>> {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([row({ order_id: 1, seq: 10 })]))
    const store = useTdOrdersStore()
    await store.init()
    return store
  }

  it('data_changed{scope:"td"} → 清空 + 丢游标从首页重拉', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 2, seq: 20 })], { next_cursor: null }))

    await store.handleDataChanged('td')
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)
    expect(tdApi.getOrders).toHaveBeenLastCalledWith({ limit: TD_ORDERS_PAGE_SIZE, accounts: undefined })
    expect(store.rows.map(r => r.order_id)).toEqual([2])
  })

  it('非 td scope 不触发委托列表重拉', async () => {
    const store = await initialized()
    await store.handleDataChanged('market_sources')
    await vi.advanceTimersByTimeAsync(0)
    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
  })

  it('日切：收到新交易日的行 → 清空重拉（不混入上一交易日数据）', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 9, seq: 30 })], { trading_day: '20260809' }))

    store.applyOrderEvent(row({ order_id: 50, seq: 31, date: '20260809' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)
    expect(store.tradingDay).toBe('20260809')
    expect(store.rows.map(r => r.order_id)).toEqual([9])
  })

  it('上一交易日的迟到行被忽略（delta 更旧）', async () => {
    const store = await initialized()
    store.applyOrderEvent(row({ order_id: 50, seq: 31, date: '20260807' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows.map(r => r.order_id)).toEqual([1])
    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
  })

  it('同账户 seq 回退视为数据重置：清空重拉', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 3, seq: 2, sort_key: 'k-3' })]))

    store.applyOrderEvent(row({ order_id: 3, seq: 2, sort_key: 'k-3' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)
    expect(store.rows.map(r => r.order_id)).toEqual([3])
    expect(store.rows[0].seq).toBe(2)
  })

  it('td_recent 到达（重连信号）→ 清空并重拉 REST 基线', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 4, seq: 40, sort_key: 'k-4' })]))

    store.applyRecent({
      events: [
        { type: 'td_order_report', data: row({ order_id: 4, seq: 40, sort_key: 'k-4' }) },
        { type: 'td_position_info', data: {} },
      ],
    })
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)
    // 基线重置丢弃了旧窗口（order 1 不在新基线里）
    expect(store.rows.map(r => r.order_id)).toEqual([4])
  })

  it('td_recent 补发事件与基线走同一合并规则：同键 seq 大者胜，补发旧行不写回', async () => {
    const store = await initialized()
    // 补发环里的 order 5 是旧版本（seq 30），基线已有新版本（seq 90）
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([
        row({ order_id: 5, seq: 90, status: 'AllTraded' }),
        row({ order_id: 6, seq: 60 }),
      ]))

    store.applyRecent({
      events: [
        { type: 'td_order_report', data: row({ order_id: 5, seq: 30, status: 'NotTraded' }) },
        // 补发里有、基线首页没有的行（异步持久化窗口内的记录）也要保留，不能丢
        { type: 'td_order_report', data: row({ order_id: 7, seq: 70 }) },
      ],
    })
    await vi.advanceTimersByTimeAsync(0)

    const byId = new Map(store.rows.map(r => [r.order_id, r]))
    expect(byId.get(5)?.seq).toBe(90)
    expect(byId.get(5)?.status).toBe('AllTraded')
    expect(byId.has(7)).toBe(true)
    // 降序：7(seq70/sort k-000007) > 6 > 5
    expect(store.rows.map(r => r.order_id)).toEqual([7, 6, 5])
  })

  it('同一业务键的迟到事件不触发 seq 回退重置（重复/乱序无害，合并规则吸收）', async () => {
    const store = await initialized()
    // order 1 已有 seq 10；同键 seq 5 的迟到事件只应被合并规则忽略
    store.applyOrderEvent(row({ order_id: 1, seq: 5, status: 'NotTraded' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
    expect(store.rows[0].seq).toBe(10)
    expect(store.rows[0].status).toBe('PartTraded')
  })

  it('未初始化时 td_recent / data_changed 不发起请求（页面未打开不产生基线流量）', async () => {
    const store = useTdOrdersStore()
    store.applyRecent({ events: [{ type: 'td_order_report', data: row() }] })
    await store.handleDataChanged('td')
    await vi.advanceTimersByTimeAsync(0)
    expect(tdApi.getOrders).not.toHaveBeenCalled()
  })

  it('账户筛选变化：重置游标从首页重拉并带 accounts 参数', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(
      listResponse([row({ order_id: 1 })], { online_accounts: ['acc_a', 'acc_b'], next_cursor: 'c1' }))
    const store = useTdOrdersStore()
    await store.init()

    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 2 })], { online_accounts: ['acc_a', 'acc_b'] }))
    await store.setAccountFilter(['acc_b'])

    expect(tdApi.getOrders).toHaveBeenLastCalledWith(
      { limit: TD_ORDERS_PAGE_SIZE, accounts: ['acc_b'] })
    expect(store.rows.map(r => r.order_id)).toEqual([2])
    expect(store.nextCursor).toBeNull()
    expect(store.allAccountsSelected).toBe(false)
  })

  it('筛选到空集合时按缺省语义请求全部（契约 rest §2.9.2：空 = 全部在线账户）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(
      listResponse([row({ order_id: 1 })], { online_accounts: ['acc_a', 'acc_b'] }))
    const store = useTdOrdersStore()
    await store.init()

    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 2 })], { online_accounts: ['acc_a', 'acc_b'] }))
    await store.setAccountFilter([])

    expect(tdApi.getOrders).toHaveBeenLastCalledWith({ limit: TD_ORDERS_PAGE_SIZE, accounts: undefined })
    expect(store.allAccountsSelected).toBe(true)
  })

  it('筛选后已下线账户不参与查询（与在线集合求交）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(
      listResponse([row({ order_id: 1 })], { online_accounts: ['acc_a', 'acc_b'] }))
    const store = useTdOrdersStore()
    await store.init()

    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([], { online_accounts: ['acc_a'] }))
    await store.setAccountFilter(['acc_a', 'acc_gone'])

    expect(tdApi.getOrders).toHaveBeenLastCalledWith(
      { limit: TD_ORDERS_PAGE_SIZE, accounts: ['acc_a'] })
  })
})

describe('useTdOrdersStore · 降级（契约 rest §2.9.8 / webui-ws §5 第 13 条）', () => {
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
    vi.mocked(tdApi.getOrders).mockResolvedValue({ available: false, reason: 'not_found' })
    const store = useTdOrdersStore()

    await store.init()

    expect(store.available).toBe(false)
    expect(store.degradedReason).toBe('not_found')
    expect(store.rows).toEqual([])
    expect(store.isEmpty).toBe(false) // 降级不是"无数据"空态
    expect(store.degradedText).toContain('交易数据')
  })

  it('降级后按退避重试，恢复后回到可用态', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdOrdersStore()
    await store.init()
    expect(store.available).toBe(false)

    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([row({ order_id: 1 })]))
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)

    expect(store.available).toBe(true)
    expect(store.degradedReason).toBeNull()
    expect(store.rows).toHaveLength(1)
    expect(store.degradedText).toBe('')
  })

  it('连续降级时退避时间递增（2s → 4s）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdOrdersStore()
    await store.init()
    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)

    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)
    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)

    // 第二次退避是 2×base：只等 base 不够
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)
    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)
    expect(tdApi.getOrders).toHaveBeenCalledTimes(3)
  })

  it('卸载再挂载（dispose → init）后重新武装退避并从首档重试', async () => {
    // 首轮挂载即降级：重试一次后 attempt 升到 1，下一档退避 = 2×base
    vi.mocked(tdApi.getOrders).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdOrdersStore()
    await store.init()
    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)
    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)
    // 第二档是 2×base：只推进 base 不应再触发（证明 attempt 确实已递增）
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)
    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)

    // 卸载再挂载：新生命周期必须重新武装定时器，并把退避档位复位到首档
    store.dispose()
    await store.init()
    expect(tdApi.getOrders).toHaveBeenCalledTimes(3)
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([row({ order_id: 2 })]))
    // 若沿用卸载前的档位（2×base），此处只推进 base 不会重试
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(4)
    expect(store.available).toBe(true)
    expect(store.rows.map(r => r.order_id)).toEqual([2])
  })

  it('降级期间 WS 事件照常合并（不丢增量）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdOrdersStore()
    await store.init()

    store.applyOrderEvent(row({ order_id: 42 }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows.map(r => r.order_id)).toEqual([42])
    expect(store.available).toBe(false)
  })

  it('请求在途时卸载：丢弃晚到的响应（不写无视图的状态）', async () => {
    let resolveRequest: (value: unknown) => void = () => {}
    vi.mocked(tdApi.getOrders).mockReturnValue(new Promise((resolve) => {
      resolveRequest = resolve as (value: unknown) => void
    }) as never)
    const store = useTdOrdersStore()
    const pending = store.init()
    store.dispose()
    resolveRequest(listResponse([row({ order_id: 1 })]))
    await pending

    expect(store.rows).toEqual([])
  })

  it('dispose 清空定时器（卸载页面后不再请求）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdOrdersStore()
    await store.init()

    store.dispose()
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS * 10)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
  })
})

describe('useTdOrdersStore · 批量定时器可注入', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdBatchSchedulerForTests()
  })

  it('注入的调度器承接批量刷新（不依赖浏览器定时器）', async () => {
    const callbacks: Array<() => void> = []
    const cleared: number[] = []
    __setTdBatchSchedulerForTests({
      set: (callback) => { callbacks.push(callback); return callbacks.length },
      clear: (handle) => { cleared.push(handle as number) },
    })
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([]))
    const store = useTdOrdersStore()
    await store.init()

    store.applyOrderEvent(row({ order_id: 1 }))
    expect(callbacks).toHaveLength(1)
    expect(store.rows).toHaveLength(0)

    callbacks[0]()
    expect(store.rows.map(r => r.order_id)).toEqual([1])

    // 第二批重新调度，flush 后 handle 复位
    store.applyOrderEvent(row({ order_id: 2 }))
    expect(callbacks).toHaveLength(2)
    callbacks[1]()
    expect(store.rows.map(r => r.order_id)).toEqual([2, 1])
    expect(cleared).toEqual([])
  })
})
