import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import type { WsDataByType, WsServerMessageType } from '@/types/ws'
import type { TdOrderRow, TdPositionRow } from '@/types/api'

/**
 * wsHandlers · 持仓接线（#249；契约 webui-ws §2.5/§5）。
 *
 * 固定四件事：
 * - `td_position_info` 只进持仓 store，不污染委托 store（三份 store 隔离）；
 * - `td_recent` 的持仓事件被认领并触发基线重拉，其他类型事件不串台；
 * - `data_changed{scope:"td"}` 同时触发委托/成交/持仓/统计四条重拉链路；
 * - 非 td scope 不触碰持仓。
 */
const registered = new Map<string, (payload: never, instanceId?: string) => void>()

vi.mock('@/composables/wsClient', () => ({
  registerHandler: (type: string, handler: (payload: never, instanceId?: string) => void) => {
    registered.set(type, handler)
  },
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

vi.mock('@/api/td', () => ({
  tdApi: { getOrders: vi.fn(), getTrades: vi.fn(), getPositions: vi.fn(), getStats: vi.fn() },
}))

// 模块级副作用：import 即注册（与 main.ts 的真实加载路径一致）
await import('@/composables/wsHandlers')
import { useTdPositionsStore, TD_POSITIONS_PAGE_SIZE } from '@/stores/tdPositions'
import { useTdOrdersStore, __resetTdBatchSchedulerForTests } from '@/stores/tdOrders'
import { useTdTradesStore } from '@/stores/tdTrades'
import { useTdStatsStore, __resetTdStatsSchedulerForTests } from '@/stores/tdStats'
import { tdApi } from '@/api/td'

function dispatch<T extends WsServerMessageType>(type: T, payload: WsDataByType[T]): void {
  const handler = registered.get(type)
  if (!handler) throw new Error(`no handler registered for ${type}`)
  handler(payload as never)
}

function position(overrides: Partial<TdPositionRow> = {}): TdPositionRow {
  return {
    account_id: 'acc_a',
    date: '20260808',
    instrument_id: 'rb2601',
    exchange_id: 'SHFE',
    direction: 'Long',
    volume: 2,
    frozen_volume: 0,
    today_volume: 1,
    yd_volume: 1,
    price: 3512.35,
    price_decimals: 1,
    seq: 10,
    ...overrides,
  }
}

function order(overrides: Partial<TdOrderRow> = {}): TdOrderRow {
  return {
    account_id: 'acc_a',
    date: '20260808',
    order_id: 1,
    strategy_id: 'stg_demo',
    instrument_id: 'rb2601',
    exchange_id: 'SHFE',
    direction: 'Long',
    position_effect: 'Open',
    price_type: 'Limit',
    status: 'NotTraded',
    price: 3512.5,
    volume: 2,
    volume_traded: 0,
    remark: '',
    time: 32400,
    avg_price: null,
    price_decimals: 1,
    avg_price_decimals: 3,
    seq: 1,
    sort_key: 'k-000001',
    ...overrides,
  }
}

function positionListResponse(items: TdPositionRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
    ...overrides,
  }
}

describe('wsHandlers · 持仓（#249 契约 webui-ws §2.5/§5）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdBatchSchedulerForTests()
    __resetTdStatsSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdBatchSchedulerForTests()
    __resetTdStatsSchedulerForTests()
  })

  it('td_position_info 增量进持仓 store（绝对态覆盖，不批量入队）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([]))
    const positions = useTdPositionsStore()
    await positions.init()

    dispatch('td_position_info', position({ instrument_id: 'rb2601', seq: 10 }))

    // 持仓是绝对态：事件立即生效，不需要等 10Hz 批量窗口
    expect(positions.rows.map(r => r.instrument_id)).toEqual(['rb2601'])
  })

  it('td_position_info 只进持仓 store，不污染委托 store（三份 store 隔离）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([]))
    vi.mocked(tdApi.getOrders).mockResolvedValue({
      available: true as const,
      trading_day: '20260808',
      online_accounts: ['acc_a'],
      items: [],
      next_cursor: null,
    })
    const positions = useTdPositionsStore()
    await positions.init()
    const orders = useTdOrdersStore()
    await orders.init()

    dispatch('td_position_info', position())
    await vi.advanceTimersByTimeAsync(200)

    expect(positions.rows).toHaveLength(1)
    expect(orders.rows).toHaveLength(0)
  })

  it('td_order_report 不进持仓 store（反向隔离）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([]))
    vi.mocked(tdApi.getOrders).mockResolvedValue({
      available: true as const,
      trading_day: '20260808',
      online_accounts: ['acc_a'],
      items: [],
      next_cursor: null,
    })
    const positions = useTdPositionsStore()
    await positions.init()
    const orders = useTdOrdersStore()
    await orders.init()

    dispatch('td_order_report', order({ order_id: 7 }))
    await vi.advanceTimersByTimeAsync(200)

    expect(orders.rows).toHaveLength(1)
    expect(positions.rows).toHaveLength(0)
  })

  it('td_recent 中的持仓事件被认领并重拉持仓基线（不动其他端点语义）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([]))
    vi.mocked(tdApi.getOrders).mockResolvedValue({
      available: true as const,
      trading_day: '20260808',
      online_accounts: ['acc_a'],
      items: [],
      next_cursor: null,
    })
    vi.mocked(tdApi.getTrades).mockResolvedValue({
      available: true as const,
      trading_day: '20260808',
      online_accounts: ['acc_a'],
      items: [],
      next_cursor: null,
    })
    const positions = useTdPositionsStore()
    await positions.init()
    await useTdOrdersStore().init()
    expect(tdApi.getPositions).toHaveBeenCalledTimes(1)

    vi.mocked(tdApi.getPositions).mockResolvedValueOnce(positionListResponse([
      position({ instrument_id: 'rb2601', seq: 40 }),
    ]))
    dispatch('td_recent', {
      events: [{ type: 'td_position_info', data: position({ instrument_id: 'rb2601', seq: 40 }) }],
    })
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getPositions).toHaveBeenCalledTimes(2)
    expect(tdApi.getPositions).toHaveBeenLastCalledWith({
      limit: TD_POSITIONS_PAGE_SIZE,
      accounts: undefined,
    })
    expect(positions.rows.map(r => r.instrument_id)).toEqual(['rb2601'])
  })

  it('data_changed{scope:"td"} 同时触发委托/成交/持仓/统计四条重拉链路', () => {
    const ordersHandle = vi.spyOn(useTdOrdersStore(), 'handleDataChanged').mockResolvedValue()
    const tradesHandle = vi.spyOn(useTdTradesStore(), 'handleDataChanged').mockResolvedValue()
    const positionsHandle = vi.spyOn(useTdPositionsStore(), 'handleDataChanged').mockResolvedValue()
    const statsHandle = vi.spyOn(useTdStatsStore(), 'handleDataChanged').mockResolvedValue()

    dispatch('data_changed', { scope: 'td' } as never)

    expect(ordersHandle).toHaveBeenCalledWith('td')
    expect(tradesHandle).toHaveBeenCalledWith('td')
    expect(positionsHandle).toHaveBeenCalledWith('td')
    expect(statsHandle).toHaveBeenCalledWith()
  })

  it('data_changed 的其他 scope 不触碰持仓', async () => {
    const positionsHandle = vi.spyOn(useTdPositionsStore(), 'handleDataChanged').mockResolvedValue()
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([]))
    const positions = useTdPositionsStore()
    await positions.init()

    dispatch('data_changed', { scope: 'users' } as never)
    await vi.advanceTimersByTimeAsync(0)

    expect(positionsHandle).not.toHaveBeenCalled()
    expect(tdApi.getPositions).toHaveBeenCalledTimes(1)
  })
})
