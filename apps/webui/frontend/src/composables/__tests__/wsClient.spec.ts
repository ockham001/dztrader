import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'

// I5：心跳必须探活——连续未收到 pong 超过阈值时主动 close 触发重连。
// 仅靠 onclose 无法感知半开（对端不可达但 TCP 未断、onclose 不触发），
// 因此心跳必须双向校验：只发 ping 而不同步验证 pong 等于无探活。
// FakeWebSocket：open 后 send 只记录不回包（模拟半开），close 时置 3 并触发 onclose

class FakeWebSocket {
  static instances: FakeWebSocket[] = []
  static OPEN = 1
  static CONNECTING = 0
  readyState = FakeWebSocket.CONNECTING
  sent: string[] = []
  closed = false
  onopen: (() => void) | null = null
  onmessage: ((e: { data: string }) => void) | null = null
  onerror: (() => void) | null = null
  onclose: (() => void) | null = null
  constructor() {
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
  simulateOpen(): void {
    this.readyState = FakeWebSocket.OPEN
    this.onopen?.()
  }
}

vi.stubGlobal('WebSocket', FakeWebSocket as unknown as typeof WebSocket)

describe('wsClient 心跳探活', () => {
  beforeEach(() => {
    vi.resetModules()
    vi.useFakeTimers()
    setActivePinia(createPinia())
    // afterEach 的 unstubAllGlobals 会移除顶层 stub，须在每例前置以恢复 FakeWebSocket
    vi.stubGlobal('WebSocket', FakeWebSocket as unknown as typeof WebSocket)
    FakeWebSocket.instances = []
    localStorage.setItem('jwt_token', 'test-token')
  })

  afterEach(() => {
    vi.useRealTimers()
    vi.unstubAllGlobals()
  })

  async function loadClient() {
    return await import('@/composables/wsClient')
  }

  it('收到 pong 前持续发 ping，正常应答不关闭', async () => {
    const { connect } = await loadClient()
    connect()
    const ws = FakeWebSocket.instances[0]!
    ws.simulateOpen()

    vi.advanceTimersByTime(30_000)
    expect(ws.sent.filter(m => m.includes('ping'))).toHaveLength(1)

    ws.onmessage?.({ data: JSON.stringify({ type: 'pong' }) })

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

  it('blob 消息解析失败不产生未处理拒绝', async () => {
    const { connect } = await loadClient()
    connect()
    const ws = FakeWebSocket.instances[0]!
    ws.simulateOpen()
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})

    // 真实 Blob 实例才能通过 instanceof Blob 分支；jsdom Blob 无 text 方法，直接覆盖使其 reject
    const badBlob = new Blob(['payload'])
    badBlob.text = () => Promise.reject(new Error('decode failed'))
    ws.onmessage?.({ data: badBlob as unknown as string })
    await vi.advanceTimersByTimeAsync(0)
    expect(warn).toHaveBeenCalled()

    warn.mockRestore()
  })
})