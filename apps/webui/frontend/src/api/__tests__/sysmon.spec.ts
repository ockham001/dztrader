import { describe, it, expect, vi, afterEach } from 'vitest'
import { sysmonApi } from '../sysmon'

// 手动时钟查询的**路径与方法**（契约 rest §2.8 / 契约 sysmon §1.1）：
// 响应仅表示「已写事件通道」，生效信号是随后的 WS `sysmon_snapshot`（clock 域更新）；
// 限流窗口内 dzsysmon 直接忽略且无错误响应。这里只断言可观察的请求，不 mock 业务层。
describe('sysmonApi 手动时钟查询端点（契约 rest §2.8）', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    localStorage.clear()
  })

  function stubFetch(): ReturnType<typeof vi.fn> {
    const fetchMock = vi.fn(async () => new Response('{"ok":true,"message":"dispatched"}', {
      status: 200,
      headers: { 'Content-Type': 'application/json' },
    }))
    vi.stubGlobal('fetch', fetchMock)
    return fetchMock
  }

  it('probeClock 以 POST 打 /api/sysmon/clock/probe（不是配置端点）', async () => {
    const fetchMock = stubFetch()

    const result = await sysmonApi.probeClock()

    expect(fetchMock).toHaveBeenCalledTimes(1)
    expect(fetchMock.mock.calls[0][0]).toBe('/api/sysmon/clock/probe')
    expect(fetchMock.mock.calls[0][1]).toMatchObject({ method: 'POST' })
    expect(result).toEqual({ ok: true, message: 'dispatched' })
  })
})
