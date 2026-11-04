import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import type { WsDataByType, WsServerMessageType } from '@/types/ws'
import type { TdOrderRow } from '@/types/api'

// 捕获 wsHandlers 注册的处理函数（不启动真实 WS 连接）：本套验证交易数据三键
// （td_order_report / td_recent / data_changed{scope:"td"}）与委托 store 的接线。
const registered = new Map<string, (payload: never, instanceId?: string) => void>()

vi.mock('@/composables/wsClient', () => ({
  registerHandler: (type: string, handler: (payload: never, instanceId?: string) => void) => {
    registered.set(type, handler)
  },
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

vi.mock('@/api/td', () => ({
  tdApi: { getOrders: vi.fn() },
}))

// 模块级副作用：import 即注册（与 main.ts 的真实加载路径一致）
await import('@/composables/wsHandlers')
import { useTdOrdersStore, TD_BATCH_INTERVAL_MS, __resetTdBatchSchedulerForTests } from '@/stores/tdOrders'
import { tdApi } from '@/api/td'

function dispatch<T extends WsServerMessageType>(type: T, payload: WsDataByType[T]): void {
  const handler = registered.get(type)
  if (!handler) throw new Error(`no handler registered for ${type}`)
  handler(payload as never)
}

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

describe('wsHandlers · 交易数据（#246 契约 webui-ws §2.5/§5）', () => {
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

  it('td_order_report 增量经 store 批量合并（10Hz 内不立即写列表）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([]))
    const store = useTdOrdersStore()
    await store.init()

    dispatch('td_order_report', row({ order_id: 5 }))
    expect(store.rows).toHaveLength(0)

    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    expect(store.rows.map(r => r.order_id)).toEqual([5])
  })

  it('td_order_report 乱序/重复无害：同键 seq 大者胜', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([]))
    const store = useTdOrdersStore()
    await store.init()

    dispatch('td_order_report', row({ order_id: 5, seq: 8, status: 'AllTraded' }))
    dispatch('td_order_report', row({ order_id: 5, seq: 4, status: 'NotTraded' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(store.rows).toHaveLength(1)
    expect(store.rows[0].status).toBe('AllTraded')
  })

  it('td_recent（重连信号）触发清空重拉 REST 基线（补发有界，基线权威 §5 第 7/8 条）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([row({ order_id: 1, seq: 10 })]))
    const store = useTdOrdersStore()
    await store.init()

    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 2, seq: 11, sort_key: 'k-000002' })]))
    dispatch('td_recent', {
      events: [{ type: 'td_order_report', data: row({ order_id: 2, seq: 11, sort_key: 'k-000002' }) }],
    })
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)
    expect(store.rows.map(r => r.order_id)).toEqual([2])
  })

  it('td_recent 空数组不报错、仍重拉基线（服务端无事件也发送，§2.5）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([]))
    const store = useTdOrdersStore()
    await store.init()

    dispatch('td_recent', { events: [] })
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)
  })

  it('td_recent 非委托事件（持仓/资金）不污染委托列表（本轮不消费，基线重拉覆盖）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([]))
    const store = useTdOrdersStore()
    await store.init()

    dispatch('td_recent', { events: [{ type: 'td_position_info', data: { foo: 1 } }] })
    await vi.advanceTimersByTimeAsync(0)

    expect(store.rows).toHaveLength(0)
    expect(store.error).toBeNull()
  })

  it('td_recent 载荷非法（无 events 数组）忽略，不重拉', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([]))
    const store = useTdOrdersStore()
    await store.init()

    dispatch('td_recent', {} as never)
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
  })

  it('data_changed{scope:"td"} 触发清空 + 重拉基线（契约 webui-ws §2.3/§5 第 10 条）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([row({ order_id: 1, seq: 10 })]))
    const store = useTdOrdersStore()
    await store.init()

    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(listResponse([row({ order_id: 3, seq: 30 })]))
    dispatch('data_changed', { scope: 'td' } as never)
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(2)
    expect(store.rows.map(r => r.order_id)).toEqual([3])
  })

  it('data_changed 的其他 scope 不触碰委托列表', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([row({ order_id: 1 })]))
    const store = useTdOrdersStore()
    await store.init()

    dispatch('data_changed', { scope: 'users' } as never)
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
  })
})
