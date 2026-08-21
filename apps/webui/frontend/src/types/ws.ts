// ===== WebSocket 消息类型（服务端 → 客户端 / 客户端 → 服务端）=====
// 自 composables/useWebSocket.ts 的 ServerMessage 迁入并扩展（P3 Task 1）
// 判别联合：data 类型经 WsDataByType 按 type 映射；真源 = 契约单源（schema → types/generated.ts）。
import type {
  ProcessStatusPayload, ProgressStatus, LogConfigPayload, AutoLoginPayload, NotifyUiPayload,
} from './generated'

// 服务端 → 客户端每类消息的 data 载荷类型（判别联合核心）
// 已收敛的用生成领域类型；未迁移的复杂域暂以 unknown，随 P1 迁移逐步收敛
export interface WsDataByType {
  snapshot: unknown                              // 结构特殊：Record<instance_id, Partial<DomainPayloads>>（镜像）
  process_status: ProcessStatusPayload
  process_config: Record<string, unknown>
  md_rtn_config: unknown                         // 待迁复杂域（契约 md-config）
  md_rtn_status: unknown
  md_rtn_subscriptions: unknown
  notify_ui: NotifyUiPayload
  log_config: LogConfigPayload
  log_line: unknown                              // 待迁复杂域（契约 webui-ws log_line）
  log_tail_unsubscribed: unknown
  data_changed: unknown
  default_password_warning: unknown
  error: { message: string }
  pong: void
  event_shm_config: unknown                      // 待迁（契约 shm）
  md_shm_config: unknown
  auto_login: AutoLoginPayload
  progress: ProgressStatus
}

// 服务端 → 客户端消息 type 联合（= WsDataByType 的 key）
export type WsServerMessageType = keyof WsDataByType

export interface WsServerMessage<T extends WsServerMessageType = WsServerMessageType> {
  type: T
  data?: WsDataByType[T]   // 领域消息（data 键）
  payload?: unknown        // 控制消息（payload 键）
  instance_id?: string
}

// 客户端 → 服务端
export type WsClientMessageType = 'ping' | 'md_connect' | 'md_disconnect'
  | 'subscribe_log' | 'unsubscribe_log' | 'query_md_subscriptions'

export interface WsClientMessage { type: WsClientMessageType; seq?: number; payload?: Record<string, unknown> }