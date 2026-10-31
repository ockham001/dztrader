// ===== WebSocket 消息类型（服务端 → 客户端 / 客户端 → 服务端）=====
// 自 composables/useWebSocket.ts 的 ServerMessage 迁入并扩展（P3 Task 1）
// 判别联合：data 类型经 WsDataByType 按 type 映射；真源 = 契约单源（schema → types/generated.ts）。
import type {
  ProcessStatusPayload, ProgressStatus, LogConfigPayload, AutoLoginPayload, NotifyUiPayload,
  MdRtnConfigPayload, MdRtnStatusPayload, ShmConfigView, LogLine,
  SysmonSnapshotPayload, SysmonConfigPayload,
} from './generated'

// 服务端 → 客户端每类消息的 data 载荷类型（判别联合核心）
// 已收敛的用生成领域类型；未迁移的复杂域暂以 unknown，随 P1 迁移逐步收敛。
// 注：`*_ack` 为控制消息（契约 webui-ws §2.4，payload 键），条目仅用于 type 全集完整性，
// 前端当前不注册 ack handler（业务结果由领域消息/镜像推送表达，契约 §2.4）。
export interface WsDataByType {
  snapshot: unknown                              // 结构特殊：Record<instance_id, Partial<DomainPayloads>>（镜像）
  process_status: ProcessStatusPayload
  process_config: Record<string, unknown>
  md_rtn_config: MdRtnConfigPayload
  md_rtn_status: MdRtnStatusPayload
  md_rtn_subscriptions: unknown                  // 待迁（契约 md-subscription）
  notify_ui: NotifyUiPayload
  log_config: LogConfigPayload
  log_line: { file?: string; line?: LogLine }    // 契约 webui-ws log_line（file + 单条 line）
  log_tail_unsubscribed: unknown
  data_changed: unknown
  default_password_warning: { message: string }  // 契约 webui-ws §2.3：文案由后端下发
  error: { message: string }
  pong: void
  event_shm_config: ShmConfigView
  md_shm_config: ShmConfigView
  auto_login: AutoLoginPayload
  progress: ProgressStatus
  // 契约 sysmon: 快照域（固定实例 dzsysmon）与运行期配置，前端消费随资源监控页票接入
  sysmon_snapshot: SysmonSnapshotPayload
  sysmon_config: SysmonConfigPayload
  // 契约 webui-ws §2.4 控制消息 ACK（payload 键）
  md_connect_ack: { source: string; ok: boolean }
  md_disconnect_ack: { source: string; ok: boolean }
  subscribe_log_ack: { file: string }
  unsubscribe_log_ack: Record<string, never>
  query_md_subscriptions_ack: { source: string; ok: boolean }
}

// 服务端 → 客户端消息 type 联合（= WsDataByType 的 key）
export type WsServerMessageType = keyof WsDataByType

// 按 type 分布的真判别联合：值层面 `msg.type === 'x'` 才能把 msg.data 收窄到
// WsDataByType['x']。若写成单一 interface + 泛型默认值，data 类型是联合索引
// （含 unknown 键）→ 收窄失效。
export type WsServerMessage<T extends WsServerMessageType = WsServerMessageType> =
  T extends WsServerMessageType
    ? {
        type: T
        data?: WsDataByType[T]   // 领域消息（data 键）
        payload?: unknown        // 控制消息（payload 键）
        instance_id?: string
        seq?: number
      }
    : never

// 客户端 → 服务端
export type WsClientMessageType = 'ping' | 'md_connect' | 'md_disconnect'
  | 'subscribe_log' | 'unsubscribe_log' | 'query_md_subscriptions'

export interface WsClientMessage { type: WsClientMessageType; seq?: number; payload?: Record<string, unknown> }
