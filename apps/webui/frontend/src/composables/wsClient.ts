import { ref, readonly } from 'vue'
import type { Ref } from 'vue'
import { useMarketSourcesStore } from '@/stores/marketSources'

export type WsConnectionState = 'disconnected' | 'connecting' | 'connected' | 'reconnecting' | 'failed'
export type WsHandler = (payload: unknown, instanceId?: string) => void

// 业务消息处理注册表：由 wsHandlers.ts 在模块加载时集中注册（注册表分发，替代原 switch）
// pong/error 为连接协议消息，留在本模块内部处理（不注册）
const handlers = new Map<string, WsHandler>()

export function registerHandler(type: string, handler: WsHandler): void {
  handlers.set(type, handler)
}

export function unregisterHandler(type: string): void {
  handlers.delete(type)
}

// Module-level singleton state
let ws: WebSocket | null = null
let heartbeatTimer: ReturnType<typeof setInterval> | null = null
let reconnectTimer: ReturnType<typeof setTimeout> | null = null
let recoveryTimer: ReturnType<typeof setTimeout> | null = null
let reconnectAttempts = 0
let seqCounter = 0
let manualClose = false

const MAX_RECONNECT_ATTEMPTS = 5
const HEARTBEAT_INTERVAL_MS = 30_000
const RECONNECT_BASE_DELAY_MS = 3_000
const RECOVERY_DELAY_MS = 30_000

const connectionState = ref<WsConnectionState>('disconnected')
const lastError = ref<string | null>(null)

function getToken(): string {
  return localStorage.getItem('jwt_token') || ''
}

function buildUrl(): string {
  const token = getToken()
  const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${proto}//${window.location.host}/ws?token=${encodeURIComponent(token)}`
}

function startHeartbeat(): void {
  stopHeartbeat()
  heartbeatTimer = setInterval(() => {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'ping' }))
    }
  }, HEARTBEAT_INTERVAL_MS)
}

function stopHeartbeat(): void {
  if (heartbeatTimer) {
    clearInterval(heartbeatTimer)
    heartbeatTimer = null
  }
}

function scheduleReconnect(): void {
  if (manualClose) return
  if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
    connectionState.value = 'failed'
    // 设计 §5.1：failed 终态 30s 后重置退避重试（7×24 下 dzweb 重启可自愈，无需人工刷新）
    if (recoveryTimer) clearTimeout(recoveryTimer)
    recoveryTimer = setTimeout(() => { reconnectAttempts = 0; connect() }, RECOVERY_DELAY_MS)
    return
  }
  const delay = RECONNECT_BASE_DELAY_MS * Math.pow(2, reconnectAttempts)
  reconnectAttempts++
  connectionState.value = 'reconnecting'
  reconnectTimer = setTimeout(() => {
    connect()
  }, delay)
}

function handleMessage(event: MessageEvent): void {
  // Blob — convert to text then parse
  if (event.data instanceof Blob) {
    event.data.text().then((text) => {
      handleTextMessage(text)
    })
    return
  }

  // Text message
  if (typeof event.data === 'string') {
    handleTextMessage(event.data)
  }
}

function handleTextMessage(text: string): void {
  try {
    const msg = JSON.parse(text) as { type: string; data?: unknown; payload?: unknown; instance_id?: string }
    if (msg.type === 'pong') return                      // 心跳应答
    if (msg.type === 'error') {                          // 连接协议错误
      if (msg.payload && typeof msg.payload === 'object' && 'message' in msg.payload) {
        lastError.value = String((msg.payload as { message: unknown }).message)
      }
      return
    }
    const payload = msg.data ?? msg.payload              // data 键（领域消息）优先，payload 键（控制消息）兜底
    const handler = handlers.get(msg.type)
    if (handler) handler(payload, msg.instance_id)
    // 未注册类型静默忽略（对应原 default: break）
  } catch { /* Ignore JSON parse errors */ }
}

export function connect(): void {
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
    return
  }

  manualClose = false
  const token = getToken()
  if (!token) {
    connectionState.value = 'failed'
    lastError.value = 'No JWT token'
    return
  }

  connectionState.value = reconnectAttempts > 0 ? 'reconnecting' : 'connecting'

  try {
    ws = new WebSocket(buildUrl())
  } catch {
    scheduleReconnect()
    return
  }

  ws.binaryType = 'arraybuffer'

  ws.onopen = () => {
    connectionState.value = 'connected'
    reconnectAttempts = 0
    lastError.value = null
    startHeartbeat()
    // 重连补拉简化（P6，设计 §5.1）：后端连接即推全量 snapshot，snapshot 领域分发
    // 复用增量帧同一 apply 函数（process_config/process_status/md_config/md_status/progress），
    // 已承接配置镜像恢复与"清 pending"副作用，故不再需要：
    //   - refreshConfigs()（逐 source GET config 补拉 md_rtn_config）→ 冗余
    //   - marketSourcesApi.refresh()（触发后端 QUERY_ALL）→ 冗余
    // 仅保留行情源列表 REST 拉取（快照 vs REST 职责分工 §5.1：列表以 DB 为真相源，
    // snapshot 不重建列表）
    const store = useMarketSourcesStore()
    store.loadSources().catch((err: unknown) => {
      console.warn('reconnect loadSources failed', err)
    })
  }

  ws.onmessage = handleMessage

  ws.onerror = () => {
    lastError.value = 'WebSocket error'
  }

  ws.onclose = () => {
    stopHeartbeat()
    ws = null
    if (!manualClose) {
      scheduleReconnect()
    } else {
      connectionState.value = 'disconnected'
    }
  }
}

export function disconnect(): void {
  manualClose = true
  stopHeartbeat()
  if (reconnectTimer) {
    clearTimeout(reconnectTimer)
    reconnectTimer = null
  }
  if (recoveryTimer) {
    clearTimeout(recoveryTimer)
    recoveryTimer = null
  }
  reconnectAttempts = 0
  if (ws) {
    ws.close()
    ws = null
  }
  connectionState.value = 'disconnected'
}

function send(message: Record<string, unknown>): void {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(message))
  }
}

export function mdConnect(source: string): void {
  seqCounter++
  send({ type: 'md_connect', seq: seqCounter, payload: { source } })
}

export function mdDisconnect(source: string): void {
  seqCounter++
  send({ type: 'md_disconnect', seq: seqCounter, payload: { source } })
}

export function subscribeLog(file: string): void {
  seqCounter++
  send({ type: 'subscribe_log', seq: seqCounter, payload: { file } })
}

export function unsubscribeLog(): void {
  seqCounter++
  send({ type: 'unsubscribe_log', seq: seqCounter })
}

export function useWebSocket(): {
  connectionState: Readonly<Ref<WsConnectionState>>
  lastError: Readonly<Ref<string | null>>
  connect: typeof connect
  disconnect: typeof disconnect
  mdConnect: typeof mdConnect
  mdDisconnect: typeof mdDisconnect
  subscribeLog: typeof subscribeLog
  unsubscribeLog: typeof unsubscribeLog
} {
  return {
    connectionState: readonly(connectionState),
    lastError: readonly(lastError),
    connect,
    disconnect,
    mdConnect,
    mdDisconnect,
    subscribeLog,
    unsubscribeLog,
  }
}
