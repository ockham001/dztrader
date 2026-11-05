import { describe, it, expect, vi, afterEach } from 'vitest'
import { tdApi } from '../td'

// 交易数据读端点的**路径与 query string**（契约 rest §2.9.2）：
// 委托与成交共用同一份参数编码，端点路径不同。这里断言可观察的请求 URL，
// 不 mock `@/api/client` 之外的东西（其余行为由 store 用例覆盖）。
describe('tdApi 交易数据端点（契约 rest §2.9）', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    localStorage.clear()
  })

  function stubFetch(): ReturnType<typeof vi.fn> {
    const fetchMock = vi.fn(async () => new Response('{}', {
      status: 200,
      headers: { 'Content-Type': 'application/json' },
    }))
    vi.stubGlobal('fetch', fetchMock)
    return fetchMock
  }

  it('getTrades 打 /api/td/trades（不是委托端点）', async () => {
    const fetchMock = stubFetch()

    await tdApi.getTrades()

    expect(fetchMock).toHaveBeenCalledTimes(1)
    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/trades')
  })

  it('getTrades 的 query 编码与 getOrders 同源（limit/cursor/accounts）', async () => {
    const fetchMock = stubFetch()

    await tdApi.getTrades({ limit: 200, cursor: 'c1', accounts: [' acc_b ', '', 'acc_a'] })

    expect(fetchMock.mock.calls[0][0])
      .toBe('/api/td/trades?limit=200&cursor=c1&accounts=acc_b%2Cacc_a')
  })

  it('缺省参数不拼 query string（后端按「未给」处理）', async () => {
    const fetchMock = stubFetch()

    await tdApi.getTrades({ accounts: [] })

    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/trades')
  })

  it('getOrders 仍打委托端点（成交接入没有改坏委托路径）', async () => {
    const fetchMock = stubFetch()

    await tdApi.getOrders({ limit: 200 })

    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/orders?limit=200')
  })

  it('getPositions 打 /api/td/positions（不是委托/成交端点）', async () => {
    const fetchMock = stubFetch()

    await tdApi.getPositions()

    expect(fetchMock).toHaveBeenCalledTimes(1)
    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/positions')
  })

  it('getPositions 的 query 编码与 getOrders/getTrades 同源（limit/cursor/accounts）', async () => {
    const fetchMock = stubFetch()

    await tdApi.getPositions({ limit: 200, cursor: 'c1', accounts: [' acc_b ', '', 'acc_a'] })

    expect(fetchMock.mock.calls[0][0])
      .toBe('/api/td/positions?limit=200&cursor=c1&accounts=acc_b%2Cacc_a')
  })

  it('getTradingAccounts 打 /api/td/trading_accounts（资金端点，不分页）', async () => {
    const fetchMock = stubFetch()

    await tdApi.getTradingAccounts()

    expect(fetchMock).toHaveBeenCalledTimes(1)
    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/trading_accounts')
  })

  it('getTradingAccounts 只编码 accounts，不发送 limit/cursor（端点忽略分页参数）', async () => {
    const fetchMock = stubFetch()

    await tdApi.getTradingAccounts({ accounts: [' acc_b ', '', 'acc_a'] })

    expect(fetchMock.mock.calls[0][0])
      .toBe('/api/td/trading_accounts?accounts=acc_b%2Cacc_a')
  })
})
