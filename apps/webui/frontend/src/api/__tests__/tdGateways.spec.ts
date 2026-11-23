import { describe, it, expect, vi, afterEach } from 'vitest'
import {
  TD_GATEWAY_ERROR_TEXT,
  TD_GATEWAY_NETWORK_ERROR_TEXT,
  tdGatewayErrorText,
  tdGatewaysApi,
  tdGatewayServerErrorText,
} from '../tdGateways'

// 交易接口进程生命周期端点（#439；契约 rest §2.10）：
// 断言可观察的请求方法、URL（含路径参数编码）与 body 形状；错误码 → 中文文案单一真相源。
describe('tdGatewaysApi 交易接口生命周期端点（契约 rest §2.10）', () => {
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

  it('listAvailable 打 GET /api/td/gateways/available（卡片列表不靠它，添加下拉用）', async () => {
    const fetchMock = stubFetch([{ name: 'dztd_ctp', display_name: 'CTP 交易接口', added: false, running: false }])

    const list = await tdGatewaysApi.listAvailable()

    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/gateways/available')
    expect((fetchMock.mock.calls[0][1] as RequestInit).method).toBeUndefined() // GET 不带 method
    expect(list[0].name).toBe('dztd_ctp')
  })

  it('start 打 POST .../start，带 display_name 时按契约 body 下发', async () => {
    const fetchMock = stubFetch({ ok: true })

    await tdGatewaysApi.start('dztd_ctp', { display_name: 'CTP 交易接口' })

    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/gateways/dztd_ctp/start')
    expect((fetchMock.mock.calls[0][1] as RequestInit).method).toBe('POST')
    expect(JSON.parse((fetchMock.mock.calls[0][1] as RequestInit).body as string))
      .toEqual({ display_name: 'CTP 交易接口' })
  })

  it('start 不带显示名时下发空对象 body（body 可选，契约 §2.10）', async () => {
    const fetchMock = stubFetch({ ok: true })

    await tdGatewaysApi.start('dztd_ctp')

    expect(JSON.parse((fetchMock.mock.calls[0][1] as RequestInit).body as string)).toEqual({})
  })

  it('stop 打 POST .../stop', async () => {
    const fetchMock = stubFetch({ ok: true })

    await tdGatewaysApi.stop('dztd_ctp')

    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/gateways/dztd_ctp/stop')
    expect((fetchMock.mock.calls[0][1] as RequestInit).method).toBe('POST')
  })

  it('remove 打 DELETE .../{name} 且无 body（契约 §2.10）', async () => {
    const fetchMock = stubFetch({ ok: true })

    await tdGatewaysApi.remove('dztd_ctp')

    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/gateways/dztd_ctp')
    expect((fetchMock.mock.calls[0][1] as RequestInit).method).toBe('DELETE')
    expect((fetchMock.mock.calls[0][1] as RequestInit).body).toBeUndefined()
  })

  it('路径参数做 URL 编码（进程名含特殊字符时不产生路径穿越）', async () => {
    const fetchMock = stubFetch({ ok: true })

    await tdGatewaysApi.start('dztd_a/b')

    expect(fetchMock.mock.calls[0][0]).toBe('/api/td/gateways/dztd_a%2Fb/start')
  })
})

describe('交易接口错误码 → 中文文案（契约 rest §2.10 预检清单）', () => {
  it('三个特有 code 各有独立中文文案，且不回显 snake_case / 英文 error', () => {
    const codes = ['invalid_gateway', 'gateway_not_found', 'gateway_has_accounts'] as const
    const texts = codes.map(code => tdGatewayErrorText(code))
    for (const text of texts) {
      expect(text).not.toBe('')
      expect(text).not.toContain('_')
      expect(text).not.toBe(TD_GATEWAY_ERROR_TEXT.admin_required)
    }
    // 文案互不相同：用户要能据此判断下一步动作（换接口 / 刷新 / 先删账户）
    expect(new Set(texts).size).toBe(codes.length)
    expect(tdGatewayErrorText('gateway_has_accounts')).toContain('先删除全部账户')
    expect(tdGatewayErrorText('gateway_not_found')).toContain('不存在')
  })

  it('未知 code / 非字符串 code 兜底（不泄露原始值）', () => {
    expect(tdGatewayErrorText('some_future_code')).toBe(tdGatewayErrorText(undefined))
    expect(tdGatewayErrorText(42)).toBe(tdGatewayErrorText(undefined))
    expect(tdGatewayErrorText(undefined)).not.toContain('some_future_code')
  })

  it('有响应但不带 code：503 → 事件通道文案，不是网络错误也不是预检失败', () => {
    const text = tdGatewayServerErrorText(503)
    expect(text).toContain('暂不可用')
    expect(text).not.toBe(TD_GATEWAY_NETWORK_ERROR_TEXT)
    expect(text).not.toBe(TD_GATEWAY_ERROR_TEXT.invalid_gateway)
    // 401/403 复用登录/权限口径
    expect(tdGatewayServerErrorText(403)).toBe(TD_GATEWAY_ERROR_TEXT.admin_required)
    expect(tdGatewayServerErrorText(401)).toContain('登录')
  })

  it('网络文案独立（无 HTTP 响应）', () => {
    expect(TD_GATEWAY_NETWORK_ERROR_TEXT).toContain('无法连接服务器')
  })
})
