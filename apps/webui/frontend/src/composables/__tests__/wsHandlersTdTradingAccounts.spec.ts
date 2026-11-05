import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import type { WsDataByType, WsServerMessageType } from '@/types/ws'
import type { TdPositionRow, TdTradingAccountRow } from '@/types/api'

/**
 * wsHandlers · 资金接线（#250；契约 webui-ws §2.5/§5）。
 *
 * 固定四件事：
 * - `td_trading_account` 只进资金 store，不污染持仓 store（绝对态 store 隔离）；
 * - `td_recent` 的资金事件被认领并触发基线重拉，其他类型事件不串台；
 * - `data_changed{scope:"td"}` 同时触发委托/成交/持仓/资金/统计五条重拉链路；
 * - 非 td scope 不触碰资金。
 */
const registered = new Map<string, (payload: never, instanceId?: string) => void>()

vi.mock('@/composables/wsClient', () => ({
  registerHandler: (type: string, handler: (payload: never, instanceId?: string) => void) => {
    registered.set(type, handler)
  },
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

vi.mock('@/api/td', () => ({
  tdApi: {
    getOrders: vi.fn(),
    getTrades: vi.fn(),
    getPositions: vi.fn(),
    getStats: vi.fn(),
    getTradingAccounts: vi.fn(),
  },
}))

// 模块级副作用：import 即注册（与 main.ts 的真实加载路径一致）
await import('@/composables/wsHandlers')
import { useTdPositionsStore } from '@/stores/tdPositions'
import { useTdOrdersStore } from '@/stores/tdOrders'
import { useTdTradesStore } from '@/stores/tdTrades'
import { useTdStatsStore } from '@/stores/tdStats'
import { useTdTradingAccountsStore } from '@/stores/tdTradingAccounts'
import { tdApi } from '@/api/td'

function dispatch<T extends WsServerMessageType>(type: T, payload: WsDataByType[T]): void {
  const handler = registered.get(type)
  if (!handler) throw new Error(`no handler registered for ${type}`)
  handler(payload as never)
}

function account(overrides: Partial<TdTradingAccountRow> = {}): TdTradingAccountRow {
  return {
    account_id: 'acc_a',
    date: '20260808',
    has_data: true,
    balance: 100_000,
    available: 85_000,
    frozen: 0,
    commission: 12.5,
    margin: 15_000,
    withdraw_quota: 80_000,
    deposit: 0,
    withdraw: 0,
    seq: 10,
    ...overrides,
  }
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

/** 资金可用信封（契约 rest §2.9.2；next_cursor 恒 null） */
function fundListResponse(items: TdTradingAccountRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
    ...overrides,
  }
}

function positionListResponse(items: TdPositionRow[]) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
  }
}

describe('wsHandlers · 资金（#250 契约 webui-ws §2.5/§5）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    useTdPositionsStore().dispose()
    useTdTradingAccountsStore().dispose()
    useTdOrdersStore().dispose()
    useTdTradesStore().dispose()
    useTdStatsStore().dispose()
    vi.useRealTimers()
  })

  it('td_trading_account 增量进资金 store（绝对态按 account_id 覆盖）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([]))
    const funds = useTdTradingAccountsStore()
    await funds.init()

    dispatch('td_trading_account', account({ seq: 10, balance: 100 }))

    expect(funds.rows.map(row => row.account_id)).toEqual(['acc_a'])
    expect(funds.rows[0].balance).toBe(100)
  })

  it('td_trading_account 只进资金 store，不污染持仓 store', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([]))
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([]))
    const funds = useTdTradingAccountsStore()
    await funds.init()
    const positions = useTdPositionsStore()
    await positions.init()

    dispatch('td_trading_account', account({ seq: 10 }))

    expect(funds.rows).toHaveLength(1)
    expect(positions.rows).toHaveLength(0)
  })

  it('td_position_info 不进资金 store（反向隔离）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([]))
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([]))
    const funds = useTdTradingAccountsStore()
    await funds.init()
    const positions = useTdPositionsStore()
    await positions.init()

    dispatch('td_position_info', position())

    expect(positions.rows).toHaveLength(1)
    expect(funds.rows).toHaveLength(0)
  })

  it('td_recent 中的资金事件被认领并重拉资金基线', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([]))
    const funds = useTdTradingAccountsStore()
    await funds.init()
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(1)

    vi.mocked(tdApi.getTradingAccounts).mockResolvedValueOnce(
      fundListResponse([account({ seq: 40, balance: 400 })]))
    dispatch('td_recent', {
      events: [
        { type: 'td_trading_account', data: account({ seq: 40, balance: 400 }) },
        { type: 'td_position_info', data: position({ seq: 41 }) },
      ],
    })
    await vi.advanceTimersByTimeAsync(0)

    // 契约 webui-ws §5 第 7 条：补发有界，不能替代基线 → 收到补发必须重拉
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(2)
    expect(funds.rows[0].seq).toBe(40)
    expect(funds.rows[0].balance).toBe(400)
  })

  it('data_changed{scope:"td"} 同时触发委托/成交/持仓/资金/统计五条重拉链路', () => {
    const ordersHandle = vi.spyOn(useTdOrdersStore(), 'handleDataChanged').mockResolvedValue()
    const tradesHandle = vi.spyOn(useTdTradesStore(), 'handleDataChanged').mockResolvedValue()
    const positionsHandle = vi.spyOn(useTdPositionsStore(), 'handleDataChanged').mockResolvedValue()
    const fundsHandle = vi.spyOn(useTdTradingAccountsStore(), 'handleDataChanged').mockResolvedValue()
    const statsHandle = vi.spyOn(useTdStatsStore(), 'handleDataChanged').mockResolvedValue()

    dispatch('data_changed', { scope: 'td' } as never)

    expect(ordersHandle).toHaveBeenCalledWith('td')
    expect(tradesHandle).toHaveBeenCalledWith('td')
    expect(positionsHandle).toHaveBeenCalledWith('td')
    expect(fundsHandle).toHaveBeenCalledWith('td')
    expect(statsHandle).toHaveBeenCalledWith()
  })

  it('data_changed 的其他 scope 不触碰资金', async () => {
    const fundsHandle = vi.spyOn(useTdTradingAccountsStore(), 'handleDataChanged').mockResolvedValue()
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([]))
    const funds = useTdTradingAccountsStore()
    await funds.init()

    dispatch('data_changed', { scope: 'users' } as never)
    await vi.advanceTimersByTimeAsync(0)

    expect(fundsHandle).not.toHaveBeenCalled()
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(1)
  })
})
