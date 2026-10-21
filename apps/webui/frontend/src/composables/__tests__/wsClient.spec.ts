import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { useAuthStore } from '@/stores/auth'

// wsClient 是模块级单例：状态（ws / 重连次数 / seq）跨用例残留，故每例 resetModules
// 后重新 import。fake timers 覆盖心跳与重连退避，测试不依赖真实时钟。

class FakeWebSocket {
  static instances: FakeWebSocket[] = []
  static OPEN = 1
  static CONNECTING = 0
  readyState = FakeWebSocket.CONNECTING
  url: string
  sent: string[] = []
  closed = false
  onopen: (() => void) | null = null
  onmessage: ((e: { data: string | Blob }) => void) | null = null
  onerror: (() => void) | null = null
  onclose: (() => void) | null = null
  constructor(url: string) {
    this.url = url
    FakeWebSocket.instances.push(this)
  }
  send(data: string): void {
    this.sent.push(data)
  }
  close(): void {
    if (this.closed) return
    this.closed = true
    this.readyState = 3
    this.onclose?.()
  }
  /** 模拟服务端/网络侧断开：不经过 manualClose 路径 */
  serverClose(): void {
    if (this.closed) return
    this.closed = true
    this.readyState = 3
    this.onclose?.()
  }
  simulateOpen(): void {
    this.readyState = FakeWebSocket.OPEN
    this.onopen?.()
  }
  simulateError(): void {
    this.onerror?.()
  }
}

vi.stubGlobal('WebSocket', FakeWebSocket as unknown as typeof WebSocket)

describe('wsClient 连接生命周期与消息协议', () => {
  beforeEach(() => {
    vi.resetModules()
    vi.useFakeTimers()
    setActivePinia(createPinia())
    // afterEach 的 unstubAllGlobals 会移除顶层 stub，须在每例前置以恢复 FakeWebSocket
    vi.stubGlobal('WebSocket', FakeWebSocket as unknown as typeof WebSocket)
    FakeWebSocket.instances = []
    localStorage.setItem('jwt_token', 'test-token')
    // onopen 的副作用（REST 补拉列表 / 刷新当前用户）不应触网
    vi.spyOn(useMarketSourcesStore(), 'loadSources').mockResolvedValue()
    vi.spyOn(useAuthStore(), 'refreshCurrentUser').mockResolvedValue()
  })

  afterEach(() => {
    vi.useRealTimers()
    vi.unstubAllGlobals()
    localStorage.clear()
  })

  async function loadClient() {
    return await import('@/composables/wsClient')
  }

  function textMessage(ws: FakeWebSocket, message: unknown): void {
    ws.onmessage?.({ data: JSON.stringify(message) })
  }

  it('connect 建立连接并在 onopen 后置 connected；URL 携带 JWT', async () => {
    const { useWebSocket } = await loadClient()
    const client = useWebSocket()
    expect(client.connectionState.value).toBe('disconnected')

    client.connect()
    const ws = FakeWebSocket.instances[0]!
    expect(client.connectionState.value).toBe('connecting')
    expect(ws.url).toContain('/ws?token=test-token')

    ws.simulateOpen()
    expect(client.connectionState.value).toBe('connected')
    expect(client.lastError.value).toBeNull()
    await vi.advanceTimersByTimeAsync(0)
    expect(useMarketSourcesStore().loadSources).toHaveBeenCalledTimes(1)
    expect(useAuthStore().refreshCurrentUser).toHaveBeenCalledTimes(1)
  })

  it('无 token 直接 failed 并记录错误，不创建连接', async () => {
    localStorage.removeItem('jwt_token')
    const { connect, useWebSocket } = await loadClient()
    connect()
    expect(useWebSocket().connectionState.value).toBe('failed')
    expect(useWebSocket().lastError.value).toBe('No JWT token')
    expect(FakeWebSocket.instances).toHaveLength(0)
  })

  it('onerror 记录 lastError，连接保持', async () => {
    const { connect, useWebSocket } = await loadClient()
    connect()
    FakeWebSocket.instances[0]!.simulateError()
    expect(useWebSocket().lastError.value).toBe('WebSocket error')
  })

  it('断线按 3s×2ⁿ 退避重连（3s → 6s）', async () => {
    const { connect, useWebSocket } = await loadClient()
    connect()
    FakeWebSocket.instances[0]!.serverClose()
    expect(useWebSocket().connectionState.value).toBe('reconnecting')
    expect(FakeWebSocket.instances).toHaveLength(1)

    await vi.advanceTimersByTimeAsync(2_999)
    expect(FakeWebSocket.instances).toHaveLength(1)  // 未到 3s 不重连
    await vi.advanceTimersByTimeAsync(1)
    expect(FakeWebSocket.instances).toHaveLength(2)

    // 第二次仍未 open 即断开 → 下一次延迟翻倍为 6s
    FakeWebSocket.instances[1]!.serverClose()
    await vi.advanceTimersByTimeAsync(5_999)
    expect(FakeWebSocket.instances).toHaveLength(2)
    await vi.advanceTimersByTimeAsync(1)
    expect(FakeWebSocket.instances).toHaveLength(3)
  })

  it('重连成功重置退避计数；再次断线从 3s 重新计', async () => {
    const { connect, useWebSocket } = await loadClient()
    connect()
    FakeWebSocket.instances[0]!.serverClose()
    await vi.advanceTimersByTimeAsync(3_000)
    FakeWebSocket.instances[1]!.simulateOpen()
    expect(useWebSocket().connectionState.value).toBe('connected')

    FakeWebSocket.instances[1]!.serverClose()
    await vi.advanceTimersByTimeAsync(2_999)
    expect(FakeWebSocket.instances).toHaveLength(2)
    await vi.advanceTimersByTimeAsync(1)
    expect(FakeWebSocket.instances).toHaveLength(3)
  })

  it('5 次重连耗尽进入 failed，30s 后归零重试', async () => {
    const { connect, useWebSocket } = await loadClient()
    connect()
    FakeWebSocket.instances[0]!.serverClose()

    // delays: 3s, 6s, 12s, 24s, 48s（对应 attempts 0..4）
    for (const delay of [3_000, 6_000, 12_000, 24_000, 48_000]) {
      await vi.advanceTimersByTimeAsync(delay)
      const latest = FakeWebSocket.instances[FakeWebSocket.instances.length - 1]!
      latest.serverClose()
    }
    expect(useWebSocket().connectionState.value).toBe('failed')
    expect(FakeWebSocket.instances).toHaveLength(6)  // 首连 + 5 次重连

    await vi.advanceTimersByTimeAsync(29_999)
    expect(FakeWebSocket.instances).toHaveLength(6)
    await vi.advanceTimersByTimeAsync(1)
    expect(FakeWebSocket.instances).toHaveLength(7)  // 30s 恢复重建
    expect(useWebSocket().connectionState.value).toBe('connecting')
  })

  it('手动 disconnect 不重连，状态回 disconnected', async () => {
    const { connect, disconnect, useWebSocket } = await loadClient()
    connect()
    FakeWebSocket.instances[0]!.simulateOpen()
    disconnect()
    expect(useWebSocket().connectionState.value).toBe('disconnected')

    await vi.advanceTimersByTimeAsync(120_000)
    expect(FakeWebSocket.instances).toHaveLength(1)
    expect(useWebSocket().connectionState.value).toBe('disconnected')
  })

  it('收到 pong 前持续发 ping，正常应答不关闭', async () => {
    const { connect } = await loadClient()
    connect()
    const ws = FakeWebSocket.instances[0]!
    ws.simulateOpen()

    vi.advanceTimersByTime(30_000)
    expect(ws.sent.filter(m => m.includes('ping'))).toHaveLength(1)

    textMessage(ws, { type: 'pong' })

    vi.advanceTimersByTime(30_000)
    expect(ws.sent.filter(m => m.includes('ping'))).toHaveLength(2)
    expect(ws.closed).toBe(false)
  })

  it('超过阈值未收到 pong → 主动 close（半开连接探活）', async () => {
    const { connect } = await loadClient()
    connect()
    const ws = FakeWebSocket.instances[0]!
    ws.simulateOpen()

    // lastPongAt 在 onopen=0s 重置；HEARTBEAT_STALE_MS=65s。
    // t=30s：差31s，发 ping；t=60s：差61s，仍发 ping；t=90s：差91s>65s → close
    vi.advanceTimersByTime(90_000)
    expect(ws.sent.filter(m => m.includes('ping'))).toHaveLength(2)
    expect(ws.closed).toBe(true)
  })

  it('S2C 分派：data 优先于 payload，并透传 instance_id', async () => {
    const { connect, registerHandler } = await loadClient()
    const handler = vi.fn()
    registerHandler('progress', handler)
    connect()
    const ws = FakeWebSocket.instances[0]!

    textMessage(ws, {
      type: 'progress',
      data: { min: 0, max: 1, current: 1 },
      payload: { min: 9, max: 9, current: 9 },
      instance_id: 'dzmd_ctp',
    })
    expect(handler).toHaveBeenCalledWith({ min: 0, max: 1, current: 1 }, 'dzmd_ctp')
  })

  it('S2C 分派：无 data 时回退 payload（控制消息）', async () => {
    const { connect, registerHandler } = await loadClient()
    const handler = vi.fn()
    registerHandler('data_changed', handler)
    connect()
    const ws = FakeWebSocket.instances[0]!

    textMessage(ws, { type: 'data_changed', payload: { scope: 'users' } })
    expect(handler).toHaveBeenCalledWith({ scope: 'users' }, undefined)
  })

  it('error 消息只写 lastError，不进入业务 handler；未知 type 静默忽略', async () => {
    const { connect, registerHandler, useWebSocket } = await loadClient()
    const errorHandler = vi.fn()
    const unknownHandler = vi.fn()
    registerHandler('error', errorHandler)
    registerHandler('notify_ui', unknownHandler)
    connect()
    const ws = FakeWebSocket.instances[0]!

    textMessage(ws, { type: 'error', payload: { message: 'auth failed' } })
    expect(useWebSocket().lastError.value).toBe('auth failed')
    expect(errorHandler).not.toHaveBeenCalled()

    textMessage(ws, { type: 'not_a_real_type', data: { x: 1 } })
    expect(unknownHandler).not.toHaveBeenCalled()
  })

  it('C2S 报文形状：type/seq 递增/payload', async () => {
    const { connect, mdConnect, mdDisconnect, subscribeLog, unsubscribeLog } = await loadClient()
    connect()
    const ws = FakeWebSocket.instances[0]!
    ws.simulateOpen()

    mdConnect('dzmd_ctp')
    mdDisconnect('dzmd_ctp')
    subscribeLog('/logs/trade.log')
    unsubscribeLog()

    expect(ws.sent.map(raw => JSON.parse(raw))).toEqual([
      { type: 'md_connect', seq: 1, payload: { source: 'dzmd_ctp' } },
      { type: 'md_disconnect', seq: 2, payload: { source: 'dzmd_ctp' } },
      { type: 'subscribe_log', seq: 3, payload: { file: '/logs/trade.log' } },
      { type: 'unsubscribe_log', seq: 4 },
    ])
  })

  it('非 OPEN 状态不发送 C2S 报文', async () => {
    const { connect, mdConnect, mdDisconnect, subscribeLog, unsubscribeLog } = await loadClient()
    connect()
    const ws = FakeWebSocket.instances[0]!  // 仍 CONNECTING

    mdConnect('dzmd_ctp')
    mdDisconnect('dzmd_ctp')
    subscribeLog('/logs/trade.log')
    unsubscribeLog()
    expect(ws.sent).toHaveLength(0)
  })

  it('blob 消息解析失败不产生未处理拒绝', async () => {
    const { connect } = await loadClient()
    connect()
    const ws = FakeWebSocket.instances[0]!
    ws.simulateOpen()
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})

    // 真实 Blob 实例才能通过 instanceof Blob 分支；jsdom Blob 无 text 方法，直接覆盖使其 reject
    const badBlob = new Blob(['payload'])
    badBlob.text = () => Promise.reject(new Error('decode failed'))
    ws.onmessage?.({ data: badBlob })
    await vi.advanceTimersByTimeAsync(0)
    expect(warn).toHaveBeenCalled()

    warn.mockRestore()
  })
})
