import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import type { WsDataByType, WsServerMessageType } from '@/types/ws'
import type { TdOrderRow, TdTradeRow } from '@/types/api'

// 捕获 wsHandlers 注册的处理函数（不启动真实 WS 连接）：本套验证成交事件三键
// （td_trade_report / td_recent / data_changed{scope:"td"}）与成交 store 的接线，
// 以及它与委托 store 的**隔离**（同一组键不得串台）。
const registered = new Map<string, (payload: never, instanceId?: string) => void>()

vi.mock('@/composables/wsClient', () => ({
  registerHandler: (type: string, handler: (payload: never, instanceId?: string) => void) => {
    registered.set(type, handler)
  },
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

vi.mock('@/api/td', () => ({
  tdApi: { getOrders: vi.fn(), getTrades: vi.fn(), getStats: vi.fn() },
}))

// 模块级副作用：import 即注册（与 main.ts 的真实加载路径一致）
await import('@/composables/wsHandlers')
import {
  useTdTradesStore,
  TD_BATCH_INTERVAL_MS,
  __resetTdBatchSchedulerForTests,
} from '@/stores/tdTrades'
import { useTdOrdersStore } from '@/stores/tdOrders'
import { tdApi } from '@/api/td'

function dispatch<T extends WsServerMessageType>(type: T, payload: WsDataByType[T]): void {
  const handler = registered.get(type)
  if (!handler) throw new Error(`no handler registered for ${type}`)
  handler(payload as never)
}

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
    seq: 1,
    sort_key: `k-${tradeId}`,
    ...overrides,
  }
}

function order(overrides: Partial<TdOrderRow> = {}): TdOrderRow {
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

function tradeListResponse(items: TdTradeRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
    ...overrides,
  }
}

function orderListResponse(items: TdOrderRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
    ...overrides,
  }
}

describe('wsHandlers · 成交（#248 契约 webui-ws §2.5/§5）', () => {
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

  it('td_trade_report 增量入成交 store 批量合并（10Hz 内不立即写列表）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(tradeListResponse([]))
    vi.mocked(tdApi.getOrders).mockResolvedValue(orderListResponse([]))
    const trades = useTdTradesStore()
    await trades.init()

    dispatch('td_trade_report', trade({ trade_id: 't5' }))
    expect(trades.rows).toHaveLength(0)

    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    expect(trades.rows.map(r => r.trade_id)).toEqual(['t5'])
  })

  it('td_trade_report 只进成交 store，不污染委托 store（两份实例隔离）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(tradeListResponse([]))
    vi.mocked(tdApi.getOrders).mockResolvedValue(orderListResponse([]))
    const trades = useTdTradesStore()
    await trades.init()
    const orders = useTdOrdersStore()
    await orders.init()

    dispatch('td_trade_report', trade({ trade_id: 't5' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(trades.rows).toHaveLength(1)
    expect(orders.rows).toHaveLength(0)
  })

  it('td_order_report 不进成交 store（反向隔离）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(tradeListResponse([]))
    vi.mocked(tdApi.getOrders).mockResolvedValue(orderListResponse([]))
    const trades = useTdTradesStore()
    await trades.init()
    const orders = useTdOrdersStore()
    await orders.init()

    dispatch('td_order_report', order({ order_id: 7 }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)

    expect(orders.rows).toHaveLength(1)
    expect(trades.rows).toHaveLength(0)
  })

  it('td_recent 中的成交事件并入成交 store 并重拉成交基线（不动委托端点语义）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(tradeListResponse([]))
    vi.mocked(tdApi.getOrders).mockResolvedValue(orderListResponse([]))
    const trades = useTdTradesStore()
    await trades.init()
    const orders = useTdOrdersStore()
    await orders.init()
    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)
    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)

    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(
      tradeListResponse([trade({ trade_id: 't5', seq: 40 })]))
    // 委托侧基线保持可用（td_recent 同时触发两份 store 重拉，各打各的端点）
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(orderListResponse([]))
    dispatch('td_recent', {
      events: [{ type: 'td_trade_report', data: trade({ trade_id: 't5', seq: 40 }) }],
    })
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getTrades).toHaveBeenCalledTimes(2)
    expect(trades.rows.map(r => r.trade_id)).toEqual(['t5'])
  })

  it('data_changed{scope:"td"} 同时触发委托与成交两条重拉链路', async () => {
    const ordersHandle = vi.spyOn(useTdOrdersStore(), 'handleDataChanged').mockResolvedValue()
    const tradesHandle = vi.spyOn(useTdTradesStore(), 'handleDataChanged').mockResolvedValue()

    dispatch('data_changed', { scope: 'td' } as never)

    expect(ordersHandle).toHaveBeenCalledTimes(1)
    expect(ordersHandle).toHaveBeenCalledWith('td')
    expect(tradesHandle).toHaveBeenCalledTimes(1)
    expect(tradesHandle).toHaveBeenCalledWith('td')
  })

  it('data_changed 的其他 scope 不触碰成交列表', async () => {
    const tradesHandle = vi.spyOn(useTdTradesStore(), 'handleDataChanged').mockResolvedValue()
    vi.mocked(tdApi.getTrades).mockResolvedValue(tradeListResponse([]))
    const trades = useTdTradesStore()
    await trades.init()

    dispatch('data_changed', { scope: 'users' } as never)
    await vi.advanceTimersByTimeAsync(0)

    expect(tradesHandle).not.toHaveBeenCalled()
    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)
  })
})
