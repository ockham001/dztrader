// ===== WebSocket 消息类型（服务端 → 客户端 / 客户端 → 服务端）=====
// 自 composables/useWebSocket.ts 的 ServerMessage 迁入并扩展（P3 Task 1）

// 服务端 → 客户端消息（type 联合 + 每类 payload）
// P6: rtn_process_control 已移除——后端不推送该消息（帧 114 已删，契约 10 §5），
// 进程操作 pending 由 process_status 帧的 event 字段清理（契约 04）
export type WsServerMessageType =
  | 'snapshot' | 'process_status' | 'process_config'
  | 'md_rtn_config' | 'md_rtn_status' | 'md_rtn_subscriptions'
  | 'notify_ui' | 'log_config' | 'log_line' | 'log_tail_unsubscribed'
  | 'data_changed' | 'default_password_warning' | 'error' | 'pong'
  | 'event_shm_config' | 'md_shm_config' | 'auto_login' | 'progress'   // P2 新增

export interface WsServerMessage<T extends WsServerMessageType = WsServerMessageType> {
  type: T
  data?: unknown        // 领域消息（P1 统一 data 键）
  payload?: unknown     // 控制消息（payload 键）
  instance_id?: string
}

// 客户端 → 服务端
export type WsClientMessageType = 'ping' | 'md_connect' | 'md_disconnect'
  | 'subscribe_log' | 'unsubscribe_log' | 'query_md_subscriptions'

export interface WsClientMessage { type: WsClientMessageType; seq?: number; payload?: Record<string, unknown> }
