import { describe, it, expect, vi, afterEach } from 'vitest'
import { tdApi } from '../td'

// 交易账户配置端点（#251；契约 rest §2.9.7/§2.9.9）：
// 断言可观察的请求方法与 URL（含路径参数编码）、body 形状，以及「PUT 不得携带 account_id」。
describe('tdApi 交易账户配置端点（契约 rest §2.9.7/§2.9.9）', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    localStorage.clear()
  })

  function stubFetch(responseBody: unknown = {}): ReturnType<typeof vi.fn> {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify(responseBody), {
      status: 200,
      headers: { 'Content-Type': 'application/json' },
    }))
    vi.stubGlobal('fetch', fetchMock)
    return fetchMock
  }

  it('listAccounts 打 GET /api/td/accounts（注册表读端点）', async () => {
    const fetchMock = stubFetch({ available: true, accounts: [] })

    await tdApi.listAccounts()

    expect(fetchMock).toHaveBeenCalledTimes(1)
    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/accounts')
    expect((fetchMock.mock.calls[0][1] as RequestInit).method).toBeUndefined() // GET 不带 method
  })

  it('addAccount 打 POST /api/td/accounts，body 为 {gateway, account}（契约 §2.9.7）', async () => {
    const fetchMock = stubFetch({ ok: true })
    const body = {
      gateway: 'dztd_ctp',
      account: {
        account_id: 'account_001',
        enabled: true,
        ext: { broker_id: '9999', user_id: '00001', password: '123456' },
      },
    }

    await tdApi.addAccount(body)

    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/accounts')
    expect((fetchMock.mock.calls[0][1] as RequestInit).method).toBe('POST')
    expect(JSON.parse((fetchMock.mock.calls[0][1] as RequestInit).body as string)).toEqual(body)
  })

  it('updateAccount 打 PUT /api/td/accounts/{id}，路径参数做 URL 编码，body 不含 account_id', async () => {
    const fetchMock = stubFetch({ ok: true })

    await tdApi.updateAccount('acct/../x', { enabled: false })

    // 路径参数必须编码：不编码时 `../` 会被服务端当成路径穿越（打到别的端点）
    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/accounts/acct%2F..%2Fx')
    expect((fetchMock.mock.calls[0][1] as RequestInit).method).toBe('PUT')
    const body = JSON.parse((fetchMock.mock.calls[0][1] as RequestInit).body as string)
    expect(body).toEqual({ enabled: false })
    expect(body).not.toHaveProperty('account_id')
  })

  it('updateAccount 允许空对象 body（契约 §2.9.7：允许 {}）', async () => {
    const fetchMock = stubFetch({ ok: true })

    await tdApi.updateAccount('account_001', {})

    expect(JSON.parse((fetchMock.mock.calls[0][1] as RequestInit).body as string)).toEqual({})
  })

  it('removeAccount 打 DELETE /api/td/accounts/{id} 且无 body（契约 §2.9.7）', async () => {
    const fetchMock = stubFetch({ ok: true })

    await tdApi.removeAccount('account_001')

    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/accounts/account_001')
    expect((fetchMock.mock.calls[0][1] as RequestInit).method).toBe('DELETE')
    expect((fetchMock.mock.calls[0][1] as RequestInit).body).toBeUndefined()
  })
})
