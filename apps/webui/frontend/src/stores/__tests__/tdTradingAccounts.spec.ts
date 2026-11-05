import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useTdTradingAccountsStore } from '../tdTradingAccounts'
// 退避口径的唯一来源是共享定时器（委托/成交/持仓/资金同一份），测试从那里取常量。
import { TD_RETRY_BASE_DELAY_MS } from '@/composables/tdListStore'
import { tdApi } from '@/api/td'
import type { TdTradingAccountRow } from '@/types/api'

vi.mock('@/api/td', () => ({
  tdApi: { getTradingAccounts: vi.fn() },
}))

/**
 * 资金 store 用例（契约 rest §2.9.1/§2.9.2/§2.9.4/§2.9.8，契约 webui-ws §2.5/§5）。
 *
 * 与委托/成交/持仓的关键差异在此被逐条固定：
 * - **不分页**：单请求拿全量账户（`next_cursor` 恒 null，非 null 也不跟随）；
 * - 一笔 = 一个已登记账户（含离线与 `has_data:false`），整体替换而不是有界窗口合并；
 * - 业务键 = account_id，`has_data:false` 行是有效数据（不能被「资金字段全 null」筛掉）；
 * - 同账户 seq **严格回退** = 数据重置 → 清空重拉基线；相等 = 重复消息忽略。
 */
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

/** 无资金记录的账户行（has_data:false；date/seq/资金字段全 null，契约 rest §2.9.4） */
function emptyAccount(overrides: Partial<TdTradingAccountRow> = {}): TdTradingAccountRow {
  return account({
    date: null,
    has_data: false,
    balance: null,
    available: null,
    frozen: null,
    commission: null,
    margin: null,
    withdraw_quota: null,
    deposit: null,
    withdraw: null,
    seq: null,
    ...overrides,
  })
}

/** 可用信封（契约 rest §2.9.2，与持仓/委托/成交同形；资金端点 next_cursor 恒 null） */
function listResponse(items: TdTradingAccountRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
    ...overrides,
  }
}

describe('useTdTradingAccountsStore · REST 基线（契约 rest §2.9.1/§2.9.2/§2.9.4）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    useTdTradingAccountsStore().dispose()
    vi.useRealTimers()
  })

  it('init 单请求拉资金基线并写入行/交易日/在线账户/聚合数', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(listResponse([
      account({ account_id: 'acc_a' }),
      account({ account_id: 'acc_b', has_data: false, date: null, seq: null, balance: null }),
    ]))
    const store = useTdTradingAccountsStore()

    await store.init()

    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(1)
    expect(store.rows).toHaveLength(2)
    expect(store.tradingDay).toBe('20260808')
    expect(store.onlineAccounts).toEqual(['acc_a'])
    expect(store.available).toBe(true)
    expect(store.isEmpty).toBe(false)
    // 聚合数：在线 1 / 已登记 2（含离线与无记录账户）
    expect(store.onlineCount).toBe(1)
    expect(store.totalAccounts).toBe(2)
  })

  it('行按 account_id 升序，且 has_data:false 的离线账户不被筛掉', async () => {
    // 故意乱序返回：前端按服务端顺序归一，而不是「信任到达顺序」
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(listResponse(
      [
        account({ account_id: 'acc_b', seq: 11 }),
        emptyAccount({ account_id: 'acc_offline' }),
        account({ account_id: 'acc_a', seq: 10 }),
      ],
      { online_accounts: ['acc_a', 'acc_b'] }))
    const store = useTdTradingAccountsStore()

    await store.init()

    expect(store.rows.map(row => row.account_id)).toEqual(['acc_a', 'acc_b', 'acc_offline'])
    const noData = store.rows[2]
    expect(noData.has_data).toBe(false)
    expect(noData.date).toBeNull()
    expect(noData.seq).toBeNull()
    expect(noData.balance).toBeNull()
    // 无记录行的 date 为 null，不进数据日期集合（null 不是数据日期）
    expect(store.dataDates).toEqual(['20260808'])
  })

  it('端点不分页：即使返回非 null 的 next_cursor 也只请求一次（不无限翻页）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(
      listResponse([account()], { next_cursor: 'should-be-ignored' }))
    const store = useTdTradingAccountsStore()

    await store.init()

    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(1)
    expect(store.rows).toHaveLength(1)
  })

  it('多账户锚点日不同：dataDates 去重升序（面板不得只标一个日期）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(listResponse([
      account({ account_id: 'acc_a', date: '20260808' }),
      account({ account_id: 'acc_b', date: '20260807' }),
      account({ account_id: 'acc_c', date: '20260807' }),
    ]))
    const store = useTdTradingAccountsStore()

    await store.init()

    expect(store.dataDates).toEqual(['20260807', '20260808'])
  })

  it('成功读到空基线：isEmpty=true（与降级/请求失败区分）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(listResponse([]))
    const store = useTdTradingAccountsStore()

    await store.init()

    expect(store.isEmpty).toBe(true)
    expect(store.available).toBe(true)
    expect(store.rows).toEqual([])
  })

  it('基线整体替换：REST 没返回的账户行被丢弃（不保留幽灵资金卡）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValueOnce(listResponse([
      account({ account_id: 'acc_a' }),
      account({ account_id: 'acc_b' }),
    ]))
    const store = useTdTradingAccountsStore()
    await store.init()
    expect(store.rows).toHaveLength(2)

    vi.mocked(tdApi.getTradingAccounts).mockResolvedValueOnce(listResponse([
      account({ account_id: 'acc_a' }),
    ]))
    await store.loadBaseline()

    expect(store.rows.map(row => row.account_id)).toEqual(['acc_a'])
  })

  it('请求失败：记 error、保留已有行、不冒充「无数据」空态', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(listResponse([account()]))
    const store = useTdTradingAccountsStore()
    await store.init()

    vi.mocked(tdApi.getTradingAccounts).mockRejectedValue(new Error('HTTP 500'))
    await store.loadBaseline()

    expect(store.error).toBe('HTTP 500')
    expect(store.rows).toHaveLength(1)
    expect(store.isEmpty).toBe(false)
  })
})

describe('useTdTradingAccountsStore · 绝对态 WS 合并（契约 webui-ws §5 第 8 条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    useTdTradingAccountsStore().dispose()
    vi.useRealTimers()
  })

  async function storeWithBaseline(
    items: TdTradingAccountRow[] = [],
    overrides: Record<string, unknown> = {},
  ): Promise<ReturnType<typeof useTdTradingAccountsStore>> {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(listResponse(items, overrides))
    const store = useTdTradingAccountsStore()
    await store.init()
    return store
  }

  it('业务键 account_id 覆盖：同键 seq 大者胜', async () => {
    const store = await storeWithBaseline([account({ seq: 10, balance: 100 })])

    store.applyTradingAccountEvent(account({ seq: 12, balance: 200 }))

    expect(store.rows).toHaveLength(1)
    expect(store.rows[0].seq).toBe(12)
    expect(store.rows[0].balance).toBe(200)
  })

  it('seq 相等 = 重复消息忽略：不覆盖、不触发重拉', async () => {
    const store = await storeWithBaseline([account({ seq: 10, balance: 100 })])

    store.applyTradingAccountEvent(account({ seq: 10, balance: 999 }))
    await vi.advanceTimersByTimeAsync(0)

    expect(store.rows[0].balance).toBe(100)
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(1)
  })

  it('新账户事件插入并按 account_id 升序排序，同时并入在线集合', async () => {
    const store = await storeWithBaseline(
      [account({ account_id: 'acc_b', seq: 10 })], { online_accounts: ['acc_b'] })

    store.applyTradingAccountEvent(account({ account_id: 'acc_a', seq: 11 }))

    expect(store.rows.map(row => row.account_id)).toEqual(['acc_a', 'acc_b'])
    expect(store.onlineAccounts).toEqual(['acc_a', 'acc_b'])
  })

  it('同账户 seq 严格回退 = 数据重置：清空并重拉基线', async () => {
    const store = await storeWithBaseline([account({ seq: 50, balance: 500 })])
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValueOnce(
      listResponse([account({ seq: 2, balance: 1 })]))

    // 回退事件（seq 5 < 已见水位 50）= 进程重启/库重置；不得把 seq 5 写进面板
    store.applyTradingAccountEvent(account({ seq: 5, balance: 99 }))
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(2)
    expect(store.rows[0].seq).toBe(2)
    expect(store.rows[0].balance).toBe(1)
  })

  it('空 account_id / 缺失 seq 的事件被忽略（坏行不让 store 抛错）', async () => {
    const store = await storeWithBaseline([account({ seq: 10 })])

    store.applyTradingAccountEvent(account({ account_id: '', seq: 20 }))
    store.applyTradingAccountEvent(account({ seq: null }))
    await vi.advanceTimersByTimeAsync(0)

    expect(store.rows[0].seq).toBe(10)
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(1)
  })

  it('降级期间 WS 事件照常合并（不丢增量）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdTradingAccountsStore()
    await store.init()

    store.applyTradingAccountEvent(account({ seq: 42, balance: 321 }))

    expect(store.rows.map(row => row.account_id)).toEqual(['acc_a'])
    expect(store.rows[0].balance).toBe(321)
    expect(store.available).toBe(false)
  })
})

describe('useTdTradingAccountsStore · 重置触发（td_recent / data_changed）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    useTdTradingAccountsStore().dispose()
    vi.useRealTimers()
  })

  async function initialized(): Promise<ReturnType<typeof useTdTradingAccountsStore>> {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(listResponse([
      account({ seq: 10, balance: 100 }),
    ]))
    const store = useTdTradingAccountsStore()
    await store.init()
    return store
  }

  it('td_recent 认领 td_trading_account 事件并重拉基线（其他类型事件忽略）', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValueOnce(listResponse([
      account({ seq: 12, balance: 200 }),
    ]))

    store.applyRecent({
      events: [
        { type: 'td_trading_account', data: account({ seq: 12, balance: 200 }) },
        // 委托事件不属资金 store，忽略
        { type: 'td_order_report', data: { order_id: 1 } },
      ],
    })
    await vi.advanceTimersByTimeAsync(0)

    // 契约 webui-ws §5 第 7 条：补发有界，不能替代基线 → 必须重拉
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(2)
    expect(store.rows[0].seq).toBe(12)
    expect(store.rows[0].balance).toBe(200)
  })

  it('td_recent 先并入补发：基线重拉失败时保留增量与旧行（不丢不空屏）', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getTradingAccounts).mockRejectedValueOnce(new Error('HTTP 503'))

    store.applyRecent({
      events: [{ type: 'td_trading_account', data: account({ seq: 12, balance: 200 }) }],
    })
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(2)
    expect(store.error).toBe('HTTP 503')
    // 补发事件按同键 seq 大者胜并入，没有被失望的重拉清掉
    expect(store.rows[0].seq).toBe(12)
    expect(store.rows[0].balance).toBe(200)
  })

  it('data_changed{scope:"td"} → 先清空再重拉基线', async () => {
    const store = await initialized()
    let rowsAtReloadRequest: number | null = null
    vi.mocked(tdApi.getTradingAccounts).mockImplementationOnce(async () => {
      // 请求发起时旧行必须已被清空（清空重拉，不残留旧账户集合/旧锚点日）
      rowsAtReloadRequest = store.rows.length
      return listResponse([account({ seq: 20, balance: 300 })])
    })

    await store.handleDataChanged('td')

    expect(rowsAtReloadRequest).toBe(0)
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(2)
    expect(store.rows[0].seq).toBe(20)
  })

  it('非 td scope 不触发资金重拉', async () => {
    const store = await initialized()

    await store.handleDataChanged('market_sources')
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(1)
  })

  it('未初始化时 td_recent / data_changed 不发起请求（页面未打开不产生基线流量）', async () => {
    const store = useTdTradingAccountsStore()

    store.applyRecent({ events: [{ type: 'td_trading_account', data: account() }] })
    await store.handleDataChanged('td')
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getTradingAccounts).not.toHaveBeenCalled()
  })
})

describe('useTdTradingAccountsStore · 降级（契约 rest §2.9.8 / webui-ws §5 第 13 条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    useTdTradingAccountsStore().dispose()
    vi.useRealTimers()
  })

  it('available:false → 降级态 + 原因，不用 0 冒充资金数据', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue({ available: false, reason: 'not_found' })
    const store = useTdTradingAccountsStore()

    await store.init()

    expect(store.available).toBe(false)
    expect(store.degradedReason).toBe('not_found')
    expect(store.rows).toEqual([])
    expect(store.isEmpty).toBe(false)
    expect(store.degradedText).toContain('交易数据')
  })

  it('降级后按退避重试，恢复后回到可用态', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdTradingAccountsStore()
    await store.init()
    expect(store.available).toBe(false)

    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(listResponse([account()]))
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)

    expect(store.available).toBe(true)
    expect(store.degradedReason).toBeNull()
    expect(store.rows).toHaveLength(1)
    expect(store.degradedText).toBe('')
  })

  it('卸载再挂载（dispose → init）后重新武装退避并从首档重试', async () => {
    // 首轮挂载即降级：重试一次后 attempt 升到 1，下一档退避 = 2×base
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdTradingAccountsStore()
    await store.init()
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(2)
    // 第二档是 2×base：只推进 base 不应再触发（证明 attempt 确实已递增）
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(2)

    // 卸载再挂载：新生命周期必须重新武装定时器，并把退避档位复位到首档
    store.dispose()
    await store.init()
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(3)
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(listResponse([account({ seq: 20 })]))
    // 若沿用卸载前的档位（2×base），此处只推进 base 不会重试
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)

    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(4)
    expect(store.available).toBe(true)
    expect(store.rows[0].seq).toBe(20)
  })

  it('dispose 清空定时器（卸载页面后不再请求）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdTradingAccountsStore()
    await store.init()

    store.dispose()
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS * 10)

    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(1)
  })

  it('dispose 后重新 init：解除停用状态并重拉基线（页面重新挂载）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(listResponse([account()]))
    const store = useTdTradingAccountsStore()
    await store.init()
    store.dispose()
    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(1)

    await store.init()

    expect(tdApi.getTradingAccounts).toHaveBeenCalledTimes(2)
    expect(store.rows).toHaveLength(1)
    expect(store.initialized).toBe(true)
  })
})
