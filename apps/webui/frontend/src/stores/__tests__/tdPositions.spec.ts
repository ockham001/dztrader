import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import {
  TD_POSITIONS_MAX_PAGES,
  TD_POSITIONS_PAGE_SIZE,
  useTdPositionsStore,
} from '../tdPositions'
// 退避口径的唯一来源是共享定时器（委托/成交/持仓同一份），测试从那里取常量。
import { TD_RETRY_BASE_DELAY_MS } from '@/composables/tdListStore'
import { tdApi } from '@/api/td'
import type { TdPositionRow } from '@/types/api'

vi.mock('@/api/td', () => ({
  tdApi: { getPositions: vi.fn() },
}))

/**
 * 持仓 store 用例（契约 rest §2.9.1/§2.9.2/§2.9.4/§2.9.8，契约 webui-ws §2.5/§5）。
 *
 * 与委托/成交 store 的关键差异在此被逐条固定：
 * - 绝对态：REST 基线**替换**（消失的业务键必须丢），不是有界窗口追加；
 * - 无 sort_key：顺序 = 服务端 `(account_id, instrument_id, direction)` 升序；
 * - 账户全集含离线（不是 online_accounts）；
 * - `volume=0` 零帧 = 删行，不是插入 0 行。
 */
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

/** 可用信封（契约 rest §2.9.2，与委托/成交同形） */
function listResponse(items: TdPositionRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
    ...overrides,
  }
}

describe('useTdPositionsStore · REST 基线（契约 rest §2.9.2/§2.9.4）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  it('init 用持仓端点拉首页并写入行/交易日/在线账户/账户全集', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse([
      position({ instrument_id: 'rb2601' }),
      position({ instrument_id: 'ag2601', direction: 'Short', volume: 3, seq: 11 }),
    ]))
    const store = useTdPositionsStore()

    await store.init()

    expect(tdApi.getPositions).toHaveBeenCalledWith({
      limit: TD_POSITIONS_PAGE_SIZE,
      accounts: undefined,
    })
    expect(store.rows).toHaveLength(2)
    expect(store.tradingDay).toBe('20260808')
    expect(store.onlineAccounts).toEqual(['acc_a'])
    // 账户全集 = 在线 ∪ 行内出现（含离线）；此处两者一致
    expect(store.accounts).toEqual(['acc_a'])
    expect(store.available).toBe(true)
    expect(store.isEmpty).toBe(false)
  })

  it('行顺序 = 服务端 (account, instrument, direction) 升序，不依赖 sort_key', async () => {
    // 故意按乱序返回（服务端顺序本应有序，这里验证前端归一而不是"信任到达顺序"）
    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse([
      position({ account_id: 'acc_b', instrument_id: 'rb2601', direction: 'Long' }),
      position({ account_id: 'acc_a', instrument_id: 'rb2601', direction: 'Long' }),
      position({ account_id: 'acc_a', instrument_id: 'rb2601', direction: 'Short' }),
      position({ account_id: 'acc_a', instrument_id: 'ag2601', direction: 'Long' }),
    ]))
    const store = useTdPositionsStore()

    await store.init()

    expect(store.rows.map(r => `${r.account_id}/${r.instrument_id}/${r.direction}`)).toEqual([
      'acc_a/ag2601/Long',
      'acc_a/rb2601/Short',
      'acc_a/rb2601/Long',
      'acc_b/rb2601/Long',
    ])
  })

  it('跟随 next_cursor 拉完整基线（绝对态面板需要完整集合才能正确聚合）', async () => {
    vi.mocked(tdApi.getPositions)
      .mockResolvedValueOnce(listResponse([position({ instrument_id: 'rb2601' })], { next_cursor: 'c1' }))
      .mockResolvedValueOnce(listResponse([position({ instrument_id: 'ag2601' })], { next_cursor: null }))
    const store = useTdPositionsStore()

    await store.init()

    expect(tdApi.getPositions).toHaveBeenCalledTimes(2)
    expect(tdApi.getPositions).toHaveBeenLastCalledWith({
      limit: TD_POSITIONS_PAGE_SIZE,
      cursor: 'c1',
      accounts: undefined,
    })
    expect(store.rows.map(r => r.instrument_id)).toEqual(['ag2601', 'rb2601'])
    expect(store.truncated).toBe(false)
  })

  it('页数超上限时停止并标记 truncated（不静默截断成"完整列表"）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(
      listResponse([position()], { next_cursor: 'more' }))
    const store = useTdPositionsStore()

    await store.init()

    expect(tdApi.getPositions).toHaveBeenCalledTimes(TD_POSITIONS_MAX_PAGES)
    expect(store.truncated).toBe(true)
  })

  it('请求失败记错误、保留已有行、不冒充「无数据」空态', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse([position({ instrument_id: 'rb2601' })]))
    const store = useTdPositionsStore()
    await store.init()

    vi.mocked(tdApi.getPositions).mockRejectedValue(new Error('HTTP 500'))
    // 直接重拉基线（resetAndReload 会先清空；这里验证"失败不产生假空态"的判据）
    await store.resetAndReload()
    await vi.advanceTimersByTimeAsync(0)

    expect(store.error).toBe('HTTP 500')
    expect(store.isEmpty).toBe(false)
  })
})

describe('useTdPositionsStore · 绝对态 WS 合并（契约 webui-ws §5 第 8 条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  async function storeWithBaseline(items: TdPositionRow[] = []): Promise<ReturnType<typeof useTdPositionsStore>> {
    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse(items))
    const store = useTdPositionsStore()
    await store.init()
    return store
  }

  it('业务键 (account, instrument, direction) 覆盖：同键 seq 大者胜，旧 seq 被忽略', async () => {
    const store = await storeWithBaseline([position({ instrument_id: 'rb2601', seq: 10, volume: 2 })])

    store.applyPositionEvent(position({ instrument_id: 'rb2601', seq: 12, volume: 5 }))
    expect(store.rows[0].volume).toBe(5)

    // 迟到的旧版本：同键 seq 更小 → 忽略（乱序无害）
    store.applyPositionEvent(position({ instrument_id: 'rb2601', seq: 9, volume: 99 }))
    expect(store.rows[0].volume).toBe(5)
    expect(store.rows[0].seq).toBe(12)
  })

  it('同合约多空是两条独立记录（不按合约单独去重）', async () => {
    const store = await storeWithBaseline()

    store.applyPositionEvent(position({ instrument_id: 'rb2601', direction: 'Long', volume: 2, seq: 10 }))
    store.applyPositionEvent(position({ instrument_id: 'rb2601', direction: 'Short', volume: 3, seq: 11 }))

    expect(store.rows).toHaveLength(2)
    expect(store.rows.map(r => r.direction)).toEqual(['Short', 'Long'])
  })

  it('volume=0 的零帧 = 该持仓已消失 → 删行（不是插入一条 0 手持仓）', async () => {
    const store = await storeWithBaseline([position({ instrument_id: 'rb2601', volume: 2, seq: 10 })])

    // 绝对态语义下零帧的 seq 必然更大（契约 td-data-sync §3：消失 side 发零帧）
    store.applyPositionEvent(position({ instrument_id: 'rb2601', volume: 0, seq: 20 }))

    expect(store.rows).toHaveLength(0)
    expect(store.isEmpty).toBe(true)
  })

  it('绝对态不接受旧快照回写：同键 seq 更大但 date 更旧的事件被丢弃', async () => {
    const store = await storeWithBaseline([
      position({ instrument_id: 'rb2601', date: '20260808', seq: 10, volume: 2 }),
    ])

    store.applyPositionEvent(
      position({ instrument_id: 'rb2601', date: '20260807', seq: 30, volume: 99 }))

    expect(store.rows[0].volume).toBe(2)
    expect(store.rows[0].date).toBe('20260808')
  })

  it('同账户 seq 回退且业务键未知 → 视为数据重置，清空重拉基线', async () => {
    const store = await storeWithBaseline([position({ instrument_id: 'rb2601', seq: 50 })])
    vi.mocked(tdApi.getPositions).mockResolvedValueOnce(listResponse([
      position({ instrument_id: 'ag2601', seq: 2 }),
    ]))

    store.applyPositionEvent(position({ instrument_id: 'ag2601', seq: 2 }))
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getPositions).toHaveBeenCalledTimes(2)
    expect(store.rows.map(r => r.instrument_id)).toEqual(['ag2601'])
  })

  it('离线账户的增量事件也并入并进入账户全集（持仓覆盖全部已登记账户）', async () => {
    const store = await storeWithBaseline([
      position({ account_id: 'acc_a', instrument_id: 'rb2601', seq: 10 }),
    ])

    store.applyPositionEvent(
      position({ account_id: 'acc_offline', instrument_id: 'ag2601', seq: 11, volume: 1 }))

    expect(store.rows.map(r => r.account_id)).toEqual(['acc_a', 'acc_offline'])
    expect(store.accounts).toEqual(['acc_a', 'acc_offline'])
  })
})

describe('useTdPositionsStore · 分组与展示面（用户确认口径：一行 = 账户×合约）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  it('同一合约多空合成一行，净仓 = 多总量 − 空总量，明细分别保留', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse([
      position({ instrument_id: 'rb2601', direction: 'Long', volume: 5, today_volume: 2, yd_volume: 3 }),
      position({ instrument_id: 'rb2601', direction: 'Short', volume: 2, today_volume: 1, yd_volume: 1 }),
    ]))
    const store = useTdPositionsStore()
    await store.init()

    expect(store.groups).toHaveLength(1)
    const group = store.groups[0]
    expect(group.netVolume).toBe(3)
    expect(group.long?.volume).toBe(5)
    expect(group.short?.volume).toBe(2)
  })

  it('只有单边的合约：另一边明细为 null（详情列显示占位，不显示 0）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse([
      position({ instrument_id: 'rb2601', direction: 'Short', volume: 4 }),
    ]))
    const store = useTdPositionsStore()
    await store.init()

    expect(store.groups[0].netVolume).toBe(-4)
    expect(store.groups[0].long).toBeNull()
    expect(store.groups[0].short?.volume).toBe(4)
  })

  it('账户全集按数据推导：含离线账户（不是 online_accounts）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse(
      [
        position({ account_id: 'acc_a', instrument_id: 'rb2601' }),
        position({ account_id: 'acc_offline', instrument_id: 'ag2601' }),
      ],
      { online_accounts: ['acc_a'] }))
    const store = useTdPositionsStore()
    await store.init()

    expect(store.onlineAccounts).toEqual(['acc_a'])
    expect(store.accounts).toEqual(['acc_a', 'acc_offline'])
  })

  it('anchorDates 去重升序：多账户锚点日不同时面板不得只标一个日期', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse([
      position({ account_id: 'acc_a', date: '20260808' }),
      position({ account_id: 'acc_b', date: '20260807' }),
    ]))
    const store = useTdPositionsStore()
    await store.init()

    expect(store.anchorDates).toEqual(['20260807', '20260808'])
  })
})

describe('useTdPositionsStore · 账户筛选（契约 webui-ws §5 第 11 条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  async function initialized(): Promise<ReturnType<typeof useTdPositionsStore>> {
    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse(
      [
        position({ account_id: 'acc_a', instrument_id: 'rb2601' }),
        position({ account_id: 'acc_b', instrument_id: 'ag2601' }),
      ],
      { online_accounts: ['acc_a', 'acc_b'] }))
    const store = useTdPositionsStore()
    await store.init()
    return store
  }

  it('筛选变化：带 accounts 重拉基线（丢弃游标）', async () => {
    const store = await initialized()

    vi.mocked(tdApi.getPositions).mockResolvedValueOnce(listResponse(
      [position({ account_id: 'acc_b', instrument_id: 'ag2601' })],
      { online_accounts: ['acc_a', 'acc_b'] }))
    await store.setAccountFilter(['acc_b'])

    expect(tdApi.getPositions).toHaveBeenLastCalledWith({
      limit: TD_POSITIONS_PAGE_SIZE,
      accounts: ['acc_b'],
    })
    expect(store.allAccountsSelected).toBe(false)
    expect(store.rows.map(r => r.account_id)).toEqual(['acc_b'])
  })

  it('筛选生效期间不缩小已知账户全集（否则其他账户会从筛选项消失）', async () => {
    const store = await initialized()

    vi.mocked(tdApi.getPositions).mockResolvedValueOnce(listResponse(
      [position({ account_id: 'acc_b', instrument_id: 'ag2601' })],
      { online_accounts: ['acc_a', 'acc_b'] }))
    await store.setAccountFilter(['acc_b'])

    expect(store.accounts).toEqual(['acc_a', 'acc_b'])
    expect(store.effectiveAccounts).toEqual(['acc_b'])
  })

  it('全选恢复：省略 accounts 参数（契约缺省 = 全部已登记账户）', async () => {
    const store = await initialized()

    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse(
      [
        position({ account_id: 'acc_a', instrument_id: 'rb2601' }),
        position({ account_id: 'acc_b', instrument_id: 'ag2601' }),
      ],
      { online_accounts: ['acc_a', 'acc_b'] }))
    await store.setAccountFilter(['acc_b'])
    await store.selectAllAccounts()

    expect(tdApi.getPositions).toHaveBeenLastCalledWith({
      limit: TD_POSITIONS_PAGE_SIZE,
      accounts: undefined,
    })
    expect(store.allAccountsSelected).toBe(true)
  })
})

describe('useTdPositionsStore · 重置触发（data_changed / td_recent）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  async function initialized(): Promise<ReturnType<typeof useTdPositionsStore>> {
    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse([
      position({ instrument_id: 'rb2601', seq: 10 }),
    ]))
    const store = useTdPositionsStore()
    await store.init()
    return store
  }

  it('data_changed{scope:"td"} → 清空重拉基线', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getPositions).mockResolvedValueOnce(listResponse([
      position({ instrument_id: 'ag2601', seq: 20 }),
    ]))

    await store.handleDataChanged('td')
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getPositions).toHaveBeenCalledTimes(2)
    expect(store.rows.map(r => r.instrument_id)).toEqual(['ag2601'])
  })

  it('非 td scope 不触发持仓重拉', async () => {
    const store = await initialized()
    await store.handleDataChanged('market_sources')
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getPositions).toHaveBeenCalledTimes(1)
  })

  it('基线替换语义：REST 没有返回的业务键即已消失（不保留幽灵持仓）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValueOnce(listResponse([
      position({ instrument_id: 'rb2601', seq: 10 }),
      position({ instrument_id: 'ag2601', seq: 11 }),
    ]))
    const store = useTdPositionsStore()
    await store.init()
    expect(store.rows).toHaveLength(2)

    vi.mocked(tdApi.getPositions).mockResolvedValueOnce(listResponse([
      position({ instrument_id: 'rb2601', seq: 10 }),
    ]))
    await store.handleDataChanged('td')

    expect(store.rows.map(r => r.instrument_id)).toEqual(['rb2601'])
  })

  it('基线查询与 WS 增量竞态：比基线更新的本地行保留（同键 seq 大者胜）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValueOnce(listResponse([
      position({ instrument_id: 'rb2601', seq: 10, volume: 2 }),
    ]))
    const store = useTdPositionsStore()
    await store.init()

    // 模拟"重拉期间到达的增量"：桩函数在返回前先灌注一条更大 seq 的行，
    // 而基线内容仍是旧版本（交易库异步持久化，基线查询先于它提交）
    vi.mocked(tdApi.getPositions).mockImplementationOnce(async () => {
      store.applyPositionEvent(position({ instrument_id: 'rb2601', seq: 30, volume: 7 }))
      return listResponse([position({ instrument_id: 'rb2601', seq: 10, volume: 2 })])
    })
    await store.handleDataChanged('td')

    expect(store.rows).toHaveLength(1)
    expect(store.rows[0].seq).toBe(30)
    expect(store.rows[0].volume).toBe(7)
  })

  it('td_recent 到达：认领持仓事件并入 + 重拉基线，其他类型事件忽略', async () => {
    const store = await initialized()
    vi.mocked(tdApi.getPositions).mockResolvedValueOnce(listResponse([
      position({ instrument_id: 'rb2601', seq: 20 }),
      position({ instrument_id: 'ag2601', seq: 21 }),
    ]))

    store.applyRecent({
      events: [
        { type: 'td_position_info', data: position({ instrument_id: 'ag2601', seq: 21 }) },
        // 委托事件不属持仓 store，忽略
        { type: 'td_order_report', data: { order_id: 1 } },
      ],
    })
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getPositions).toHaveBeenCalledTimes(2)
    expect(store.rows.map(r => r.instrument_id)).toEqual(['ag2601', 'rb2601'])
  })

  it('未初始化时 td_recent / data_changed 不发起请求（页面未打开不产生基线流量）', async () => {
    const store = useTdPositionsStore()
    store.applyRecent({ events: [{ type: 'td_position_info', data: position() }] })
    await store.handleDataChanged('td')
    await vi.advanceTimersByTimeAsync(0)

    expect(tdApi.getPositions).not.toHaveBeenCalled()
  })
})

describe('useTdPositionsStore · 降级（契约 rest §2.9.8 / webui-ws §5 第 13 条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  it('available:false → 降级态 + 原因，不显示 0 冒充数据', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue({ available: false, reason: 'not_found' })
    const store = useTdPositionsStore()

    await store.init()

    expect(store.available).toBe(false)
    expect(store.degradedReason).toBe('not_found')
    expect(store.rows).toEqual([])
    expect(store.groups).toEqual([])
    expect(store.isEmpty).toBe(false)
    expect(store.degradedText).toContain('交易数据')
  })

  it('降级后按退避重试，恢复后回到可用态', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdPositionsStore()
    await store.init()
    expect(store.available).toBe(false)

    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse([position()]))
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)

    expect(store.available).toBe(true)
    expect(store.degradedReason).toBeNull()
    expect(store.rows).toHaveLength(1)
    expect(store.degradedText).toBe('')
  })

  it('卸载再挂载（dispose → init）后重新武装退避并从首档重试', async () => {
    // 首轮挂载即降级：重试一次后 attempt 升到 1，下一档退避 = 2×base
    vi.mocked(tdApi.getPositions).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdPositionsStore()
    await store.init()
    expect(tdApi.getPositions).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)
    expect(tdApi.getPositions).toHaveBeenCalledTimes(2)
    // 第二档是 2×base：只推进 base 不应再触发（证明 attempt 确实已递增）
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)
    expect(tdApi.getPositions).toHaveBeenCalledTimes(2)

    // 卸载再挂载：新生命周期必须重新武装定时器，并把退避档位复位到首档
    store.dispose()
    await store.init()
    expect(tdApi.getPositions).toHaveBeenCalledTimes(3)
    vi.mocked(tdApi.getPositions).mockResolvedValue(listResponse([position({ seq: 20 })]))
    // 若沿用卸载前的档位（2×base），此处只推进 base 不会重试
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS)

    expect(tdApi.getPositions).toHaveBeenCalledTimes(4)
    expect(store.available).toBe(true)
    expect(store.rows).toHaveLength(1)
  })

  it('降级期间 WS 事件照常合并（不丢增量）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdPositionsStore()
    await store.init()

    store.applyPositionEvent(position({ instrument_id: 'rb2601', seq: 42 }))

    expect(store.rows.map(r => r.instrument_id)).toEqual(['rb2601'])
    expect(store.available).toBe(false)
  })

  it('dispose 清空定时器（卸载页面后不再请求）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue({ available: false, reason: 'busy' })
    const store = useTdPositionsStore()
    await store.init()

    store.dispose()
    await vi.advanceTimersByTimeAsync(TD_RETRY_BASE_DELAY_MS * 10)

    expect(tdApi.getPositions).toHaveBeenCalledTimes(1)
  })
})
